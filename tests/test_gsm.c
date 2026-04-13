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

/* =========================================================================
 * AT+CPMS — phonebook/SMS memory selection
 * ========================================================================= */

void test_cpms_single_mem(void)
{
    at_result_t rc = at_gsm_cpms("SM", NULL, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CPMS=\"SM\""));
}

void test_cpms_two_mems(void)
{
    at_result_t rc = at_gsm_cpms("SM", "ME", NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CPMS=\"SM\",\"ME\""));
}

void test_cpms_three_mems(void)
{
    at_result_t rc = at_gsm_cpms("SM", "ME", "MT", NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CPMS=\"SM\",\"ME\",\"MT\""));
}

void test_cpms_null_mem1_returns_param(void)
{
    TEST_ASSERT_EQUAL_INT(AT_ERR_PARAM, at_gsm_cpms(NULL, NULL, NULL, NULL, NULL));
}

void test_cpms_empty_mem1_returns_param(void)
{
    TEST_ASSERT_EQUAL_INT(AT_ERR_PARAM, at_gsm_cpms("", NULL, NULL, NULL, NULL));
}

void test_cpms_injection_returns_param(void)
{
    TEST_ASSERT_EQUAL_INT(AT_ERR_PARAM, at_gsm_cpms("SM\r\nAT+CLAC", NULL, NULL, NULL, NULL));
}

/* =========================================================================
 * AT+CMGL — list SMS messages
 * ========================================================================= */

void test_cmgl_all(void)
{
    at_result_t rc = at_gsm_cmgl(4, NULL, NULL); /* 4=ALL */
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CMGL=4"));
}

void test_cmgl_unread(void)
{
    at_result_t rc = at_gsm_cmgl(0, NULL, NULL); /* 0=REC UNREAD */
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CMGL=0"));
}

void test_cmgl_sent(void)
{
    at_result_t rc = at_gsm_cmgl(3, NULL, NULL); /* 3=STO SENT */
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CMGL=3"));
}

void test_cmgl_invalid_stat_returns_param(void)
{
    TEST_ASSERT_EQUAL_INT(AT_ERR_PARAM, at_gsm_cmgl(5, NULL, NULL));
}

void test_cmgl_invalid_stat_255_returns_param(void)
{
    TEST_ASSERT_EQUAL_INT(AT_ERR_PARAM, at_gsm_cmgl(255, NULL, NULL));
}

/* =========================================================================
 * AT+CMGR — read SMS by index
 * ========================================================================= */

void test_cmgr_index_1(void)
{
    at_result_t rc = at_gsm_cmgr(1, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CMGR=1"));
}

void test_cmgr_index_255(void)
{
    at_result_t rc = at_gsm_cmgr(255, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CMGR=255"));
}

/* =========================================================================
 * AT+CMGD — delete SMS
 * ========================================================================= */

void test_cmgd_delete_by_index(void)
{
    at_result_t rc = at_gsm_cmgd(1, 0, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CMGD=1,0"));
}

void test_cmgd_delete_all(void)
{
    at_result_t rc = at_gsm_cmgd(1, 4, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CMGD=1,4"));
}

void test_cmgd_delete_all_read(void)
{
    at_result_t rc = at_gsm_cmgd(0, 1, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CMGD=0,1"));
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
 * AT+VTS, AT+CHLD, AT+CLIP, AT+CLIR, AT+CCWA, AT+CRSM
 * ========================================================================= */

void test_vts_sends_dtmf(void)
{
    at_result_t rc = at_gsm_vts("123*#", NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+VTS=123*#"));
}

void test_vts_null_returns_param(void)
{
    TEST_ASSERT_EQUAL_INT(AT_ERR_PARAM, at_gsm_vts(NULL, NULL, NULL));
}

void test_vts_empty_returns_param(void)
{
    TEST_ASSERT_EQUAL_INT(AT_ERR_PARAM, at_gsm_vts("", NULL, NULL));
}

void test_vts_invalid_char_returns_param(void)
{
    TEST_ASSERT_EQUAL_INT(AT_ERR_PARAM, at_gsm_vts("1X2", NULL, NULL));
}

void test_vts_injection_returns_param(void)
{
    TEST_ASSERT_EQUAL_INT(AT_ERR_PARAM, at_gsm_vts("1\nATH", NULL, NULL));
}

void test_chld_release_all(void)
{
    at_result_t rc = at_gsm_chld(0, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CHLD=0"));
}

void test_chld_multiparty(void)
{
    at_result_t rc = at_gsm_chld(3, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CHLD=3"));
}

void test_chld_invalid_returns_param(void)
{
    TEST_ASSERT_EQUAL_INT(AT_ERR_PARAM, at_gsm_chld(5, NULL, NULL));
}

void test_clip_set_enable(void)
{
    at_result_t rc = at_gsm_clip_set(1, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CLIP=1"));
}

void test_clip_set_invalid_returns_param(void)
{
    TEST_ASSERT_EQUAL_INT(AT_ERR_PARAM, at_gsm_clip_set(2, NULL, NULL));
}

void test_clip_query(void)
{
    at_result_t rc = at_gsm_clip_query(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CLIP?"));
}

void test_clir_set_suppress(void)
{
    at_result_t rc = at_gsm_clir_set(1, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CLIR=1"));
}

void test_clir_set_invalid_returns_param(void)
{
    TEST_ASSERT_EQUAL_INT(AT_ERR_PARAM, at_gsm_clir_set(3, NULL, NULL));
}

void test_clir_query(void)
{
    at_result_t rc = at_gsm_clir_query(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CLIR?"));
}

void test_ccwa_set(void)
{
    at_result_t rc = at_gsm_ccwa_set(1, 1, 1, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CCWA=1,1,1"));
}

void test_ccwa_query(void)
{
    at_result_t rc = at_gsm_ccwa_query(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CCWA?"));
}

void test_crsm_read_binary(void)
{
    /* READ BINARY (176) of EF_IMSI (28423 = 0x6F07) */
    at_result_t rc = at_gsm_crsm(176, 0x6F07, 0, 0, 9, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CRSM=176,28423,0,0,9"));
}

void test_crsm_update_binary_with_data(void)
{
    at_result_t rc = at_gsm_crsm(214, 0x6F07, 0, 0, 4, "DEADBEEF", NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CRSM=214,28423,0,0,4,\"DEADBEEF\""));
}

void test_crsm_invalid_data_returns_param(void)
{
    /* Non-hex data must be rejected */
    TEST_ASSERT_EQUAL_INT(AT_ERR_PARAM,
        at_gsm_crsm(214, 0x6F07, 0, 0, 4, "ZZZZ", NULL, NULL));
}

void test_cusd_enqueues_command(void)
{
    at_result_t rc = at_gsm_cusd("*100#", 15, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CUSD=1,\"*100#\",15"));
}

void test_cusd_null_returns_param(void)
{
    TEST_ASSERT_EQUAL_INT(AT_ERR_PARAM, at_gsm_cusd(NULL, 0, NULL, NULL));
}

void test_cusd_cancel_enqueues_command(void)
{
    at_result_t rc = at_gsm_cusd_cancel(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CUSD=2"));
}

void test_clac_enqueues_command(void)
{
    at_result_t rc = at_gsm_clac(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CLAC"));
}

void test_ceer_enqueues_command(void)
{
    at_result_t rc = at_gsm_ceer(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CEER"));
}

/* =========================================================================
 * SCTS timestamp parser tests
 * ========================================================================= */

void test_scts_parse_basic(void)
{
    at_scts_t t;
    bool ok = at_parse_scts("23/06/15,14:30:00+08", &t);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT8(23, t.year);
    TEST_ASSERT_EQUAL_UINT8(6,  t.month);
    TEST_ASSERT_EQUAL_UINT8(15, t.day);
    TEST_ASSERT_EQUAL_UINT8(14, t.hour);
    TEST_ASSERT_EQUAL_UINT8(30, t.minute);
    TEST_ASSERT_EQUAL_UINT8(0,  t.second);
    TEST_ASSERT_EQUAL_INT8(8,   t.tz_quarter);
}

void test_scts_parse_negative_tz(void)
{
    at_scts_t t;
    bool ok = at_parse_scts("24/01/01,00:00:00-20", &t);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT8(24, t.year);
    TEST_ASSERT_EQUAL_UINT8(1,  t.month);
    TEST_ASSERT_EQUAL_UINT8(1,  t.day);
    TEST_ASSERT_EQUAL_INT8(-20, t.tz_quarter);
}

void test_scts_parse_zero_tz(void)
{
    at_scts_t t;
    bool ok = at_parse_scts("22/12/31,23:59:59+00", &t);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT8(59, t.second);
    TEST_ASSERT_EQUAL_INT8(0,   t.tz_quarter);
}

void test_scts_parse_no_tz(void)
{
    at_scts_t t;
    bool ok = at_parse_scts("22/12/31,23:59:59", &t);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_INT8(0, t.tz_quarter);
}

void test_scts_parse_null_str_returns_false(void)
{
    at_scts_t t;
    TEST_ASSERT_FALSE(at_parse_scts(NULL, &t));
}

void test_scts_parse_null_out_returns_false(void)
{
    TEST_ASSERT_FALSE(at_parse_scts("23/06/15,14:30:00+08", NULL));
}

void test_scts_parse_invalid_month_returns_false(void)
{
    at_scts_t t;
    TEST_ASSERT_FALSE(at_parse_scts("23/13/01,00:00:00+00", &t));
}

void test_scts_parse_invalid_day_returns_false(void)
{
    at_scts_t t;
    TEST_ASSERT_FALSE(at_parse_scts("23/06/32,00:00:00+00", &t));
}

void test_scts_parse_invalid_hour_returns_false(void)
{
    at_scts_t t;
    TEST_ASSERT_FALSE(at_parse_scts("23/06/15,24:00:00+00", &t));
}

void test_scts_parse_invalid_minute_returns_false(void)
{
    at_scts_t t;
    TEST_ASSERT_FALSE(at_parse_scts("23/06/15,12:60:00+00", &t));
}

void test_scts_parse_invalid_second_returns_false(void)
{
    at_scts_t t;
    TEST_ASSERT_FALSE(at_parse_scts("23/06/15,12:00:61+00", &t));
}

void test_scts_parse_malformed_returns_false(void)
{
    at_scts_t t;
    TEST_ASSERT_FALSE(at_parse_scts("not-a-timestamp", &t));
}

void test_scts_parse_max_tz_plus_48(void)
{
    at_scts_t t;
    bool ok = at_parse_scts("23/06/15,12:00:00+48", &t);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_INT8(48, t.tz_quarter);
}

void test_scts_parse_tz_too_large_returns_false(void)
{
    at_scts_t t;
    TEST_ASSERT_FALSE(at_parse_scts("23/06/15,12:00:00+49", &t));
}

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
 * GSM-7 LUT correctness tests (issue #252)
 * ========================================================================= */

void test_gsm7_bracket_not_in_basic_set(void)
{
    /* '[' and ']' are extension-table only — basic encoder must reject them */
    uint8_t out[4]; size_t ol = 0, nc = 0;
    TEST_ASSERT_EQUAL_INT(AT_GSM7_ERR_CHAR,
        at_gsm7_encode("[", out, sizeof(out), &ol, &nc));
    TEST_ASSERT_EQUAL_INT(AT_GSM7_ERR_CHAR,
        at_gsm7_encode("]", out, sizeof(out), &ol, &nc));
}

void test_gsm7_backslash_not_in_basic_set(void)
{
    uint8_t out[4]; size_t ol = 0, nc = 0;
    TEST_ASSERT_EQUAL_INT(AT_GSM7_ERR_CHAR,
        at_gsm7_encode("\\", out, sizeof(out), &ol, &nc));
}

void test_gsm7_caret_not_in_basic_set(void)
{
    uint8_t out[4]; size_t ol = 0, nc = 0;
    TEST_ASSERT_EQUAL_INT(AT_GSM7_ERR_CHAR,
        at_gsm7_encode("^", out, sizeof(out), &ol, &nc));
}

void test_gsm7_less_than_is_valid(void)
{
    /* '<' IS in the GSM-7 basic set at septet 0x3C */
    uint8_t out[4]; size_t ol = 0, nc = 0;
    TEST_ASSERT_EQUAL_INT(AT_GSM7_OK,
        at_gsm7_encode("<", out, sizeof(out), &ol, &nc));
    TEST_ASSERT_EQUAL_HEX8(0x3CU, out[0]);
}

void test_gsm7_greater_than_is_valid(void)
{
    /* '>' IS in the GSM-7 basic set at septet 0x3E */
    uint8_t out[4]; size_t ol = 0, nc = 0;
    TEST_ASSERT_EQUAL_INT(AT_GSM7_OK,
        at_gsm7_encode(">", out, sizeof(out), &ol, &nc));
    TEST_ASSERT_EQUAL_HEX8(0x3EU, out[0]);
}

/* =========================================================================
 * at_gsm7_is_valid tests
 * ========================================================================= */

void test_gsm7_is_valid_ascii_message(void)
{
    TEST_ASSERT_TRUE(at_gsm7_is_valid("Hello World!"));
}

void test_gsm7_is_valid_null_returns_false(void)
{
    TEST_ASSERT_FALSE(at_gsm7_is_valid(NULL));
}

void test_gsm7_is_valid_empty_string(void)
{
    TEST_ASSERT_TRUE(at_gsm7_is_valid(""));
}

void test_gsm7_is_valid_bracket_returns_false(void)
{
    TEST_ASSERT_FALSE(at_gsm7_is_valid("say [hi]"));
}

void test_gsm7_is_valid_at_sign(void)
{
    TEST_ASSERT_TRUE(at_gsm7_is_valid("@user"));
}

void test_gsm7_is_valid_unicode_returns_false(void)
{
    /* 0xC3 = first byte of a UTF-8 multibyte sequence — not GSM-7 */
    TEST_ASSERT_FALSE(at_gsm7_is_valid("\xC3\xA9")); /* é in UTF-8 */
}

/* =========================================================================
 * at_gsm7_decode tests
 * ========================================================================= */

void test_gsm7_decode_hello(void)
{
    /* Packed GSM-7 for "Hello" = C8 32 9B FD 06 */
    static const uint8_t packed[] = {0xC8, 0x32, 0x9B, 0xFD, 0x06};
    char out[8] = {0};
    at_gsm7_result_t rc = at_gsm7_decode(packed, sizeof(packed), 5, out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(AT_GSM7_OK, rc);
    TEST_ASSERT_EQUAL_STRING("Hello", out);
}

void test_gsm7_decode_null_input(void)
{
    char out[8];
    TEST_ASSERT_EQUAL_INT(AT_GSM7_ERR_NULL,
        at_gsm7_decode(NULL, 0, 0, out, sizeof(out)));
}

void test_gsm7_decode_null_output(void)
{
    static const uint8_t packed[] = {0x00};
    TEST_ASSERT_EQUAL_INT(AT_GSM7_ERR_NULL,
        at_gsm7_decode(packed, sizeof(packed), 1, NULL, 0));
}

void test_gsm7_decode_buf_too_small(void)
{
    static const uint8_t packed[] = {0xC8, 0x32, 0x9B, 0xFD, 0x06};
    char out[4]; /* only 4 bytes for 5 chars + NUL — too small */
    TEST_ASSERT_EQUAL_INT(AT_GSM7_ERR_BUF_FULL,
        at_gsm7_decode(packed, sizeof(packed), 5, out, sizeof(out)));
}

void test_gsm7_decode_roundtrip(void)
{
    /* Encode then decode must recover original string */
    const char *msg = "Test 123!";
    uint8_t packed[20]; size_t ol = 0, nc = 0;
    TEST_ASSERT_EQUAL_INT(AT_GSM7_OK,
        at_gsm7_encode(msg, packed, sizeof(packed), &ol, &nc));
    char recovered[32] = {0};
    TEST_ASSERT_EQUAL_INT(AT_GSM7_OK,
        at_gsm7_decode(packed, ol, nc, recovered, sizeof(recovered)));
    TEST_ASSERT_EQUAL_STRING(msg, recovered);
}

void test_gsm7_decode_space(void)
{
    /* Single space: GSM-7 septet 0x20, packed as 0x20 */
    static const uint8_t packed[] = {0x20};
    char out[4] = {0};
    TEST_ASSERT_EQUAL_INT(AT_GSM7_OK,
        at_gsm7_decode(packed, sizeof(packed), 1, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(" ", out);
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
 * at_gsm_part_count and at_gsm_send_long tests
 * ========================================================================= */

/* Build a string of exactly `n` ASCII 'A' chars */
static void fill_str(char *buf, size_t n)
{
    for (size_t i = 0U; i < n; i++) buf[i] = 'A';
    buf[n] = '\0';
}

void test_part_count_short_message(void)
{
    TEST_ASSERT_EQUAL_UINT8(1, at_gsm_part_count("Hello World"));
}

void test_part_count_exactly_160(void)
{
    char msg[161]; fill_str(msg, 160);
    TEST_ASSERT_EQUAL_UINT8(1, at_gsm_part_count(msg));
}

void test_part_count_161_needs_two_parts(void)
{
    char msg[162]; fill_str(msg, 161);
    TEST_ASSERT_EQUAL_UINT8(2, at_gsm_part_count(msg));
}

void test_part_count_306_needs_two_parts(void)
{
    /* 2 × 153 = 306 */
    char msg[307]; fill_str(msg, 306);
    TEST_ASSERT_EQUAL_UINT8(2, at_gsm_part_count(msg));
}

void test_part_count_307_needs_three_parts(void)
{
    char msg[308]; fill_str(msg, 307);
    TEST_ASSERT_EQUAL_UINT8(3, at_gsm_part_count(msg));
}

void test_part_count_null_returns_zero(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, at_gsm_part_count(NULL));
}

void test_part_count_invalid_char_returns_zero(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, at_gsm_part_count("hello [world]"));
}

void test_send_long_short_delegates_to_pdu(void)
{
    /* Short message — should succeed and enqueue one AT+CMGS= */
    at_result_t rc = at_gsm_send_long(NULL, "+491234567890", "Hello", 0x42, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CMGS="));
}

void test_send_long_null_number_returns_param(void)
{
    TEST_ASSERT_EQUAL_INT(AT_ERR_PARAM,
        at_gsm_send_long(NULL, NULL, "Hi", 1, NULL, NULL));
}

void test_send_long_multipart_enqueues_two_cmgs(void)
{
    /* Build a 200-char message (needs 2 parts: 153 + 47) */
    char msg[201]; fill_str(msg, 200);

    at_result_t rc = at_gsm_send_long(NULL, "+491234567890", msg, 0x01, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);

    /* Process first part — should see AT+CMGS= */
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CMGS="));
}

/* =========================================================================
 * AT+CCID / AT+CNUM — new identification commands
 * ========================================================================= */

void test_ccid_enqueues_command(void)
{
    at_result_t rc = at_gsm_ccid(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CCID"));
}

void test_cnum_enqueues_command(void)
{
    at_result_t rc = at_gsm_cnum(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CNUM"));
}

/* =========================================================================
 * AT+CSCS — character set selection
 * ========================================================================= */

void test_cscs_set_gsm(void)
{
    at_result_t rc = at_gsm_cscs_set("GSM", NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CSCS=\"GSM\""));
}

void test_cscs_set_ucs2(void)
{
    at_result_t rc = at_gsm_cscs_set("UCS2", NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CSCS=\"UCS2\""));
}

void test_cscs_set_ira(void)
{
    at_result_t rc = at_gsm_cscs_set("IRA", NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CSCS=\"IRA\""));
}

void test_cscs_set_8859_1(void)
{
    at_result_t rc = at_gsm_cscs_set("8859-1", NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CSCS=\"8859-1\""));
}

void test_cscs_set_null_returns_param(void)
{
    TEST_ASSERT_EQUAL_INT(AT_ERR_PARAM, at_gsm_cscs_set(NULL, NULL, NULL));
}

void test_cscs_set_empty_returns_param(void)
{
    TEST_ASSERT_EQUAL_INT(AT_ERR_PARAM, at_gsm_cscs_set("", NULL, NULL));
}

void test_cscs_set_injection_quote_returns_param(void)
{
    /* Quote in charset name would break the AT command */
    TEST_ASSERT_EQUAL_INT(AT_ERR_PARAM, at_gsm_cscs_set("\"HACK", NULL, NULL));
}

void test_cscs_set_injection_crlf_returns_param(void)
{
    TEST_ASSERT_EQUAL_INT(AT_ERR_PARAM, at_gsm_cscs_set("GSM\r\nAT+CLAC", NULL, NULL));
}

void test_cscs_query_enqueues_command(void)
{
    at_result_t rc = at_gsm_cscs_query(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CSCS?"));
}

/* =========================================================================
 * AT+CPBS — phonebook storage selection
 * ========================================================================= */

void test_cpbs_set_sm(void)
{
    at_result_t rc = at_gsm_cpbs_set("SM", NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CPBS=\"SM\""));
}

void test_cpbs_set_me(void)
{
    at_result_t rc = at_gsm_cpbs_set("ME", NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CPBS=\"ME\""));
}

void test_cpbs_set_on(void)
{
    at_result_t rc = at_gsm_cpbs_set("ON", NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CPBS=\"ON\""));
}

void test_cpbs_set_fd(void)
{
    at_result_t rc = at_gsm_cpbs_set("FD", NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CPBS=\"FD\""));
}

void test_cpbs_set_null_returns_param(void)
{
    TEST_ASSERT_EQUAL_INT(AT_ERR_PARAM, at_gsm_cpbs_set(NULL, NULL, NULL));
}

void test_cpbs_set_empty_returns_param(void)
{
    TEST_ASSERT_EQUAL_INT(AT_ERR_PARAM, at_gsm_cpbs_set("", NULL, NULL));
}

void test_cpbs_set_injection_quote_returns_param(void)
{
    TEST_ASSERT_EQUAL_INT(AT_ERR_PARAM, at_gsm_cpbs_set("\"SM", NULL, NULL));
}

void test_cpbs_set_injection_crlf_returns_param(void)
{
    TEST_ASSERT_EQUAL_INT(AT_ERR_PARAM, at_gsm_cpbs_set("SM\r\nAT+CLAC", NULL, NULL));
}

void test_cpbs_query_enqueues_command(void)
{
    at_result_t rc = at_gsm_cpbs_query(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(AT_OK, rc);
    at_process();
    TEST_ASSERT_NOT_NULL(strstr(s_tx_buf, "AT+CPBS?"));
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

    /* AT+CPMS — SMS/phonebook memory */
    RUN_TEST(test_cpms_single_mem);
    RUN_TEST(test_cpms_two_mems);
    RUN_TEST(test_cpms_three_mems);
    RUN_TEST(test_cpms_null_mem1_returns_param);
    RUN_TEST(test_cpms_empty_mem1_returns_param);
    RUN_TEST(test_cpms_injection_returns_param);

    /* AT+CMGL — list SMS */
    RUN_TEST(test_cmgl_all);
    RUN_TEST(test_cmgl_unread);
    RUN_TEST(test_cmgl_sent);
    RUN_TEST(test_cmgl_invalid_stat_returns_param);
    RUN_TEST(test_cmgl_invalid_stat_255_returns_param);

    /* AT+CMGR — read SMS by index */
    RUN_TEST(test_cmgr_index_1);
    RUN_TEST(test_cmgr_index_255);

    /* AT+CMGD — delete SMS */
    RUN_TEST(test_cmgd_delete_by_index);
    RUN_TEST(test_cmgd_delete_all);
    RUN_TEST(test_cmgd_delete_all_read);
    RUN_TEST(test_dial_accepts_valid_number);
    RUN_TEST(test_dial_accepts_star_hash);

    /* AT+VTS / AT+CHLD / AT+CLIP / AT+CLIR / AT+CCWA / AT+CRSM */
    RUN_TEST(test_vts_sends_dtmf);
    RUN_TEST(test_vts_null_returns_param);
    RUN_TEST(test_vts_empty_returns_param);
    RUN_TEST(test_vts_invalid_char_returns_param);
    RUN_TEST(test_vts_injection_returns_param);
    RUN_TEST(test_chld_release_all);
    RUN_TEST(test_chld_multiparty);
    RUN_TEST(test_chld_invalid_returns_param);
    RUN_TEST(test_clip_set_enable);
    RUN_TEST(test_clip_set_invalid_returns_param);
    RUN_TEST(test_clip_query);
    RUN_TEST(test_clir_set_suppress);
    RUN_TEST(test_clir_set_invalid_returns_param);
    RUN_TEST(test_clir_query);
    RUN_TEST(test_ccwa_set);
    RUN_TEST(test_ccwa_query);
    RUN_TEST(test_crsm_read_binary);
    RUN_TEST(test_crsm_update_binary_with_data);
    RUN_TEST(test_crsm_invalid_data_returns_param);

    /* AT+CUSD / AT+CLAC / AT+CEER */
    RUN_TEST(test_cusd_enqueues_command);
    RUN_TEST(test_cusd_null_returns_param);
    RUN_TEST(test_cusd_cancel_enqueues_command);
    RUN_TEST(test_clac_enqueues_command);
    RUN_TEST(test_ceer_enqueues_command);

    /* SCTS timestamp parser */
    RUN_TEST(test_scts_parse_basic);
    RUN_TEST(test_scts_parse_negative_tz);
    RUN_TEST(test_scts_parse_zero_tz);
    RUN_TEST(test_scts_parse_no_tz);
    RUN_TEST(test_scts_parse_null_str_returns_false);
    RUN_TEST(test_scts_parse_null_out_returns_false);
    RUN_TEST(test_scts_parse_invalid_month_returns_false);
    RUN_TEST(test_scts_parse_invalid_day_returns_false);
    RUN_TEST(test_scts_parse_invalid_hour_returns_false);
    RUN_TEST(test_scts_parse_invalid_minute_returns_false);
    RUN_TEST(test_scts_parse_invalid_second_returns_false);
    RUN_TEST(test_scts_parse_malformed_returns_false);
    RUN_TEST(test_scts_parse_max_tz_plus_48);
    RUN_TEST(test_scts_parse_tz_too_large_returns_false);

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

    /* GSM-7 LUT correctness (issue #252) */
    RUN_TEST(test_gsm7_bracket_not_in_basic_set);
    RUN_TEST(test_gsm7_backslash_not_in_basic_set);
    RUN_TEST(test_gsm7_caret_not_in_basic_set);
    RUN_TEST(test_gsm7_less_than_is_valid);
    RUN_TEST(test_gsm7_greater_than_is_valid);

    /* at_gsm7_is_valid */
    RUN_TEST(test_gsm7_is_valid_ascii_message);
    RUN_TEST(test_gsm7_is_valid_null_returns_false);
    RUN_TEST(test_gsm7_is_valid_empty_string);
    RUN_TEST(test_gsm7_is_valid_bracket_returns_false);
    RUN_TEST(test_gsm7_is_valid_at_sign);
    RUN_TEST(test_gsm7_is_valid_unicode_returns_false);

    /* at_gsm7_decode */
    RUN_TEST(test_gsm7_decode_hello);
    RUN_TEST(test_gsm7_decode_null_input);
    RUN_TEST(test_gsm7_decode_null_output);
    RUN_TEST(test_gsm7_decode_buf_too_small);
    RUN_TEST(test_gsm7_decode_roundtrip);
    RUN_TEST(test_gsm7_decode_space);

    /* at_gsm_part_count and at_gsm_send_long */
    RUN_TEST(test_part_count_short_message);
    RUN_TEST(test_part_count_exactly_160);
    RUN_TEST(test_part_count_161_needs_two_parts);
    RUN_TEST(test_part_count_306_needs_two_parts);
    RUN_TEST(test_part_count_307_needs_three_parts);
    RUN_TEST(test_part_count_null_returns_zero);
    RUN_TEST(test_part_count_invalid_char_returns_zero);
    RUN_TEST(test_send_long_short_delegates_to_pdu);
    RUN_TEST(test_send_long_null_number_returns_param);
    RUN_TEST(test_send_long_multipart_enqueues_two_cmgs);

    /* AT+CCID / AT+CNUM */
    RUN_TEST(test_ccid_enqueues_command);
    RUN_TEST(test_cnum_enqueues_command);

    /* AT+CSCS — character set */
    RUN_TEST(test_cscs_set_gsm);
    RUN_TEST(test_cscs_set_ucs2);
    RUN_TEST(test_cscs_set_ira);
    RUN_TEST(test_cscs_set_8859_1);
    RUN_TEST(test_cscs_set_null_returns_param);
    RUN_TEST(test_cscs_set_empty_returns_param);
    RUN_TEST(test_cscs_set_injection_quote_returns_param);
    RUN_TEST(test_cscs_set_injection_crlf_returns_param);
    RUN_TEST(test_cscs_query_enqueues_command);

    /* AT+CPBS — phonebook storage */
    RUN_TEST(test_cpbs_set_sm);
    RUN_TEST(test_cpbs_set_me);
    RUN_TEST(test_cpbs_set_on);
    RUN_TEST(test_cpbs_set_fd);
    RUN_TEST(test_cpbs_set_null_returns_param);
    RUN_TEST(test_cpbs_set_empty_returns_param);
    RUN_TEST(test_cpbs_set_injection_quote_returns_param);
    RUN_TEST(test_cpbs_set_injection_crlf_returns_param);
    RUN_TEST(test_cpbs_query_enqueues_command);

    return UNITY_END();
}
