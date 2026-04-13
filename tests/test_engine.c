/**
 * @file test_engine.c
 * @brief Unit tests for the AT engine core (at.c).
 *
 * The engine uses a single static `g_at` instance.  Each test calls
 * at_init() in setUp() to reset to a known state.
 *
 * at_platform_write() is provided here as a mock that captures the last
 * transmitted command string for assertion.
 *
 * Modem responses are injected synchronously via at_feed() + at_process().
 *
 * SPDX-License-Identifier: MIT
 */

#include "unity.h"
#include "at.h"

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* =========================================================================
 * Platform mock
 * ========================================================================= */

static char   s_tx_buf[256];
static size_t s_tx_len;
static bool   s_write_fail; /* when true, at_platform_write() returns 0 */

size_t at_platform_write(const uint8_t *data, size_t len)
{
    if (s_write_fail) return 0U;
    size_t avail = sizeof(s_tx_buf) - s_tx_len - 1U;
    if (len > avail) len = avail;
    memcpy(s_tx_buf + s_tx_len, data, len);
    s_tx_len += len;
    s_tx_buf[s_tx_len] = '\0';
    return len;
}

/* =========================================================================
 * Test helpers
 * ========================================================================= */

static at_result_t        s_last_status;
static int32_t            s_last_error_code;
static char               s_last_line0[128];
static uint8_t            s_last_num_lines;
static bool               s_cb_called;

static void generic_cb(const at_response_t *resp, void *user)
{
    (void)user;
    s_cb_called       = true;
    s_last_status     = resp->status;
    s_last_error_code = resp->error_code;
    s_last_num_lines  = resp->num_lines;
    if (resp->num_lines > 0 && resp->lines[0]) {
        size_t n = strlen(resp->lines[0]);
        if (n >= sizeof(s_last_line0)) n = sizeof(s_last_line0) - 1U;
        memcpy(s_last_line0, resp->lines[0], n);
        s_last_line0[n] = '\0';
    } else {
        s_last_line0[0] = '\0';
    }
}

/** Inject a NUL-terminated ASCII line (adds \r\n) then call at_process(). */
static void feed_line(const char *line)
{
    at_feed((const uint8_t *)line, strlen(line));
    at_feed((const uint8_t *)"\r\n", 2);
    at_process();
}

/** Feed a raw response sequence as if from the modem. */
static void feed_ok(void)    { feed_line("OK"); }
static void feed_error(void) { feed_line("ERROR"); }

/* =========================================================================
 * setUp / tearDown
 * ========================================================================= */

void setUp(void)
{
    at_init();
    s_cb_called       = false;
    s_last_status     = AT_PENDING;
    s_last_error_code = 0;
    s_last_num_lines  = 0;
    s_last_line0[0]   = '\0';
    s_tx_buf[0]       = '\0';
    s_tx_len          = 0;
    s_write_fail      = false;
}

void tearDown(void) {}

/* =========================================================================
 * Initialisation
 * ========================================================================= */

void test_init_state_is_idle(void)
{
    TEST_ASSERT_EQUAL_INT(AT_STATE_IDLE, at_state());
}

void test_init_queue_depth_is_zero(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, at_queue_depth());
}

/* =========================================================================
 * at_send_raw — basic enqueue
 * ========================================================================= */

void test_send_raw_returns_ok(void)
{
    at_result_t rc = at_send_raw("AT", 0, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
}

void test_send_raw_null_cmd_returns_param_error(void)
{
    at_result_t rc = at_send_raw(NULL, 0, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_ERR_PARAM, rc);
}

void test_send_raw_transmits_cmd_on_process(void)
{
    at_send_raw("AT", 0, NULL, NULL);
    at_process(); /* triggers engine_start_next → at_platform_write */
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT"));
}

void test_send_raw_appends_cr(void)
{
    at_send_raw("ATH", 0, NULL, NULL);
    at_process();
    /* Engine appends \r (V.250 §5.2.1) */
    size_t len = strlen(s_tx_buf);
    TEST_ASSERT_GREATER_THAN(0, len);
    TEST_ASSERT_EQUAL_CHAR('\r', s_tx_buf[len - 1]);
}

/* =========================================================================
 * Command state transitions
 * ========================================================================= */

void test_state_transitions_to_waiting_after_send(void)
{
    at_send_raw("AT", 0, NULL, NULL);
    at_process(); /* start_next() → state = WAITING */
    TEST_ASSERT_EQUAL_INT(AT_STATE_WAITING, at_state());
}

void test_state_returns_to_idle_after_ok(void)
{
    at_send_raw("AT", 0, NULL, NULL);
    at_process();

    feed_ok();

    TEST_ASSERT_EQUAL_INT(AT_STATE_IDLE, at_state());
}

void test_queue_depth_decrements_on_completion(void)
{
    at_send_raw("AT", 0, NULL, NULL);
    at_process();
    TEST_ASSERT_EQUAL_UINT8(1, at_queue_depth());

    feed_ok();
    TEST_ASSERT_EQUAL_UINT8(0, at_queue_depth());
}

/* =========================================================================
 * Callback delivery
 * ========================================================================= */

void test_callback_called_on_ok(void)
{
    at_send_raw("AT", 0, generic_cb, NULL);
    at_process();
    feed_ok();

    TEST_ASSERT_TRUE(s_cb_called);
    TEST_ASSERT_EQUAL_INT(AT_OK, s_last_status);
}

void test_callback_called_on_error(void)
{
    at_send_raw("AT+INVALID", 0, generic_cb, NULL);
    at_process();
    feed_error();

    TEST_ASSERT_TRUE(s_cb_called);
    TEST_ASSERT_EQUAL_INT(AT_ERR_GENERIC, s_last_status);
}

void test_callback_receives_cme_error(void)
{
    at_send_raw("AT+CPIN?", 0, generic_cb, NULL);
    at_process();

    feed_line("+CME ERROR: 11");

    TEST_ASSERT_TRUE(s_cb_called);
    TEST_ASSERT_EQUAL_INT(AT_ERR_CME, s_last_status);
    TEST_ASSERT_EQUAL_INT32(11, s_last_error_code);
}

void test_callback_receives_cms_error(void)
{
    at_send_raw("AT+CMGS", 0, generic_cb, NULL);
    at_process();

    feed_line("+CMS ERROR: 330");

    TEST_ASSERT_TRUE(s_cb_called);
    TEST_ASSERT_EQUAL_INT(AT_ERR_CMS, s_last_status);
    TEST_ASSERT_EQUAL_INT32(330, s_last_error_code);
}

void test_callback_receives_response_lines(void)
{
    at_send_raw("AT+CGMI", 0, generic_cb, NULL);
    at_process();

    feed_line("Quectel");
    feed_ok();

    TEST_ASSERT_TRUE(s_cb_called);
    TEST_ASSERT_EQUAL_INT(AT_OK, s_last_status);
    TEST_ASSERT_EQUAL_UINT8(1, s_last_num_lines);
    TEST_ASSERT_EQUAL_STRING("Quectel", s_last_line0);
}

void test_callback_not_called_without_ok(void)
{
    at_send_raw("AT+CGMI", 0, generic_cb, NULL);
    at_process();

    /* Only intermediate lines, no final result yet */
    feed_line("Quectel");

    TEST_ASSERT_FALSE(s_cb_called);
}

void test_null_callback_does_not_crash(void)
{
    at_send_raw("AT", 0, NULL, NULL);
    at_process();
    feed_ok();
    /* Just verifying no crash */
    TEST_ASSERT_EQUAL_INT(AT_STATE_IDLE, at_state());
}

/* =========================================================================
 * Queue sequencing
 * ========================================================================= */

void test_second_command_sent_after_first_completes(void)
{
    /* Enqueue two commands before processing */
    at_send_raw("ATE0", 0, NULL, NULL);
    at_send_raw("ATZ",  0, NULL, NULL);
    TEST_ASSERT_EQUAL_UINT8(2, at_queue_depth());

    at_process(); /* starts ATE0 */
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "ATE0"));

    feed_ok(); /* completes ATE0, engine auto-starts ATZ */
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "ATZ"));
}

static int s_call_order[2];
static int s_order_idx;

static void order_cb0(const at_response_t *r, void *u) {
    (void)r; (void)u; s_call_order[s_order_idx++] = 0;
}
static void order_cb1(const at_response_t *r, void *u) {
    (void)r; (void)u; s_call_order[s_order_idx++] = 1;
}

void test_callbacks_called_in_order(void)
{
    s_order_idx = 0;

    at_send_raw("AT", 0, order_cb0, NULL);
    at_send_raw("AT", 0, order_cb1, NULL);
    at_process(); feed_ok(); /* cmd0 done → order_cb0 */
                  feed_ok(); /* cmd1 done → order_cb1 */

    TEST_ASSERT_EQUAL_INT(0, s_call_order[0]);
    TEST_ASSERT_EQUAL_INT(1, s_call_order[1]);
}

void test_queue_full_returns_busy(void)
{
    /* Fill the queue to AT_CFG_CMD_QUEUE_DEPTH */
    at_result_t rc = AT_OK;
    for (size_t i = 0; i < (size_t)(AT_CFG_CMD_QUEUE_DEPTH + 2); i++) {
        rc = at_send_raw("AT", 0, NULL, NULL);
    }
    TEST_ASSERT_EQUAL_INT(AT_ERR_BUSY, rc);
}

/* =========================================================================
 * at_abort
 * ========================================================================= */

void test_abort_active_command_calls_cb_aborted(void)
{
    at_send_raw("AT+CPIN?", 0, generic_cb, NULL);
    at_process(); /* state → WAITING */

    at_abort();
    at_process();

    TEST_ASSERT_TRUE(s_cb_called);
    TEST_ASSERT_EQUAL_INT(AT_ERR_ABORTED, s_last_status);
}

void test_abort_when_idle_is_noop(void)
{
    at_abort(); /* should not crash */
    TEST_ASSERT_EQUAL_INT(AT_STATE_IDLE, at_state());
}

/* =========================================================================
 * at_reset
 * ========================================================================= */

void test_reset_flushes_queue(void)
{
    at_send_raw("AT", 0, generic_cb, NULL);
    at_send_raw("AT", 0, generic_cb, NULL);
    at_process(); /* start first */

    at_reset();

    TEST_ASSERT_EQUAL_INT(AT_STATE_IDLE, at_state());
    TEST_ASSERT_EQUAL_UINT8(0, at_queue_depth());
}

void test_reset_fires_aborted_for_active(void)
{
    at_send_raw("AT+CPIN?", 0, generic_cb, NULL);
    at_process();

    at_reset();

    TEST_ASSERT_TRUE(s_cb_called);
    TEST_ASSERT_EQUAL_INT(AT_ERR_ABORTED, s_last_status);
}

/* =========================================================================
 * Timeout
 * ========================================================================= */

void test_timeout_calls_cb_with_timeout_error(void)
{
    /* Use a 1 ms timeout so we can simulate expiry with one tick */
    at_send_raw("AT+CPIN?", 1U, generic_cb, NULL);
    at_process();

    at_tick(10U); /* advance past timeout */
    at_process();

    TEST_ASSERT_TRUE(s_cb_called);
    TEST_ASSERT_EQUAL_INT(AT_ERR_TIMEOUT, s_last_status);
}

void test_tick_does_nothing_when_idle(void)
{
    at_tick(5000U); /* no active command — should not crash */
    TEST_ASSERT_EQUAL_INT(AT_STATE_IDLE, at_state());
}

/* =========================================================================
 * URC dispatch
 * ========================================================================= */

static bool        s_urc_called;
static char        s_urc_line[128];
static const char *s_urc_user_sentinel = "sentinel";

static void urc_handler(const char *line, void *user)
{
    s_urc_called = true;
    (void)user;
    size_t n = strlen(line);
    if (n >= sizeof(s_urc_line)) n = sizeof(s_urc_line) - 1U;
    memcpy(s_urc_line, line, n);
    s_urc_line[n] = '\0';
}

void test_urc_register_returns_ok(void)
{
    at_result_t rc = at_register_urc("+CREG", urc_handler, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
}

void test_urc_dispatched_when_no_active_command(void)
{
    s_urc_called  = false;
    s_urc_line[0] = '\0';

    at_register_urc("+CREG", urc_handler, NULL);
    feed_line("+CREG: 1");

    TEST_ASSERT_TRUE(s_urc_called);
    TEST_ASSERT_NOT_NULL(strstr(s_urc_line, "+CREG"));
}

void test_urc_prefix_matches_only_correct_prefix(void)
{
    s_urc_called = false;
    at_register_urc("+CREG", urc_handler, NULL);
    feed_line("+CGREG: 1"); /* different prefix — should NOT match */
    TEST_ASSERT_FALSE(s_urc_called);
}

void test_urc_deregister_stops_dispatch(void)
{
    s_urc_called = false;
    at_register_urc("+CREG", urc_handler, NULL);
    at_deregister_urc("+CREG");
    feed_line("+CREG: 1");
    TEST_ASSERT_FALSE(s_urc_called);
}

void test_urc_ring_dispatched(void)
{
    s_urc_called = false;
    at_register_urc("RING", urc_handler, NULL);
    feed_line("RING");
    TEST_ASSERT_TRUE(s_urc_called);
}

static void *s_received_user = NULL;

static void urc_user_cb(const char *l, void *u) { (void)l; s_received_user = u; }

void test_urc_user_pointer_passed(void)
{
    s_received_user = NULL;
    at_register_urc("+TEST", urc_user_cb, (void *)s_urc_user_sentinel);
    feed_line("+TEST: data");
    TEST_ASSERT_EQUAL_PTR(s_urc_user_sentinel, s_received_user);
}

/* =========================================================================
 * at_result_str
 * ========================================================================= */

void test_result_str_ok(void)
{
    TEST_ASSERT_NOT_NULL(at_result_str(AT_OK));
    TEST_ASSERT_GREATER_THAN(0U, strlen(at_result_str(AT_OK)));
}

void test_result_str_covers_all_codes(void)
{
    /* Smoke-test all defined codes are non-NULL and non-empty */
    at_result_t codes[] = {
        AT_OK, AT_ERR_CME, AT_ERR_CMS, AT_ERR_GENERIC,
        AT_ERR_TIMEOUT, AT_ERR_BUSY, AT_ERR_PARAM,
        AT_ERR_OVERFLOW, AT_ERR_ABORTED,
    };
    for (size_t i = 0; i < sizeof(codes)/sizeof(codes[0]); i++) {
        const char *s = at_result_str(codes[i]);
        TEST_ASSERT_NOT_NULL(s);
        TEST_ASSERT_GREATER_THAN(0U, strlen(s));
    }
}

/* =========================================================================
 * Feed robustness
 * ========================================================================= */

void test_feed_empty_data_does_not_crash(void)
{
    const uint8_t empty[] = "";
    at_feed(empty, 0U);
    at_process();
    TEST_ASSERT_EQUAL_INT(AT_STATE_IDLE, at_state());
}

void test_feed_lf_only_line_ending(void)
{
    /* Some modems send LF without CR */
    at_send_raw("AT", 0, generic_cb, NULL);
    at_process();

    const char *resp = "OK\n";
    at_feed((const uint8_t *)resp, strlen(resp));
    at_process();

    TEST_ASSERT_TRUE(s_cb_called);
    TEST_ASSERT_EQUAL_INT(AT_OK, s_last_status);
}

void test_multiple_intermediate_lines(void)
{
    at_send_raw("AT+CPBR=1,5", 0, generic_cb, NULL);
    at_process();

    feed_line("+CPBR: 1,\"Alice\",145,\"Alice Smith\"");
    feed_line("+CPBR: 2,\"Bob\",145,\"Bob Jones\"");
    feed_ok();

    TEST_ASSERT_TRUE(s_cb_called);
    TEST_ASSERT_EQUAL_INT(AT_OK, s_last_status);
    TEST_ASSERT_EQUAL_UINT8(2, s_last_num_lines);
}

/* =========================================================================
 * HAL write failure (AT_ERR_IO)
 * ========================================================================= */

/* Write fails on the very first at_platform_write() call (command body). */
void test_write_failure_delivers_io_error(void)
{
    s_write_fail = true;
    at_send_raw("AT+CSQ", 0, generic_cb, NULL);
    at_process(); /* triggers engine_start_next → write fails */

    TEST_ASSERT_TRUE(s_cb_called);
    TEST_ASSERT_EQUAL_INT(AT_ERR_IO, s_last_status);
    TEST_ASSERT_EQUAL_INT(AT_STATE_IDLE, at_state());
}

/* Engine returns to IDLE after an IO error — next command can be queued. */
void test_write_failure_engine_returns_to_idle(void)
{
    s_write_fail = true;
    at_send_raw("AT", 0, generic_cb, NULL);
    at_process();

    TEST_ASSERT_EQUAL_INT(AT_STATE_IDLE, at_state());
    TEST_ASSERT_EQUAL_UINT8(0, at_queue_depth());
}

/* Successful write after a previous IO error works normally. */
void test_write_failure_then_success(void)
{
    /* First command — write fails */
    s_write_fail = true;
    at_send_raw("AT+CSQ", 0, generic_cb, NULL);
    at_process();
    TEST_ASSERT_TRUE(s_cb_called);
    TEST_ASSERT_EQUAL_INT(AT_ERR_IO, s_last_status);

    /* Reset test state; re-enable write */
    s_cb_called   = false;
    s_last_status = AT_PENDING;
    s_write_fail  = false;

    at_send_raw("AT", 0, generic_cb, NULL);
    at_process();
    feed_ok();

    TEST_ASSERT_TRUE(s_cb_called);
    TEST_ASSERT_EQUAL_INT(AT_OK, s_last_status);
}

/* =========================================================================
 * Test runner
 * ========================================================================= */

int main(void)
{
    UNITY_BEGIN();

    /* Initialisation */
    RUN_TEST(test_init_state_is_idle);
    RUN_TEST(test_init_queue_depth_is_zero);

    /* at_send_raw */
    RUN_TEST(test_send_raw_returns_ok);
    RUN_TEST(test_send_raw_null_cmd_returns_param_error);
    RUN_TEST(test_send_raw_transmits_cmd_on_process);
    RUN_TEST(test_send_raw_appends_cr);

    /* State transitions */
    RUN_TEST(test_state_transitions_to_waiting_after_send);
    RUN_TEST(test_state_returns_to_idle_after_ok);
    RUN_TEST(test_queue_depth_decrements_on_completion);

    /* Callbacks */
    RUN_TEST(test_callback_called_on_ok);
    RUN_TEST(test_callback_called_on_error);
    RUN_TEST(test_callback_receives_cme_error);
    RUN_TEST(test_callback_receives_cms_error);
    RUN_TEST(test_callback_receives_response_lines);
    RUN_TEST(test_callback_not_called_without_ok);
    RUN_TEST(test_null_callback_does_not_crash);

    /* Queue sequencing */
    RUN_TEST(test_second_command_sent_after_first_completes);
    RUN_TEST(test_callbacks_called_in_order);
    RUN_TEST(test_queue_full_returns_busy);

    /* at_abort */
    RUN_TEST(test_abort_active_command_calls_cb_aborted);
    RUN_TEST(test_abort_when_idle_is_noop);

    /* at_reset */
    RUN_TEST(test_reset_flushes_queue);
    RUN_TEST(test_reset_fires_aborted_for_active);

    /* Timeout */
    RUN_TEST(test_timeout_calls_cb_with_timeout_error);
    RUN_TEST(test_tick_does_nothing_when_idle);

    /* URC */
    RUN_TEST(test_urc_register_returns_ok);
    RUN_TEST(test_urc_dispatched_when_no_active_command);
    RUN_TEST(test_urc_prefix_matches_only_correct_prefix);
    RUN_TEST(test_urc_deregister_stops_dispatch);
    RUN_TEST(test_urc_ring_dispatched);
    RUN_TEST(test_urc_user_pointer_passed);

    /* at_result_str */
    RUN_TEST(test_result_str_ok);
    RUN_TEST(test_result_str_covers_all_codes);

    /* Feed robustness */
    RUN_TEST(test_feed_empty_data_does_not_crash);
    RUN_TEST(test_feed_lf_only_line_ending);
    RUN_TEST(test_multiple_intermediate_lines);

    /* HAL write failure */
    RUN_TEST(test_write_failure_delivers_io_error);
    RUN_TEST(test_write_failure_engine_returns_to_idle);
    RUN_TEST(test_write_failure_then_success);

    return UNITY_END();
}
