/**
 * @file at.c
 * @brief Zero-malloc AT command engine implementation.
 *
 * SPDX-License-Identifier: MIT
 */

#include "at.h"
#include "at_fmt.h"

#include <string.h>
#include <stddef.h>
#include <stdbool.h>

/* =========================================================================
 * Internal macros
 * ========================================================================= */

#define AT_MASK(n)        ((n) - 1U)          /* ring-buffer index mask      */
#define AT_MIN(a, b)      ((a) < (b) ? (a) : (b))
#define AT_ARRAY_SIZE(a)  (sizeof(a) / sizeof((a)[0]))

/* =========================================================================
 * RX ring buffer  (lock-free SPSC)
 * =========================================================================
 *
 * head  → written by ISR (producer)
 * tail  → read   by at_process() (consumer)
 *
 * Both indices are uint16_t and wrap naturally; no masking needed for
 * overflow detection.  Actual index into storage = index & MASK.
 */
typedef struct {
    volatile uint16_t  head;                        /* producer writes */
    volatile uint16_t  tail;                        /* consumer reads  */
    uint8_t            buf[AT_CFG_RX_BUF_SIZE];
} rxring_t;

static inline bool     rxring_empty(const rxring_t *r) { return r->head == r->tail; }
static inline uint16_t rxring_used(const rxring_t *r)  { return (uint16_t)(r->head - r->tail); }

static inline bool rxring_push(rxring_t *r, uint8_t b)
{
    if (rxring_used(r) >= AT_CFG_RX_BUF_SIZE) return false;
    r->buf[r->head & AT_MASK(AT_CFG_RX_BUF_SIZE)] = b;
    /* compiler barrier before head increment so byte is visible first */
    __asm__ volatile ("" ::: "memory");
    r->head++;
    return true;
}

static inline bool rxring_pop(rxring_t *r, uint8_t *out)
{
    if (rxring_empty(r)) return false;
    *out = r->buf[r->tail & AT_MASK(AT_CFG_RX_BUF_SIZE)];
    __asm__ volatile ("" ::: "memory");
    r->tail++;
    return true;
}

/* =========================================================================
 * Response line pool
 * =========================================================================
 *
 * A flat pool of fixed-size slots.  at_response_t::lines[] are pointers
 * into this pool.  The pool is reused for each command; it is safe because
 * the caller must not hold onto pointers after the callback returns.
 */
typedef struct {
    char    data[AT_CFG_RESP_LINES_MAX][AT_CFG_LINE_MAX];
    uint8_t count;
} resp_pool_t;

static bool resp_pool_append(resp_pool_t *p, const char *line, size_t len)
{
    if (p->count >= AT_CFG_RESP_LINES_MAX) return false;
    if (len >= AT_CFG_LINE_MAX) len = AT_CFG_LINE_MAX - 1U;
    memcpy(p->data[p->count], line, len);
    p->data[p->count][len] = '\0';
    p->count++;
    return true;
}

/* =========================================================================
 * Command queue entry
 * ========================================================================= */

#define AT_CMD_STR_MAX  AT_CFG_TX_BUF_SIZE
#define AT_BODY_MAX     (AT_CFG_TX_BUF_SIZE * 2U)

typedef struct {
    char              cmd[AT_CMD_STR_MAX];
    char              body[AT_BODY_MAX];
    uint32_t          timeout_ms;
    at_prompt_mode_t  prompt;
    at_cb_t           cb;
    void             *user;
    bool              has_body;
} cmd_entry_t;

/* =========================================================================
 * URC table entry
 * ========================================================================= */

typedef struct {
    char         prefix[AT_CFG_LINE_MAX];
    at_urc_cb_t  cb;
    void        *user;
    bool         active;
} urc_entry_t;

/* =========================================================================
 * Engine state
 * ========================================================================= */

typedef enum {
    LFSM_START = 0,    /* waiting for first char of line                */
    LFSM_DATA,         /* accumulating line data                        */
    LFSM_CR,           /* saw \r, waiting for \n                        */
} line_fsm_t;

typedef struct {
    /* RX ring buffer */
    rxring_t    rx;

    /* Command queue (simple circular array) */
    cmd_entry_t  queue[AT_CFG_CMD_QUEUE_DEPTH];
    uint8_t      q_head;                    /* next slot to write         */
    uint8_t      q_tail;                    /* next slot to execute       */
    uint8_t      q_count;

    /* Active command tracking */
    at_state_t   state;
    uint32_t     timer_ms;                  /* countdown to timeout       */
    resp_pool_t  pool;                      /* accumulated response lines */
    at_response_t response;                 /* built up during exec       */

    /* Line accumulation FSM */
    line_fsm_t   lfsm;
    char         line_buf[AT_CFG_LINE_MAX];
    uint16_t     line_len;

    /* URC dispatch table */
    urc_entry_t  urc_table[AT_CFG_URC_TABLE_SIZE];

    /* Echo cancellation scratch */
    char         echo_expect[AT_CMD_STR_MAX];
    uint16_t     echo_len;
    bool         echo_armed;
} at_engine_t;

/* Single static engine instance — the only allocation in this library. */
static at_engine_t g_at;

/* =========================================================================
 * Forward declarations
 * ========================================================================= */

static void     engine_dispatch_line(const char *line, uint16_t len);
static void     engine_finalize(at_result_t status, int32_t err_code);
static void     engine_start_next(void);
static bool     engine_is_final(const char *line, uint16_t len,
                                at_result_t *status, int32_t *err_code);
static bool     urc_dispatch(const char *line);
static uint16_t str_copy_safe(char *dst, size_t dst_size, const char *src);

/* =========================================================================
 * Public API — init / reset
 * ========================================================================= */

void at_init(void)
{
    memset(&g_at, 0, sizeof(g_at));
    g_at.state = AT_STATE_IDLE;
}

void at_reset(void)
{
    /* Abort active command */
    if (g_at.state != AT_STATE_IDLE && g_at.q_count > 0U) {
        engine_finalize(AT_ERR_ABORTED, 0);
    }
    /* Drain remaining queue */
    while (g_at.q_count > 0U) {
        cmd_entry_t *e = &g_at.queue[g_at.q_tail];
        if (e->cb) {
            at_response_t r = {
                .status     = AT_ERR_ABORTED,
                .error_code = 0,
                .num_lines  = 0,
            };
            e->cb(&r, e->user);
        }
        g_at.q_tail = (uint8_t)((g_at.q_tail + 1U) % AT_CFG_CMD_QUEUE_DEPTH);
        g_at.q_count--;
    }
    /* Reset volatile ring-buffer heads atomically */
    g_at.rx.head = g_at.rx.tail = 0U;
    g_at.state   = AT_STATE_IDLE;
    g_at.lfsm    = LFSM_START;
    g_at.line_len = 0U;
    g_at.echo_armed = false;
    memset(&g_at.pool, 0, sizeof(g_at.pool));
}

/* =========================================================================
 * Public API — ISR-safe feed
 * ========================================================================= */

void at_feed(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (!rxring_push(&g_at.rx, data[i])) {
            AT_CFG_LOG(1, "RX ring overflow — byte dropped");
            /* On overflow we drop the byte; the engine will eventually
             * time out the active command. */
        }
    }
}

/* =========================================================================
 * Public API — tick (called from SysTick / timer ISR)
 * ========================================================================= */

void at_tick(uint32_t ms_elapsed)
{
    if (g_at.state == AT_STATE_WAITING || g_at.state == AT_STATE_PROMPT) {
        if (g_at.timer_ms <= ms_elapsed) {
            g_at.timer_ms = 0U;
            /* Flag timeout; at_process() will handle it next call */
        } else {
            g_at.timer_ms -= ms_elapsed;
        }
    }
}

/* =========================================================================
 * Public API — main process loop
 * ========================================================================= */

void at_process(void)
{
    /* ---------- 1. Drain RX ring into line FSM ---------- */
    uint8_t b;
    while (rxring_pop(&g_at.rx, &b)) {
        switch (g_at.lfsm) {
        case LFSM_START:
            if (b == '\r' || b == '\n') break;   /* skip leading whitespace */
            g_at.line_buf[0] = (char)b;
            g_at.line_len    = 1U;
            g_at.lfsm        = LFSM_DATA;
            break;

        case LFSM_DATA:
            if (b == '\r') {
                g_at.lfsm = LFSM_CR;
            } else if (b == '\n') {
                /* LF without preceding CR — treat as line end */
                g_at.line_buf[g_at.line_len] = '\0';
                engine_dispatch_line(g_at.line_buf, g_at.line_len);
                g_at.line_len = 0U;
                g_at.lfsm     = LFSM_START;
            } else {
                if (g_at.line_len < AT_CFG_LINE_MAX - 1U) {
                    g_at.line_buf[g_at.line_len++] = (char)b;
                } else {
                    AT_CFG_LOG(1, "line overflow — truncating");
                    /* Keep consuming until CRLF to re-sync */
                }
            }
            break;

        case LFSM_CR:
            /* Expect \n; handle "> " prompt (bare \r then space) */
            if (b == '\n') {
                g_at.line_buf[g_at.line_len] = '\0';
                engine_dispatch_line(g_at.line_buf, g_at.line_len);
                g_at.line_len = 0U;
                g_at.lfsm     = LFSM_START;
            } else if (b == ' ' && g_at.line_len == 1U && g_at.line_buf[0] == '>') {
                /* "> " prompt: two chars, no trailing \n */
                g_at.line_buf[1] = ' ';
                g_at.line_buf[2] = '\0';
                engine_dispatch_line(g_at.line_buf, 2U);
                g_at.line_len = 0U;
                g_at.lfsm     = LFSM_START;
            } else {
                /* Unexpected byte after \r — re-enter DATA */
                if (g_at.line_len < AT_CFG_LINE_MAX - 1U)
                    g_at.line_buf[g_at.line_len++] = (char)b;
                g_at.lfsm = LFSM_DATA;
            }
            break;
        }
    }

    /* ---------- 2. Check for timeout ---------- */
    if ((g_at.state == AT_STATE_WAITING || g_at.state == AT_STATE_PROMPT)
            && g_at.timer_ms == 0U) {
        AT_CFG_LOG(1, "command timeout: %s",
                   g_at.q_count ? g_at.queue[g_at.q_tail].cmd : "?");
        engine_finalize(AT_ERR_TIMEOUT, 0);
    }

    /* ---------- 3. Start next command if idle ---------- */
    if (g_at.state == AT_STATE_IDLE && g_at.q_count > 0U) {
        engine_start_next();
    }
}

/* =========================================================================
 * Public API — send
 * ========================================================================= */

at_result_t at_send(const at_cmd_desc_t *desc)
{
    if (!desc || !desc->cmd || desc->cmd[0] == '\0') return AT_ERR_PARAM;
    if (g_at.q_count >= AT_CFG_CMD_QUEUE_DEPTH)      return AT_ERR_BUSY;

    cmd_entry_t *e = &g_at.queue[g_at.q_head];
    memset(e, 0, sizeof(*e));

    str_copy_safe(e->cmd, sizeof(e->cmd), desc->cmd);
    e->timeout_ms = desc->timeout_ms ? desc->timeout_ms : (uint16_t)AT_CFG_DEFAULT_TIMEOUT_MS;
    e->prompt     = desc->prompt;
    e->cb         = desc->cb;
    e->user       = desc->user;

    if (desc->body && desc->body[0] != '\0') {
        str_copy_safe(e->body, sizeof(e->body), desc->body);
        e->has_body = true;
    }

    g_at.q_head = (uint8_t)((g_at.q_head + 1U) % AT_CFG_CMD_QUEUE_DEPTH);
    g_at.q_count++;
    AT_CFG_LOG(2, "enqueued: %s (depth=%u)", e->cmd, g_at.q_count);
    return AT_OK;
}

at_result_t at_send_raw(const char *cmd, uint32_t timeout_ms, at_cb_t cb, void *user)
{
    at_cmd_desc_t d = {
        .cmd        = cmd,
        .body       = NULL,
        .timeout_ms = timeout_ms,
        .prompt     = AT_PROMPT_NONE,
        .cb         = cb,
        .user       = user,
    };
    return at_send(&d);
}

/* =========================================================================
 * Public API — abort
 * ========================================================================= */

void at_abort(void)
{
    if (g_at.state == AT_STATE_WAITING || g_at.state == AT_STATE_PROMPT) {
        engine_finalize(AT_ERR_ABORTED, 0);
    }
}

/* =========================================================================
 * Public API — URC management
 * ========================================================================= */

at_result_t at_register_urc(const char *prefix, at_urc_cb_t cb, void *user)
{
    if (!prefix || !cb) return AT_ERR_PARAM;

    /* Reuse existing slot if prefix already registered */
    for (uint8_t i = 0; i < AT_CFG_URC_TABLE_SIZE; i++) {
        urc_entry_t *u = &g_at.urc_table[i];
        if (u->active && strncmp(u->prefix, prefix, sizeof(u->prefix) - 1U) == 0) {
            u->cb   = cb;
            u->user = user;
            return AT_OK;
        }
    }
    /* Find free slot */
    for (uint8_t i = 0; i < AT_CFG_URC_TABLE_SIZE; i++) {
        urc_entry_t *u = &g_at.urc_table[i];
        if (!u->active) {
            str_copy_safe(u->prefix, sizeof(u->prefix), prefix);
            u->cb     = cb;
            u->user   = user;
            u->active = true;
            return AT_OK;
        }
    }
    return AT_ERR_BUSY;
}

at_result_t at_deregister_urc(const char *prefix)
{
    if (!prefix) return AT_ERR_PARAM;
    for (uint8_t i = 0; i < AT_CFG_URC_TABLE_SIZE; i++) {
        urc_entry_t *u = &g_at.urc_table[i];
        if (u->active && strncmp(u->prefix, prefix, sizeof(u->prefix) - 1U) == 0) {
            memset(u, 0, sizeof(*u));
            return AT_OK;
        }
    }
    return AT_ERR_PARAM;
}

/* =========================================================================
 * Public API — query
 * ========================================================================= */

at_state_t at_state(void)        { return g_at.state;   }
uint8_t    at_queue_depth(void)  { return g_at.q_count; }

const char *at_result_str(at_result_t r)
{
    switch (r) {
    case AT_OK:            return "OK";
    case AT_ERR_CME:       return "CME ERROR";
    case AT_ERR_CMS:       return "CMS ERROR";
    case AT_ERR_GENERIC:   return "ERROR";
    case AT_ERR_TIMEOUT:   return "TIMEOUT";
    case AT_ERR_BUSY:      return "BUSY";
    case AT_ERR_PARAM:     return "PARAM";
    case AT_ERR_OVERFLOW:  return "OVERFLOW";
    case AT_ERR_ABORTED:   return "ABORTED";
    case AT_ERR_IO:        return "IO ERROR";
    case AT_PENDING:       return "PENDING";
    default:               return "UNKNOWN";
    }
}

/* =========================================================================
 * Internal — start next queued command
 * ========================================================================= */

static void engine_start_next(void)
{
    cmd_entry_t *e = &g_at.queue[g_at.q_tail];

    /* Clear response pool */
    memset(&g_at.pool, 0, sizeof(g_at.pool));
    memset(&g_at.response, 0, sizeof(g_at.response));
    g_at.response.status = AT_PENDING;

    /* Arm echo cancellation */
#if AT_CFG_ECHO_CANCEL
    g_at.echo_len    = (uint16_t)str_copy_safe(g_at.echo_expect, sizeof(g_at.echo_expect), e->cmd);
    g_at.echo_armed  = true;
#else
    g_at.echo_armed  = false;
#endif

    /* Set timeout */
    g_at.timer_ms = e->timeout_ms;

    /* Transmit command + \r; abort with AT_ERR_IO if the HAL reports failure */
    g_at.state = AT_STATE_SENDING;
    size_t cmd_len = strlen(e->cmd);
    if (at_platform_write((const uint8_t *)e->cmd, cmd_len) != cmd_len ||
        at_platform_write((const uint8_t *)"\r", 1U) != 1U)
    {
        engine_finalize(AT_ERR_IO, 0);
        return;
    }

    g_at.state = AT_STATE_WAITING;
    AT_CFG_LOG(2, "sent: %s", e->cmd);
}

/* =========================================================================
 * Internal — dispatch a fully assembled line
 * ========================================================================= */

static void engine_dispatch_line(const char *line, uint16_t len)
{
    if (len == 0U) return;

    AT_CFG_LOG(2, "rx_line: '%s'", line);

    /* --- Echo cancellation --- */
#if AT_CFG_ECHO_CANCEL
    if (g_at.echo_armed && g_at.state == AT_STATE_WAITING) {
        if (strncmp(line, g_at.echo_expect, (size_t)g_at.echo_len) == 0) {
            g_at.echo_armed = false;
            return;   /* discard echo */
        }
    }
#endif

    /* --- "> " prompt --- */
    if (g_at.state == AT_STATE_WAITING &&
        len >= 2U && line[0] == '>' && line[1] == ' ')
    {
        cmd_entry_t *e = &g_at.queue[g_at.q_tail];
        if (e->prompt == AT_PROMPT_SMS && e->has_body) {
            g_at.state    = AT_STATE_PROMPT;
            g_at.timer_ms = AT_CFG_PROMPT_TIMEOUT_MS;
            /* Send body + Ctrl-Z; abort with AT_ERR_IO if HAL fails */
            size_t body_len = strlen(e->body);
            if (at_platform_write((const uint8_t *)e->body, body_len) != body_len ||
                at_platform_write((const uint8_t *)"\x1A", 1U) != 1U)
            {
                engine_finalize(AT_ERR_IO, 0);
                return;
            }
            g_at.state = AT_STATE_WAITING;
        }
        return;
    }

    /* --- Check for final result code --- */
    at_result_t status;
    int32_t     err_code;
    if (g_at.state == AT_STATE_WAITING &&
        engine_is_final(line, len, &status, &err_code))
    {
        engine_finalize(status, err_code);
        return;
    }

    /* --- Check for URC (even while waiting) --- */
    if (urc_dispatch(line)) return;

    /* --- Accumulate response line --- */
    if (g_at.state == AT_STATE_WAITING) {
        if (!resp_pool_append(&g_at.pool, line, (size_t)len)) {
            AT_CFG_LOG(1, "response pool full — line dropped");
            g_at.response.status = AT_ERR_OVERFLOW;
        }
    }
}

/* =========================================================================
 * Internal — check if line is a final result code
 * ========================================================================= */

static bool engine_is_final(const char *line, uint16_t len,
                             at_result_t *status, int32_t *err_code)
{
    (void)len;
    *err_code = 0;

    if (strcmp(line, "OK") == 0) {
        *status = AT_OK;
        return true;
    }
    if (strcmp(line, "ERROR") == 0) {
        *status = AT_ERR_GENERIC;
        return true;
    }
    if (strcmp(line, "NO CARRIER") == 0 ||
        strcmp(line, "NO DIALTONE") == 0 ||
        strcmp(line, "BUSY") == 0 ||
        strcmp(line, "NO ANSWER") == 0) {
        *status = AT_ERR_GENERIC;
        return true;
    }
    /* +CME ERROR: <n> */
    if (strncmp(line, "+CME ERROR:", 11) == 0) {
        const char *p = line + 11;
        *status   = AT_ERR_CME;
        *err_code = at__parse_int(&p);
        return true;
    }
    /* +CMS ERROR: <n> */
    if (strncmp(line, "+CMS ERROR:", 11) == 0) {
        const char *p = line + 11;
        *status   = AT_ERR_CMS;
        *err_code = at__parse_int(&p);
        return true;
    }
    return false;
}

/* =========================================================================
 * Internal — finalize active command
 * ========================================================================= */

static void engine_finalize(at_result_t status, int32_t err_code)
{
    /* Build response struct pointing into pool */
    g_at.response.status     = status;
    g_at.response.error_code = err_code;
    g_at.response.num_lines  = g_at.pool.count;
    for (uint8_t i = 0; i < g_at.pool.count; i++) {
        g_at.response.lines[i] = g_at.pool.data[i];
    }

    cmd_entry_t *e = &g_at.queue[g_at.q_tail];
    AT_CFG_LOG(2, "finalized: %s → %s (err=%ld)",
               e->cmd, at_result_str(status), (long)err_code);

    /* Fire callback before dequeuing so cb can re-enqueue follow-ups */
    if (e->cb) {
        e->cb(&g_at.response, e->user);
    }

    /* Dequeue */
    memset(e, 0, sizeof(*e));
    g_at.q_tail  = (uint8_t)((g_at.q_tail + 1U) % AT_CFG_CMD_QUEUE_DEPTH);
    g_at.q_count--;

    g_at.state     = AT_STATE_IDLE;
    g_at.timer_ms  = 0U;
    g_at.echo_armed = false;
}

/* =========================================================================
 * Internal — URC dispatch
 * ========================================================================= */

static bool urc_dispatch(const char *line)
{
    for (uint8_t i = 0; i < AT_CFG_URC_TABLE_SIZE; i++) {
        const urc_entry_t *u = &g_at.urc_table[i];
        if (!u->active) continue;
        size_t plen = strlen(u->prefix);
        if (strncmp(line, u->prefix, plen) == 0) {
            AT_CFG_LOG(2, "URC dispatch: %s", u->prefix);
            u->cb(line, u->user);
            return true;
        }
    }
    return false;
}

/* =========================================================================
 * Internal — safe string copy, returns bytes written (excl NUL)
 * ========================================================================= */

static uint16_t str_copy_safe(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0U) return 0U;
    size_t n = strlen(src);
    if (n >= dst_size) n = dst_size - 1U;
    memcpy(dst, src, n);
    dst[n] = '\0';
    return (uint16_t)n;
}
