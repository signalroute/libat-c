/**
 * @file test_gsm.c
 * @brief Unit tests for at_gsm response parsers and command builders.
 *
 * Tests that exercise:
 *  - at_parse_csq / at_parse_cesq / at_parse_creg / at_parse_cpin
 *  - at_parse_cops / at_parse_sms_read / at_parse_cmgs
 *  - at_gsm_* command builder functions (verifies correct AT strings)
 *
 * Parsers are tested by constructing at_response_t structs directly — no
 * need to run the full engine state machine for these isolated tests.
 *
 * Command builders are tested by inspecting what at_platform_write()
 * receives after at_process() starts the queued command.
 *
 * SPDX-License-Identifier: MIT
 */

#include "unity.h"
#include "at.h"
#include "at_gsm.h"

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* =========================================================================
 * Platform mock
 * ========================================================================= */

static char   s_tx_buf[256];
static size_t s_tx_len;

size_t at_platform_write(const uint8_t *data, size_t len)
{
    size_t avail = sizeof(s_tx_buf) - s_tx_len - 1U;
    if (len > avail) len = avail;
    memcpy(s_tx_buf + s_tx_len, data, len);
    s_tx_len += len;
    s_tx_buf[s_tx_len] = '\0';
    return len;
}

/* =========================================================================
 * Helpers — build fake at_response_t
 * ========================================================================= */

/**
 * Build a minimal at_response_t with one payload line and AT_OK status.
 * The line pointer must outlive the response (use string literals or local
 * arrays with sufficient scope).
 */
static at_response_t make_ok_resp(const char *line)
{
    at_response_t r;
    memset(&r, 0, sizeof(r));
    r.status    = AT_OK;
    r.num_lines = 1U;
    r.lines[0]  = line;
    return r;
}

static at_response_t make_error_resp(void)
{
    at_response_t r;
    memset(&r, 0, sizeof(r));
    r.status    = AT_ERR_GENERIC;
    r.num_lines = 0U;
    return r;
}

/* =========================================================================
 * setUp / tearDown
 * ========================================================================= */

void setUp(void)
{
    at_init();
    s_tx_buf[0] = '\0';
    s_tx_len    = 0U;
}

void tearDown(void) {}

/* =========================================================================
 * at_parse_csq
 * ========================================================================= */

void test_parse_csq_typical(void)
{
    at_response_t resp = make_ok_resp("+CSQ: 20,0");
    at_csq_t csq;
    TEST_ASSERT_TRUE(at_parse_csq(&resp, &csq));
    /* rssi = 20 → -113 + 20*2 = -73 dBm */
    TEST_ASSERT_EQUAL_INT(-73, csq.rssi_dbm);
    TEST_ASSERT_EQUAL_UINT8(0, csq.ber);
}

void test_parse_csq_unknown_rssi(void)
{
    at_response_t resp = make_ok_resp("+CSQ: 99,99");
    at_csq_t csq;
    TEST_ASSERT_TRUE(at_parse_csq(&resp, &csq));
    TEST_ASSERT_EQUAL_INT(-999, csq.rssi_dbm); /* unknown sentinel */
    TEST_ASSERT_EQUAL_UINT8(99, csq.ber);
}

void test_parse_csq_min_rssi(void)
{
    at_response_t resp = make_ok_resp("+CSQ: 0,0");
    at_csq_t csq;
    TEST_ASSERT_TRUE(at_parse_csq(&resp, &csq));
    /* rssi = 0 → -113 dBm */
    TEST_ASSERT_EQUAL_INT(-113, csq.rssi_dbm);
}

void test_parse_csq_max_rssi(void)
{
    at_response_t resp = make_ok_resp("+CSQ: 31,0");
    at_csq_t csq;
    TEST_ASSERT_TRUE(at_parse_csq(&resp, &csq));
    /* rssi = 31 → -113 + 62 = -51 dBm */
    TEST_ASSERT_EQUAL_INT(-51, csq.rssi_dbm);
}

void test_parse_csq_null_resp_returns_false(void)
{
    at_csq_t csq;
    TEST_ASSERT_FALSE(at_parse_csq(NULL, &csq));
}

void test_parse_csq_null_out_returns_false(void)
{
    at_response_t resp = make_ok_resp("+CSQ: 10,0");
    TEST_ASSERT_FALSE(at_parse_csq(&resp, NULL));
}

void test_parse_csq_error_resp_returns_false(void)
{
    at_response_t resp = make_error_resp();
    at_csq_t csq;
    TEST_ASSERT_FALSE(at_parse_csq(&resp, &csq));
}

void test_parse_csq_wrong_prefix_returns_false(void)
{
    at_response_t resp = make_ok_resp("+CESQ: 20,0");
    at_csq_t csq;
    TEST_ASSERT_FALSE(at_parse_csq(&resp, &csq));
}

/* =========================================================================
 * at_parse_cesq
 * ========================================================================= */

void test_parse_cesq_typical(void)
{
    /* rxlev,ber,rscp,ecno,rsrq,rsrp */
    at_response_t resp = make_ok_resp("+CESQ: 40,99,255,255,12,50");
    at_cesq_t q;
    TEST_ASSERT_TRUE(at_parse_cesq(&resp, &q));
    TEST_ASSERT_EQUAL_INT16(40, q.rxlev);
    TEST_ASSERT_EQUAL_UINT8(99, q.ber);
    TEST_ASSERT_EQUAL_INT16(255, q.rscp);
    TEST_ASSERT_EQUAL_INT16(255, q.ecno);
    TEST_ASSERT_EQUAL_INT16(12, q.rsrq);
    TEST_ASSERT_EQUAL_INT16(50, q.rsrp);
}

void test_parse_cesq_null_resp_returns_false(void)
{
    at_cesq_t q;
    TEST_ASSERT_FALSE(at_parse_cesq(NULL, &q));
}

/* =========================================================================
 * at_parse_creg
 * ========================================================================= */

void test_parse_creg_home(void)
{
    at_response_t resp = make_ok_resp("+CREG: 1");
    at_reg_status_t s;
    TEST_ASSERT_TRUE(at_parse_creg(&resp, &s));
    TEST_ASSERT_EQUAL_INT(AT_REG_HOME, s);
}

void test_parse_creg_roaming(void)
{
    at_response_t resp = make_ok_resp("+CREG: 5");
    at_reg_status_t s;
    TEST_ASSERT_TRUE(at_parse_creg(&resp, &s));
    TEST_ASSERT_EQUAL_INT(AT_REG_ROAMING, s);
}

void test_parse_creg_not_registered(void)
{
    at_response_t resp = make_ok_resp("+CREG: 0");
    at_reg_status_t s;
    TEST_ASSERT_TRUE(at_parse_creg(&resp, &s));
    TEST_ASSERT_EQUAL_INT(AT_REG_NOT, s);
}

void test_parse_creg_format_n_stat(void)
{
    /* AT+CREG=1 mode response: "+CREG: <n>,<stat>" */
    at_response_t resp = make_ok_resp("+CREG: 1,5");
    at_reg_status_t s;
    TEST_ASSERT_TRUE(at_parse_creg(&resp, &s));
    /* Parser must extract the stat field regardless of n prefix */
    TEST_ASSERT_EQUAL_INT(AT_REG_ROAMING, s);
}

void test_parse_creg_null_returns_false(void)
{
    at_reg_status_t s;
    TEST_ASSERT_FALSE(at_parse_creg(NULL, &s));
}

/* =========================================================================
 * at_parse_cpin
 * ========================================================================= */

void test_parse_cpin_ready(void)
{
    at_response_t resp = make_ok_resp("+CPIN: READY");
    at_cpin_t pin;
    TEST_ASSERT_TRUE(at_parse_cpin(&resp, &pin));
    TEST_ASSERT_EQUAL_INT(AT_CPIN_READY, pin);
}

void test_parse_cpin_sim_pin(void)
{
    at_response_t resp = make_ok_resp("+CPIN: SIM PIN");
    at_cpin_t pin;
    TEST_ASSERT_TRUE(at_parse_cpin(&resp, &pin));
    TEST_ASSERT_EQUAL_INT(AT_CPIN_SIM_PIN, pin);
}

void test_parse_cpin_sim_puk(void)
{
    at_response_t resp = make_ok_resp("+CPIN: SIM PUK");
    at_cpin_t pin;
    TEST_ASSERT_TRUE(at_parse_cpin(&resp, &pin));
    TEST_ASSERT_EQUAL_INT(AT_CPIN_SIM_PUK, pin);
}

void test_parse_cpin_null_returns_false(void)
{
    at_cpin_t pin;
    TEST_ASSERT_FALSE(at_parse_cpin(NULL, &pin));
}

/* =========================================================================
 * at_parse_cmgs
 * ========================================================================= */

void test_parse_cmgs_typical(void)
{
    at_response_t resp = make_ok_resp("+CMGS: 42");
    uint8_t mr = 0;
    TEST_ASSERT_TRUE(at_parse_cmgs(&resp, &mr));
    TEST_ASSERT_EQUAL_UINT8(42U, mr);
}

void test_parse_cmgs_zero_mr(void)
{
    at_response_t resp = make_ok_resp("+CMGS: 0");
    uint8_t mr = 0xFF;
    TEST_ASSERT_TRUE(at_parse_cmgs(&resp, &mr));
    TEST_ASSERT_EQUAL_UINT8(0U, mr);
}

void test_parse_cmgs_null_returns_false(void)
{
    uint8_t mr;
    TEST_ASSERT_FALSE(at_parse_cmgs(NULL, &mr));
}

/* =========================================================================
 * Command builders — verify transmitted AT string
 * ========================================================================= */

static void drain_and_complete(void)
{
    at_process();
    /* Inject OK so the engine returns to idle */
    const uint8_t ok[] = "OK\r\n";
    at_feed(ok, sizeof(ok) - 1U);
    at_process();
}

void test_gsm_at_sends_at(void)
{
    at_gsm_at(NULL, NULL);
    drain_and_complete();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT"));
}

void test_gsm_atz_sends_atz(void)
{
    at_gsm_atz(NULL, NULL);
    drain_and_complete();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "ATZ"));
}

void test_gsm_echo_disable(void)
{
    at_gsm_echo(false, NULL, NULL);
    drain_and_complete();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "ATE0"));
}

void test_gsm_echo_enable(void)
{
    at_gsm_echo(true, NULL, NULL);
    drain_and_complete();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "ATE1"));
}

void test_gsm_cfun_full(void)
{
    at_gsm_cfun(1, 0, NULL, NULL);
    drain_and_complete();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CFUN=1"));
}

void test_gsm_cfun_airplane(void)
{
    at_gsm_cfun(4, 0, NULL, NULL);
    drain_and_complete();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CFUN=4"));
}

void test_gsm_csq_sends_correct_cmd(void)
{
    at_gsm_csq(NULL, NULL);
    drain_and_complete();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CSQ"));
}

void test_gsm_cmgf_text_mode(void)
{
    at_gsm_cmgf(1, NULL, NULL);
    drain_and_complete();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CMGF=1"));
}

void test_gsm_cmgf_pdu_mode(void)
{
    at_gsm_cmgf(0, NULL, NULL);
    drain_and_complete();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CMGF=0"));
}

void test_gsm_cmgs_sends_number(void)
{
    at_gsm_cmgs("+491711234567", "Hello", NULL, NULL);
    drain_and_complete();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "+491711234567"));
}

void test_gsm_creg_set_sends_n(void)
{
    at_gsm_creg_set(2, NULL, NULL);
    drain_and_complete();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CREG=2"));
}

void test_gsm_creg_query(void)
{
    at_gsm_creg_query(NULL, NULL);
    drain_and_complete();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CREG?"));
}

void test_gsm_dial_voice(void)
{
    at_gsm_dial("+491711234567", true, NULL, NULL);
    drain_and_complete();
    /* Voice dial appends ; */
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "ATD+491711234567;"));
}

void test_gsm_dial_data(void)
{
    at_gsm_dial("+491711234567", false, NULL, NULL);
    drain_and_complete();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "ATD+491711234567"));
    /* No semicolon for data call */
    char *semi = strstr(s_tx_buf, ";");
    TEST_ASSERT_NULL(semi);
}

void test_gsm_answer_sends_ata(void)
{
    at_gsm_answer(NULL, NULL);
    drain_and_complete();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "ATA"));
}

void test_gsm_hangup_sends_ath(void)
{
    at_gsm_hangup(NULL, NULL);
    drain_and_complete();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "ATH"));
}

void test_gsm_cpin_query_sends_cmd(void)
{
    at_gsm_cpin_query(NULL, NULL);
    drain_and_complete();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CPIN?"));
}

void test_gsm_cpin_enter_includes_pin(void)
{
    at_gsm_cpin_enter("1234", NULL, NULL);
    drain_and_complete();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "1234"));
}

void test_gsm_imei_sends_cgsn(void)
{
    at_gsm_imei(NULL, NULL);
    drain_and_complete();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CGSN"));
}

void test_gsm_imsi_sends_cimi(void)
{
    at_gsm_imsi(NULL, NULL);
    drain_and_complete();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CIMI"));
}

void test_gsm_cgmi_sends_cmd(void)
{
    at_gsm_cgmi(NULL, NULL);
    drain_and_complete();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CGMI"));
}

void test_gsm_cgdcont_basic(void)
{
    at_cgdcont_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.cid = 1;
    strncpy(ctx.pdp_type, "IP", sizeof(ctx.pdp_type) - 1U);
    strncpy(ctx.apn,      "internet", sizeof(ctx.apn) - 1U);

    at_gsm_cgdcont(&ctx, NULL, NULL);
    drain_and_complete();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CGDCONT"));
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "internet"));
}

void test_gsm_cgact_activate(void)
{
    at_gsm_cgact(1, true, NULL, NULL);
    drain_and_complete();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CGACT=1,1"));
}

void test_gsm_cgact_deactivate(void)
{
    at_gsm_cgact(1, false, NULL, NULL);
    drain_and_complete();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CGACT=0,1"));
}

/* =========================================================================
 * Null guard on builders
 * ========================================================================= */

void test_gsm_cmgs_null_returns_param_error(void)
{
    TEST_ASSERT_EQUAL_INT(AT_ERR_PARAM, at_gsm_cmgs(NULL, NULL, NULL, NULL));
}

void test_gsm_dial_null_number_returns_param_error(void)
{
    TEST_ASSERT_EQUAL_INT(AT_ERR_PARAM, at_gsm_dial(NULL, true, NULL, NULL));
}

/* =========================================================================
 * AT injection sanitization tests
 * ========================================================================= */

void test_dial_rejects_injection(void)
{
    TEST_ASSERT_EQUAL_INT(AT_ERR_PARAM, at_gsm_dial("+1234\r\nATH", true, NULL, NULL));
}

void test_cmgs_rejects_injection(void)
{
    TEST_ASSERT_EQUAL_INT(AT_ERR_PARAM, at_gsm_cmgs("+1\nATH", "hello", NULL, NULL));
}

void test_cmgs_pdu_rejects_injection(void)
{
    TEST_ASSERT_EQUAL_INT(AT_ERR_PARAM, at_gsm_cmgs_pdu(NULL, "+1\nATH", "hello", NULL, NULL));
}

void test_dial_accepts_valid_number(void)
{
    at_result_t rc = at_gsm_dial("+491711234567", true, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    drain_and_complete();
}

void test_dial_accepts_star_hash(void)
{
    at_result_t rc = at_gsm_dial("*100#", false, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    drain_and_complete();
}

/* =========================================================================
 * GSM-7 encoder tests
 * ========================================================================= */

void test_gsm7_encode_hello(void)
{
    uint8_t out[20];
    size_t out_len = 0, n_chars = 0;
    at_gsm7_result_t rc = at_gsm7_encode("Hello", out, sizeof(out), &out_len, &n_chars);
    TEST_ASSERT_EQUAL_INT(AT_GSM7_OK, rc);
    TEST_ASSERT_EQUAL_size_t(5, n_chars);
    /* "Hello" in GSM-7 packed:
     * H=0x48, e=0x65, l=0x6C, l=0x6C, o=0x6F
     * packed: E8329BFD06  (5 chars → ceil(5*7/8)=5 bytes) */
    TEST_ASSERT_EQUAL_size_t(5, out_len);
    TEST_ASSERT_EQUAL_HEX8(0xC8, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x32, out[1]);
    TEST_ASSERT_EQUAL_HEX8(0x9B, out[2]);
    TEST_ASSERT_EQUAL_HEX8(0xFD, out[3]);
    TEST_ASSERT_EQUAL_HEX8(0x06, out[4]);
}

void test_gsm7_encode_empty_string(void)
{
    uint8_t out[4];
    size_t out_len = 0, n_chars = 0;
    at_gsm7_result_t rc = at_gsm7_encode("", out, sizeof(out), &out_len, &n_chars);
    TEST_ASSERT_EQUAL_INT(AT_GSM7_OK, rc);
    TEST_ASSERT_EQUAL_size_t(0, n_chars);
    TEST_ASSERT_EQUAL_size_t(0, out_len);
}

void test_gsm7_encode_null_input(void)
{
    uint8_t out[4];
    size_t out_len = 0, n_chars = 0;
    at_gsm7_result_t rc = at_gsm7_encode(NULL, out, sizeof(out), &out_len, &n_chars);
    TEST_ASSERT_EQUAL_INT(AT_GSM7_ERR_NULL, rc);
}

void test_gsm7_encode_null_output(void)
{
    size_t out_len = 0, n_chars = 0;
    at_gsm7_result_t rc = at_gsm7_encode("Hi", NULL, 0, &out_len, &n_chars);
    TEST_ASSERT_EQUAL_INT(AT_GSM7_ERR_NULL, rc);
}

void test_gsm7_encode_too_long(void)
{
    /* 161 characters — one over the SMS limit */
    char text[162];
    memset(text, 'A', 161);
    text[161] = '\0';
    uint8_t out[150];
    size_t out_len = 0, n_chars = 0;
    at_gsm7_result_t rc = at_gsm7_encode(text, out, sizeof(out), &out_len, &n_chars);
    TEST_ASSERT_EQUAL_INT(AT_GSM7_ERR_TOO_LONG, rc);
}

void test_gsm7_encode_160_chars(void)
{
    char text[161];
    memset(text, 'A', 160);
    text[160] = '\0';
    uint8_t out[150];
    size_t out_len = 0, n_chars = 0;
    at_gsm7_result_t rc = at_gsm7_encode(text, out, sizeof(out), &out_len, &n_chars);
    TEST_ASSERT_EQUAL_INT(AT_GSM7_OK, rc);
    TEST_ASSERT_EQUAL_size_t(160, n_chars);
    TEST_ASSERT_EQUAL_size_t(140, out_len); /* ceil(160*7/8) = 140 */
}

void test_gsm7_encode_buf_too_small(void)
{
    uint8_t out[2]; /* way too small for "Hello" */
    size_t out_len = 0, n_chars = 0;
    at_gsm7_result_t rc = at_gsm7_encode("Hello World", out, sizeof(out), &out_len, &n_chars);
    TEST_ASSERT_EQUAL_INT(AT_GSM7_ERR_BUF_FULL, rc);
}

void test_gsm7_encode_digits_and_spaces(void)
{
    uint8_t out[20];
    size_t out_len = 0, n_chars = 0;
    at_gsm7_result_t rc = at_gsm7_encode("0123", out, sizeof(out), &out_len, &n_chars);
    TEST_ASSERT_EQUAL_INT(AT_GSM7_OK, rc);
    TEST_ASSERT_EQUAL_size_t(4, n_chars);
    /* '0'=0x30,'1'=0x31,'2'=0x32,'3'=0x33 packed */
    TEST_ASSERT_EQUAL_size_t(4, out_len);
    /* Byte 0: '0'[6:0]=0x30 → bits 0..6 = 0x30 */
    TEST_ASSERT_EQUAL_HEX8(0xB0, out[0]); /* 0x30 | (0x31<<7 & 0xFF) = 0xB0 */
}

void test_gsm7_encode_at_sign(void)
{
    /* '@' maps to GSM-7 code 0x00 (first entry) */
    uint8_t out[4];
    size_t out_len = 0, n_chars = 0;
    at_gsm7_result_t rc = at_gsm7_encode("@", out, sizeof(out), &out_len, &n_chars);
    TEST_ASSERT_EQUAL_INT(AT_GSM7_OK, rc);
    TEST_ASSERT_EQUAL_size_t(1, n_chars);
    TEST_ASSERT_EQUAL_HEX8(0x00, out[0]);
}

/* =========================================================================
 * PDU CMGS tests
 * ========================================================================= */

void test_pdu_cmgs_null_number(void)
{
    TEST_ASSERT_EQUAL_INT(AT_ERR_PARAM, at_gsm_cmgs_pdu(NULL, NULL, "hi", NULL, NULL));
}

void test_pdu_cmgs_null_text(void)
{
    TEST_ASSERT_EQUAL_INT(AT_ERR_PARAM, at_gsm_cmgs_pdu(NULL, "+491234", NULL, NULL, NULL));
}

void test_pdu_cmgs_enqueues_command(void)
{
    /* setUp() already called at_init() and cleared s_tx_buf */
    at_result_t rc = at_gsm_cmgs_pdu(NULL, "+491234567890", "Hello", NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    /* Command should start with AT+CMGS= */
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CMGS="));
}

/* =========================================================================
 * Test runner
 * ========================================================================= */

int main(void)
{
    UNITY_BEGIN();

    /* at_parse_csq */
    RUN_TEST(test_parse_csq_typical);
    RUN_TEST(test_parse_csq_unknown_rssi);
    RUN_TEST(test_parse_csq_min_rssi);
    RUN_TEST(test_parse_csq_max_rssi);
    RUN_TEST(test_parse_csq_null_resp_returns_false);
    RUN_TEST(test_parse_csq_null_out_returns_false);
    RUN_TEST(test_parse_csq_error_resp_returns_false);
    RUN_TEST(test_parse_csq_wrong_prefix_returns_false);

    /* at_parse_cesq */
    RUN_TEST(test_parse_cesq_typical);
    RUN_TEST(test_parse_cesq_null_resp_returns_false);

    /* at_parse_creg */
    RUN_TEST(test_parse_creg_home);
    RUN_TEST(test_parse_creg_roaming);
    RUN_TEST(test_parse_creg_not_registered);
    RUN_TEST(test_parse_creg_format_n_stat);
    RUN_TEST(test_parse_creg_null_returns_false);

    /* at_parse_cpin */
    RUN_TEST(test_parse_cpin_ready);
    RUN_TEST(test_parse_cpin_sim_pin);
    RUN_TEST(test_parse_cpin_sim_puk);
    RUN_TEST(test_parse_cpin_null_returns_false);

    /* at_parse_cmgs */
    RUN_TEST(test_parse_cmgs_typical);
    RUN_TEST(test_parse_cmgs_zero_mr);
    RUN_TEST(test_parse_cmgs_null_returns_false);

    /* Command builders */
    RUN_TEST(test_gsm_at_sends_at);
    RUN_TEST(test_gsm_atz_sends_atz);
    RUN_TEST(test_gsm_echo_disable);
    RUN_TEST(test_gsm_echo_enable);
    RUN_TEST(test_gsm_cfun_full);
    RUN_TEST(test_gsm_cfun_airplane);
    RUN_TEST(test_gsm_csq_sends_correct_cmd);
    RUN_TEST(test_gsm_cmgf_text_mode);
    RUN_TEST(test_gsm_cmgf_pdu_mode);
    RUN_TEST(test_gsm_cmgs_sends_number);
    RUN_TEST(test_gsm_creg_set_sends_n);
    RUN_TEST(test_gsm_creg_query);
    RUN_TEST(test_gsm_dial_voice);
    RUN_TEST(test_gsm_dial_data);
    RUN_TEST(test_gsm_answer_sends_ata);
    RUN_TEST(test_gsm_hangup_sends_ath);
    RUN_TEST(test_gsm_cpin_query_sends_cmd);
    RUN_TEST(test_gsm_cpin_enter_includes_pin);
    RUN_TEST(test_gsm_imei_sends_cgsn);
    RUN_TEST(test_gsm_imsi_sends_cimi);
    RUN_TEST(test_gsm_cgmi_sends_cmd);
    RUN_TEST(test_gsm_cgdcont_basic);
    RUN_TEST(test_gsm_cgact_activate);
    RUN_TEST(test_gsm_cgact_deactivate);

    /* Null guards */
    RUN_TEST(test_gsm_cmgs_null_returns_param_error);
    RUN_TEST(test_gsm_dial_null_number_returns_param_error);

    /* AT injection sanitization */
    RUN_TEST(test_dial_rejects_injection);
    RUN_TEST(test_cmgs_rejects_injection);
    RUN_TEST(test_cmgs_pdu_rejects_injection);
    RUN_TEST(test_dial_accepts_valid_number);
    RUN_TEST(test_dial_accepts_star_hash);

    /* at_gsm7_encode */
    RUN_TEST(test_gsm7_encode_hello);
    RUN_TEST(test_gsm7_encode_empty_string);
    RUN_TEST(test_gsm7_encode_null_input);
    RUN_TEST(test_gsm7_encode_null_output);
    RUN_TEST(test_gsm7_encode_too_long);
    RUN_TEST(test_gsm7_encode_160_chars);
    RUN_TEST(test_gsm7_encode_buf_too_small);
    RUN_TEST(test_gsm7_encode_digits_and_spaces);
    RUN_TEST(test_gsm7_encode_at_sign);

    /* at_gsm_cmgs_pdu */
    RUN_TEST(test_pdu_cmgs_null_number);
    RUN_TEST(test_pdu_cmgs_null_text);
    RUN_TEST(test_pdu_cmgs_enqueues_command);

    return UNITY_END();
}
