/**
 * @file at.h
 * @brief Zero-malloc AT command engine for embedded GSM/LTE modems.
 *
 * Architecture
 * ============
 *   ┌─────────────────────────────────────────────────────────┐
 *   │                    Application / GSM layer              │
 *   └──────────────┬───────────────────────────┬─────────────┘
 *                  │  at_send() / at_send_raw() │ at_register_urc()
 *   ┌──────────────▼───────────────────────────▼─────────────┐
 *   │                    AT Engine  (at.c)                    │
 *   │  cmd_queue  ·  rx_ringbuf  ·  line_fsm  ·  urc_table   │
 *   └──────────────┬───────────────────────────┬─────────────┘
 *                  │  at_platform_write()       │ at_feed()
 *   ┌──────────────▼───────────────────────────▼─────────────┐
 *   │              Platform HAL  (user-provided)              │
 *   │  UART ISR → at_feed()    ·    at_platform_write()       │
 *   └─────────────────────────────────────────────────────────┘
 *
 * Thread / interrupt safety
 * =========================
 *   at_feed() is designed to be called from an ISR or DMA callback.
 *   All other API functions must be called from a single task / main loop.
 *   The only shared state between ISR and task contexts is the RX ring
 *   buffer, protected by a 1-byte volatile head/tail pair (lock-free SPSC).
 *
 *   Summary table:
 *
 *   Function / facility       │ ISR-safe │ Notes
 *   ──────────────────────────┼──────────┼─────────────────────────────────
 *   at_feed()                 │   YES    │ Lock-free SPSC ring buffer write
 *   at_tick()                 │   YES    │ Single volatile decrement
 *   at_process()              │   NO     │ Call from one task only
 *   at_send() / at_send_raw() │   NO     │ Modifies cmd queue; task context
 *   at_register_urc()         │   NO     │ Modifies URC table; task context
 *   at_abort() / at_reset()   │   NO     │ Modifies engine state; task context
 *   at_platform_write()       │   NO     │ Called by engine from task context
 *
 *   RTOS usage:
 *   - Wire the UART RX ISR/DMA callback to at_feed().  No mutex needed.
 *   - Call at_tick() from a SysTick ISR or a high-priority 1 ms timer task.
 *   - Call at_process() from a single dedicated AT task or your main loop.
 *   - Protect concurrent at_send() calls with a mutex if multiple tasks
 *     must enqueue commands; at_send() itself is NOT re-entrant.
 *
 * Usage skeleton
 * ==============
 *   1.  Implement at_platform_write() in your BSP.
 *   2.  Call at_init() once.
 *   3.  Wire UART RX byte/DMA callback → at_feed().
 *   4.  Call at_tick(ms_elapsed) from your 1 ms SysTick handler.
 *   5.  Call at_process() in your main loop.
 *   6.  Issue commands via at_send() or the at_gsm_*() helpers.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AT_H
#define AT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "at_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Result codes
 * ========================================================================= */

typedef enum {
    AT_OK            =  0,  /**< Command completed with OK              */
    AT_ERR_CME      =  1,  /**< +CME ERROR: <n>                        */
    AT_ERR_CMS      =  2,  /**< +CMS ERROR: <n>                        */
    AT_ERR_GENERIC  =  3,  /**< Plain ERROR response                   */
    AT_ERR_TIMEOUT  =  4,  /**< No final result within timeout          */
    AT_ERR_BUSY     =  5,  /**< Command queue full                      */
    AT_ERR_PARAM    =  6,  /**< Bad argument to API call                */
    AT_ERR_OVERFLOW =  7,  /**< Response buffer overflowed              */
    AT_ERR_ABORTED  =  8,  /**< Command aborted by at_abort()           */
    AT_ERR_IO       =  9,  /**< at_platform_write() reported a failure  */
    AT_PENDING      = 99,  /**< Internal: command in progress           */
} at_result_t;

/* =========================================================================
 * Response container
 * ========================================================================= */

/**
 * @brief Accumulated response for a completed command.
 *
 * All storage is inside the engine's static pool; the caller must
 * not cache pointers beyond the callback/polling call that delivers it.
 */
typedef struct {
    at_result_t  status;                            /**< Final result code  */
    int32_t      error_code;                        /**< CME/CMS error num  */
    uint8_t      num_lines;                         /**< Payload line count */
    const char  *lines[AT_CFG_RESP_LINES_MAX];      /**< Pointers into pool */
} at_response_t;

/* =========================================================================
 * Callbacks
 * ========================================================================= */

/**
 * @brief Command completion callback.
 * @param resp   Pointer to the completed response (valid only during call).
 * @param user   Opaque pointer passed to at_send().
 */
typedef void (*at_cb_t)(const at_response_t *resp, void *user);

/**
 * @brief URC (Unsolicited Result Code) handler.
 * @param line   NUL-terminated URC line (e.g. "+CREG: 1").
 * @param user   Opaque pointer registered with at_register_urc().
 */
typedef void (*at_urc_cb_t)(const char *line, void *user);

/* =========================================================================
 * Command descriptor
 * ========================================================================= */

/**
 * @brief Intermediate prompt handling mode.
 *
 * Some commands (AT+CMGS, AT+CMGW) pause after sending the command string
 * and wait for a "> " prompt before the payload is transmitted.
 */
typedef enum {
    AT_PROMPT_NONE = 0, /**< No intermediate prompt expected            */
    AT_PROMPT_SMS,      /**< Wait for "> ", then send body + Ctrl-Z     */
} at_prompt_mode_t;

/**
 * @brief Descriptor for a command to enqueue.
 *
 * Callers fill this struct and pass it to at_send().  The cmd string is
 * copied into the engine's TX buffer so the caller's buffer may be on the
 * stack.
 */
typedef struct {
    const char       *cmd;              /**< Command string (no \\r\\n needed)  */
    const char       *body;             /**< Optional payload (SMS text, etc.)  */
    uint32_t          timeout_ms;       /**< 0 → AT_CFG_DEFAULT_TIMEOUT_MS      */
    at_prompt_mode_t  prompt;           /**< Intermediate prompt handling        */
    at_cb_t           cb;              /**< Completion callback (may be NULL)   */
    void             *user;            /**< Passed through to cb                */
} at_cmd_desc_t;

/* =========================================================================
 * Engine state (opaque public view)
 * ========================================================================= */

/** Engine phases — exposed so callers can poll if not using callbacks. */
typedef enum {
    AT_STATE_IDLE      = 0,
    AT_STATE_SENDING,
    AT_STATE_WAITING,       /**< Waiting for final result / prompt       */
    AT_STATE_PROMPT,        /**< Got "> ", sending body                  */
    AT_STATE_COMPLETE,      /**< Last command finished, not yet dequeued */
} at_state_t;

/* =========================================================================
 * Platform interface — implement in your BSP
 * ========================================================================= */

/**
 * @brief Write bytes to the modem UART.
 *
 * May be blocking or DMA-backed.  The engine never calls this from an ISR.
 *
 * @param data    Bytes to transmit.
 * @param len     Number of bytes.
 * @return        Number of bytes actually written (must equal len on success).
 */
extern size_t at_platform_write(const uint8_t *data, size_t len);

/* =========================================================================
 * Core engine API
 * =========================================================================
 *
 * Thread-safety notes
 * -------------------
 * The engine is designed for a single-producer / single-consumer (SPSC)
 * ISR ↔ task split.  The rules are:
 *
 *  • at_feed()   — ISR-SAFE.  Uses a lock-free SPSC ring buffer (volatile
 *                  head/tail with a compiler barrier).  Call freely from any
 *                  interrupt priority.
 *
 *  • at_tick()   — ISR-SAFE.  Performs a single volatile decrement.  Call
 *                  from SysTick or a high-priority RTOS timer task.
 *
 *  • at_process() — NOT ISR-SAFE.  Must be called from exactly one task or
 *                   the main loop.  Never call from multiple tasks concurrently.
 *
 *  • at_send() / at_send_raw() — NOT ISR-SAFE.  Modifies the command queue.
 *    If multiple tasks need to enqueue commands, guard all at_send*() calls
 *    with a mutex; at_process() must still run from a single task.
 *
 *  • at_register_urc() / at_deregister_urc() — NOT ISR-SAFE.  Call before
 *    starting at_process() or hold the same mutex as at_send().
 *
 *  • at_abort() / at_reset() — NOT ISR-SAFE.  Task context only.
 * ========================================================================= */

/**
 * @brief Initialise the AT engine.  Call once before any other function.
 */
void at_init(void);

/**
 * @brief Reset engine to idle state and flush all queued commands.
 *
 * Useful after a modem reset.  Pending callbacks receive AT_ERR_ABORTED.
 */
void at_reset(void);

/**
 * @brief Feed received bytes into the engine (call from UART ISR / DMA cb).
 *
 * Lock-free SPSC — safe to call from interrupt context.
 *
 * @param data   Received bytes.
 * @param len    Number of bytes.
 */
void at_feed(const uint8_t *data, size_t len);

/**
 * @brief Advance engine timers.  Call from your 1 ms SysTick ISR or RTOS timer.
 *
 * @param ms_elapsed  Milliseconds elapsed since last call.
 */
void at_tick(uint32_t ms_elapsed);

/**
 * @brief Drive the engine state machine.  Call as often as possible in main loop.
 *
 * Processes buffered RX data, dispatches URCs, completes commands.
 */
void at_process(void);

/**
 * @brief Enqueue a command for transmission.
 *
 * @param desc   Command descriptor.  Strings are copied internally.
 * @return       AT_OK on success, AT_ERR_BUSY if queue is full, AT_ERR_PARAM on bad args.
 */
at_result_t at_send(const at_cmd_desc_t *desc);

/**
 * @brief Convenience wrapper — send a raw command string with a callback.
 *
 * @param cmd         Command string.
 * @param timeout_ms  0 → use default.
 * @param cb          Completion callback (may be NULL).
 * @param user        Opaque user pointer.
 * @return            AT_OK or AT_ERR_BUSY / AT_ERR_PARAM.
 */
at_result_t at_send_raw(const char *cmd, uint32_t timeout_ms, at_cb_t cb, void *user);

/**
 * @brief Abort the currently executing command (if any).
 *
 * The active command's callback receives AT_ERR_ABORTED.
 */
void at_abort(void);

/**
 * @brief Register a URC handler.
 *
 * The engine scans every non-response line against registered prefixes.
 * Registration is checked in order; first match wins.
 *
 * @param prefix  URC prefix to match (e.g. "+CREG", "RING").
 * @param cb      Handler function.
 * @param user    Opaque pointer passed to cb.
 * @return        AT_OK or AT_ERR_BUSY (table full).
 */
at_result_t at_register_urc(const char *prefix, at_urc_cb_t cb, void *user);

/**
 * @brief Deregister a previously registered URC handler.
 *
 * @param prefix  Same string passed to at_register_urc().
 * @return        AT_OK or AT_ERR_PARAM (not found).
 */
at_result_t at_deregister_urc(const char *prefix);

/**
 * @brief Query the current engine state.
 */
at_state_t at_state(void);

/**
 * @brief Return number of commands currently in the queue (including active).
 */
uint8_t at_queue_depth(void);

/**
 * @brief Return a human-readable string for an at_result_t code.
 */
const char *at_result_str(at_result_t r);

/**
 * @brief Trace hook callback — called for every byte written to / read from the modem.
 *
 * @param dir   'T' = transmit (engine → modem), 'R' = receive (modem → engine).
 * @param data  Raw bytes.
 * @param len   Number of bytes.
 * @param user  Opaque pointer registered with at_set_trace_hook().
 */
typedef void (*at_trace_cb_t)(char dir, const uint8_t *data, size_t len, void *user);

/**
 * @brief Register a trace hook to observe all modem traffic.
 *
 * Useful for logging, protocol analysers, or test spies.  Pass NULL to
 * disable tracing.  Only one hook can be active at a time.
 *
 * The hook is called from the same context as at_process() (task context,
 * NOT ISR-safe).  Keep the callback short — do not call any at_*() functions
 * from inside the trace callback.
 *
 * @param cb    Trace callback (or NULL to disable).
 * @param user  Opaque pointer passed to every invocation.
 */
void at_set_trace_hook(at_trace_cb_t cb, void *user);

/**
 * @brief De-initialise the AT engine and release logical resources.
 *
 * Aborts any queued commands (callbacks receive AT_ERR_ABORTED), clears the
 * URC table, and removes the trace hook.  After this call the engine is in
 * the same state as before at_init() was called; call at_init() again to
 * reuse the engine (e.g. after a full modem power cycle).
 *
 * This function does NOT call any platform HAL — it will not close a UART
 * or release a mutex.  Platform teardown is the caller's responsibility.
 */
void at_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* AT_H */
