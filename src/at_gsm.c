/**
 * @file at_gsm.c
 * @brief GSM/LTE AT command helpers — zero-dependency implementation.
 *
 * No snprintf, no atoi, no stdio.h.
 * Integer serialisation → at_fmt.h builder (AB_* macros).
 * Integer parsing       → at__parse_int / at__parse_uint.
 *
 * SPDX-License-Identifier: MIT
 */

#include "at_gsm.h"
#include "at_fmt.h"

#include <string.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/**
 * Validate a phone-number string.
 * Rejects any character outside [0-9+*#,;] to prevent AT command injection.
 * Returns false if number is NULL, empty, or contains illegal characters.
 */
static bool validate_phone_number(const char *number)
{
    if (!number || number[0] == '\0') return false;
    for (const char *p = number; *p != '\0'; p++) {
        char c = *p;
        if (c >= '0' && c <= '9') continue;
        if (c == '+' || c == '*' || c == '#' || c == ',' || c == ';') continue;
        return false;
    }
    return true;
}

static const char *resp_find_value(const at_response_t *resp, const char *prefix)
{
    size_t plen = strlen(prefix);
    for (uint8_t i = 0; i < resp->num_lines; i++) {
        const char *l = resp->lines[i];
        if (strncmp(l, prefix, plen) != 0) continue;
        l += plen;
        if (*l == ':') l++;
        if (*l == ' ') l++;
        return l;
    }
    return NULL;
}

static bool parse_quoted(const char **p, char *buf, size_t buf_sz)
{
    if (**p != '"') return false;
    (*p)++;
    const char *start = *p;
    while (**p && **p != '"') (*p)++;
    size_t len = (size_t)(*p - start);
    if (**p == '"') (*p)++;
    if (len >= buf_sz) len = buf_sz - 1U;
    memcpy(buf, start, len);
    buf[len] = '\0';
    return true;
}

/* =========================================================================
 * Parsers
 * ========================================================================= */

bool at_parse_csq(const at_response_t *resp, at_csq_t *out)
{
    if (!resp || !out || resp->status != AT_OK) return false;
    const char *p = resp_find_value(resp, "+CSQ");
    if (!p) return false;
    uint32_t rssi = at__parse_uint(&p);
    if (!at__skip_comma(&p)) return false;
    uint32_t ber  = at__parse_uint(&p);
    out->ber      = (uint8_t)(ber > 99U ? 99U : ber);
    out->rssi_dbm = (rssi == 99U) ? -999 : (int16_t)(-113 + (int16_t)(rssi * 2U));
    return true;
}

bool at_parse_cesq(const at_response_t *resp, at_cesq_t *out)
{
    if (!resp || !out || resp->status != AT_OK) return false;
    const char *p = resp_find_value(resp, "+CESQ");
    if (!p) return false;
    out->rxlev = (int16_t)at__parse_int(&p);  at__skip_comma(&p);
    out->ber   = (uint8_t)at__parse_uint(&p); at__skip_comma(&p);
    out->rscp  = (int16_t)at__parse_int(&p);  at__skip_comma(&p);
    out->ecno  = (int16_t)at__parse_int(&p);  at__skip_comma(&p);
    out->rsrq  = (int16_t)at__parse_int(&p);  at__skip_comma(&p);
    out->rsrp  = (int16_t)at__parse_int(&p);
    return true;
}

bool at_parse_creg(const at_response_t *resp, at_reg_status_t *out)
{
    if (!resp || !out || resp->status != AT_OK) return false;
    const char *p = resp_find_value(resp, "+CREG");
    if (!p) p = resp_find_value(resp, "+CGREG");
    if (!p) p = resp_find_value(resp, "+CEREG");
    if (!p) return false;
    uint32_t a = at__parse_uint(&p);
    *out = at__skip_comma(&p) ? (at_reg_status_t)at__parse_uint(&p)
                              : (at_reg_status_t)a;
    return true;
}

bool at_parse_cpin(const at_response_t *resp, at_cpin_t *out)
{
    if (!resp || !out || resp->status != AT_OK) return false;
    const char *p = resp_find_value(resp, "+CPIN");
    if (!p) return false;

    static const struct { const char *str; uint8_t len; at_cpin_t val; } map[] = {
        { "READY",       5,  AT_CPIN_READY       },
        { "SIM PIN",     7,  AT_CPIN_SIM_PIN     },
        { "SIM PUK",     7,  AT_CPIN_SIM_PUK     },
        { "SIM PIN2",    8,  AT_CPIN_SIM_PIN2    },
        { "SIM PUK2",    8,  AT_CPIN_SIM_PUK2    },
        { "PH-SIM PIN", 10,  AT_CPIN_PH_SIM_PIN  },
        { "PH-NET PIN", 10,  AT_CPIN_PH_NET_PIN  },
        { "PH-NET PUK", 10,  AT_CPIN_PH_NET_PUK  },
    };
    for (size_t i = 0; i < sizeof(map)/sizeof(map[0]); i++) {
        if (strncmp(p, map[i].str, map[i].len) == 0) { *out = map[i].val; return true; }
    }
    *out = AT_CPIN_UNKNOWN;
    return true;
}

bool at_parse_cops(const at_response_t *resp, at_cops_t *out)
{
    if (!resp || !out || resp->status != AT_OK) return false;
    const char *p = resp_find_value(resp, "+COPS");
    if (!p) return false;

    out->mode = (uint8_t)at__parse_uint(&p);
    if (!at__skip_comma(&p)) { out->format = 0; out->oper[0] = '\0'; out->act = 0xFFU; return true; }
    out->format = (uint8_t)at__parse_uint(&p);
    if (!at__skip_comma(&p)) { out->oper[0] = '\0'; out->act = 0xFFU; return true; }

    if (*p == '"') {
        parse_quoted(&p, out->oper, sizeof(out->oper));
    } else {
        char *d = out->oper, *end = out->oper + sizeof(out->oper) - 1U;
        while (*p && *p != ',' && d < end) *d++ = *p++;
        *d = '\0';
    }
    out->act = at__skip_comma(&p) ? (uint8_t)at__parse_uint(&p) : 0xFFU;
    return true;
}

bool at_parse_cgpaddr(const at_response_t *resp, uint8_t cid,
                       char *ip_buf, size_t ip_buf_sz)
{
    if (!resp || !ip_buf || resp->status != AT_OK) return false;
    for (uint8_t i = 0; i < resp->num_lines; i++) {
        const char *l = resp->lines[i];
        if (strncmp(l, "+CGPADDR:", 9) != 0) continue;
        l += 9; if (*l == ' ') l++;
        if ((uint8_t)at__parse_uint(&l) != cid) continue;
        if (!at__skip_comma(&l)) return false;
        if (*l == '"') { parse_quoted(&l, ip_buf, ip_buf_sz); }
        else {
            size_t n = strlen(l);
            if (n >= ip_buf_sz) n = ip_buf_sz - 1U;
            memcpy(ip_buf, l, n); ip_buf[n] = '\0';
        }
        return true;
    }
    return false;
}

bool at_parse_sms_read(const at_response_t *resp, at_sms_t *out)
{
    if (!resp || !out || resp->status != AT_OK || resp->num_lines < 2U) return false;
    const char *l = resp->lines[0];
    if (strncmp(l, "+CMGR:", 6) != 0) return false;
    l += 6; if (*l == ' ') l++;

    if (*l == '"') {
        char ss[16]; parse_quoted(&l, ss, sizeof(ss));
        if      (strncmp(ss, "REC UNREAD", 10) == 0) out->stat = 0;
        else if (strncmp(ss, "REC READ",    8) == 0) out->stat = 1;
        else if (strncmp(ss, "STO UNSENT", 10) == 0) out->stat = 2;
        else                                          out->stat = 3;
        at__skip_comma(&l);
    } else {
        out->stat = (uint8_t)at__parse_uint(&l);
        at__skip_comma(&l);
    }
    parse_quoted(&l, out->oa, sizeof(out->oa));
    at__skip_comma(&l);
    if (*l == '"') { while (*l && *l != ',') l++; }  /* skip alpha */
    at__skip_comma(&l);
    parse_quoted(&l, out->scts, sizeof(out->scts));

    size_t tlen = strlen(resp->lines[1]);
    if (tlen >= sizeof(out->text)) tlen = sizeof(out->text) - 1U;
    memcpy(out->text, resp->lines[1], tlen);
    out->text[tlen] = '\0';
    return true;
}

bool at_parse_cmgs(const at_response_t *resp, uint8_t *mr_out)
{
    if (!resp || !mr_out || resp->status != AT_OK) return false;
    const char *p = resp_find_value(resp, "+CMGS");
    if (!p) return false;
    *mr_out = (uint8_t)at__parse_uint(&p);
    return true;
}

/* =========================================================================
 * General
 * ========================================================================= */

at_result_t at_gsm_at(at_cb_t cb, void *user)    { return at_send_raw("AT",    0, cb, user); }
at_result_t at_gsm_atz(at_cb_t cb, void *user)   { return at_send_raw("ATZ", 2000U, cb, user); }
at_result_t at_gsm_gcap(at_cb_t cb, void *user)  { return at_send_raw("AT+GCAP", 0, cb, user); }

at_result_t at_gsm_echo(bool enable, at_cb_t cb, void *user)
{
    return at_send_raw(enable ? "ATE1" : "ATE0", 0, cb, user);
}

at_result_t at_gsm_cmee(uint8_t mode, at_cb_t cb, void *user)
{
    char buf[12]; AB_INIT(buf, sizeof(buf));
    AB_STR("AT+CMEE="); AB_U8(mode);
    if (!AB_OK()) return AT_ERR_PARAM;
    return at_send_raw(AB_DONE(), 0, cb, user);
}

/* =========================================================================
 * Identification
 * ========================================================================= */

at_result_t at_gsm_imei(at_cb_t cb, void *user) { return at_send_raw("AT+CGSN", 0, cb, user); }
at_result_t at_gsm_imsi(at_cb_t cb, void *user) { return at_send_raw("AT+CIMI", 0, cb, user); }
at_result_t at_gsm_cgmi(at_cb_t cb, void *user) { return at_send_raw("AT+CGMI", 0, cb, user); }
at_result_t at_gsm_cgmm(at_cb_t cb, void *user) { return at_send_raw("AT+CGMM", 0, cb, user); }
at_result_t at_gsm_cgmr(at_cb_t cb, void *user) { return at_send_raw("AT+CGMR", 0, cb, user); }

/* =========================================================================
 * Power
 * ========================================================================= */

at_result_t at_gsm_cfun(uint8_t fun, uint8_t rst, at_cb_t cb, void *user)
{
    char buf[16]; AB_INIT(buf, sizeof(buf));
    AB_STR("AT+CFUN="); AB_U8(fun);
    if (rst) { AB_CHAR(','); AB_U8(rst); }
    if (!AB_OK()) return AT_ERR_PARAM;
    return at_send_raw(AB_DONE(), (fun == 1U && rst == 1U) ? 10000U : 0U, cb, user);
}

/* =========================================================================
 * SIM / Security
 * ========================================================================= */

at_result_t at_gsm_cpin_query(at_cb_t cb, void *user)
{
    return at_send_raw("AT+CPIN?", 0, cb, user);
}

at_result_t at_gsm_cpin_enter(const char *pin, at_cb_t cb, void *user)
{
    if (!pin || strlen(pin) > 8U) return AT_ERR_PARAM;
    char buf[20]; AB_INIT(buf, sizeof(buf));
    AB_STR("AT+CPIN="); AB_QSTR(pin);
    if (!AB_OK()) return AT_ERR_PARAM;
    return at_send_raw(AB_DONE(), 5000U, cb, user);
}

at_result_t at_gsm_cpin_puk(const char *puk, const char *new_pin, at_cb_t cb, void *user)
{
    if (!puk || !new_pin) return AT_ERR_PARAM;
    char buf[32]; AB_INIT(buf, sizeof(buf));
    AB_STR("AT+CPIN="); AB_QSTR(puk); AB_CHAR(','); AB_QSTR(new_pin);
    if (!AB_OK()) return AT_ERR_PARAM;
    return at_send_raw(AB_DONE(), 5000U, cb, user);
}

at_result_t at_gsm_clck_query(const char *fac, at_cb_t cb, void *user)
{
    if (!fac) return AT_ERR_PARAM;
    char buf[20]; AB_INIT(buf, sizeof(buf));
    AB_STR("AT+CLCK="); AB_QSTR(fac); AB_STR(",2");
    if (!AB_OK()) return AT_ERR_PARAM;
    return at_send_raw(AB_DONE(), 0, cb, user);
}

at_result_t at_gsm_clck_set(const char *fac, uint8_t mode,
                              const char *passwd, at_cb_t cb, void *user)
{
    if (!fac) return AT_ERR_PARAM;
    char buf[40]; AB_INIT(buf, sizeof(buf));
    AB_STR("AT+CLCK="); AB_QSTR(fac); AB_CHAR(','); AB_U8(mode);
    if (passwd && passwd[0]) { AB_CHAR(','); AB_QSTR(passwd); }
    if (!AB_OK()) return AT_ERR_PARAM;
    return at_send_raw(AB_DONE(), 30000U, cb, user);
}

at_result_t at_gsm_cpwd(const char *fac, const char *old_pw,
                          const char *new_pw, at_cb_t cb, void *user)
{
    if (!fac || !old_pw || !new_pw) return AT_ERR_PARAM;
    char buf[56]; AB_INIT(buf, sizeof(buf));
    AB_STR("AT+CPWD="); AB_QSTR(fac); AB_CHAR(','); AB_QSTR(old_pw); AB_CHAR(','); AB_QSTR(new_pw);
    if (!AB_OK()) return AT_ERR_PARAM;
    return at_send_raw(AB_DONE(), 0, cb, user);
}

/* =========================================================================
 * Network registration
 * ========================================================================= */

at_result_t at_gsm_creg_set(uint8_t n, at_cb_t cb, void *user)
{
    char buf[12]; AB_INIT(buf, sizeof(buf));
    AB_STR("AT+CREG="); AB_U8(n);
    if (!AB_OK()) return AT_ERR_PARAM;
    return at_send_raw(AB_DONE(), 0, cb, user);
}
at_result_t at_gsm_creg_query(at_cb_t cb, void *user)  { return at_send_raw("AT+CREG?",  0, cb, user); }

at_result_t at_gsm_cgreg_set(uint8_t n, at_cb_t cb, void *user)
{
    char buf[13]; AB_INIT(buf, sizeof(buf));
    AB_STR("AT+CGREG="); AB_U8(n);
    if (!AB_OK()) return AT_ERR_PARAM;
    return at_send_raw(AB_DONE(), 0, cb, user);
}
at_result_t at_gsm_cgreg_query(at_cb_t cb, void *user) { return at_send_raw("AT+CGREG?", 0, cb, user); }

at_result_t at_gsm_cereg_set(uint8_t n, at_cb_t cb, void *user)
{
    char buf[13]; AB_INIT(buf, sizeof(buf));
    AB_STR("AT+CEREG="); AB_U8(n);
    if (!AB_OK()) return AT_ERR_PARAM;
    return at_send_raw(AB_DONE(), 0, cb, user);
}
at_result_t at_gsm_cereg_query(at_cb_t cb, void *user) { return at_send_raw("AT+CEREG?", 0, cb, user); }

/* =========================================================================
 * Operator selection
 * ========================================================================= */

at_result_t at_gsm_cops_query(at_cb_t cb, void *user) { return at_send_raw("AT+COPS?", 0, cb, user); }
at_result_t at_gsm_cops_auto(at_cb_t cb, void *user)  { return at_send_raw("AT+COPS=0", 180000U, cb, user); }

at_result_t at_gsm_cops_manual(const char *oper, uint8_t act, at_cb_t cb, void *user)
{
    if (!oper) return AT_ERR_PARAM;
    char buf[40]; AB_INIT(buf, sizeof(buf));
    AB_STR("AT+COPS=1,2,"); AB_QSTR(oper); AB_CHAR(','); AB_U8(act);
    if (!AB_OK()) return AT_ERR_PARAM;
    return at_send_raw(AB_DONE(), 180000U, cb, user);
}

/* =========================================================================
 * Signal quality
 * ========================================================================= */

at_result_t at_gsm_csq(at_cb_t cb, void *user)  { return at_send_raw("AT+CSQ",  0, cb, user); }
at_result_t at_gsm_cesq(at_cb_t cb, void *user) { return at_send_raw("AT+CESQ", 0, cb, user); }

/* =========================================================================
 * Clock
 * ========================================================================= */

at_result_t at_gsm_cclk_query(at_cb_t cb, void *user) { return at_send_raw("AT+CCLK?", 0, cb, user); }

at_result_t at_gsm_cclk_set(const char *time_str, at_cb_t cb, void *user)
{
    if (!time_str) return AT_ERR_PARAM;
    char buf[40]; AB_INIT(buf, sizeof(buf));
    AB_STR("AT+CCLK="); AB_QSTR(time_str);
    if (!AB_OK()) return AT_ERR_PARAM;
    return at_send_raw(AB_DONE(), 0, cb, user);
}

/* =========================================================================
 * Packet data
 * ========================================================================= */

at_result_t at_gsm_cgdcont(const at_cgdcont_t *ctx, at_cb_t cb, void *user)
{
    if (!ctx) return AT_ERR_PARAM;
    char buf[160]; AB_INIT(buf, sizeof(buf));
    AB_STR("AT+CGDCONT="); AB_U8(ctx->cid);
    AB_CHAR(','); AB_QSTR(ctx->pdp_type);
    AB_CHAR(','); AB_QSTR(ctx->apn);
    if (ctx->addr[0]) { AB_CHAR(','); AB_QSTR(ctx->addr); }
    if (!AB_OK()) return AT_ERR_PARAM;
    return at_send_raw(AB_DONE(), 0, cb, user);
}

at_result_t at_gsm_cgact(uint8_t cid, bool activate, at_cb_t cb, void *user)
{
    char buf[20]; AB_INIT(buf, sizeof(buf));
    AB_STR("AT+CGACT="); AB_U8(activate ? 1U : 0U); AB_CHAR(','); AB_U8(cid);
    if (!AB_OK()) return AT_ERR_PARAM;
    return at_send_raw(AB_DONE(), 150000U, cb, user);
}

at_result_t at_gsm_cgpaddr(uint8_t cid, at_cb_t cb, void *user)
{
    char buf[18]; AB_INIT(buf, sizeof(buf));
    AB_STR("AT+CGPADDR="); AB_U8(cid);
    if (!AB_OK()) return AT_ERR_PARAM;
    return at_send_raw(AB_DONE(), 0, cb, user);
}

/* =========================================================================
 * SMS
 * ========================================================================= */

at_result_t at_gsm_cmgf(uint8_t mode, at_cb_t cb, void *user)
{
    char buf[12]; AB_INIT(buf, sizeof(buf));
    AB_STR("AT+CMGF="); AB_U8(mode);
    if (!AB_OK()) return AT_ERR_PARAM;
    return at_send_raw(AB_DONE(), 0, cb, user);
}

at_result_t at_gsm_cmgs(const char *number, const char *text, at_cb_t cb, void *user)
{
    if (!number || !text)           return AT_ERR_PARAM;
    if (!validate_phone_number(number)) return AT_ERR_PARAM;
    if (strlen(number) > 15U)       return AT_ERR_PARAM;
    if (strlen(text)   > 160U)      return AT_ERR_PARAM;
    char cmd[32]; AB_INIT(cmd, sizeof(cmd));
    AB_STR("AT+CMGS="); AB_QSTR(number);
    if (!AB_OK()) return AT_ERR_PARAM;
    at_cmd_desc_t d = {
        .cmd        = AB_DONE(),
        .body       = text,
        .timeout_ms = AT_CFG_PROMPT_TIMEOUT_MS,
        .prompt     = AT_PROMPT_SMS,
        .cb         = cb,
        .user       = user,
    };
    return at_send(&d);
}

at_result_t at_gsm_cmgr(uint8_t index, at_cb_t cb, void *user)
{
    char buf[14]; AB_INIT(buf, sizeof(buf));
    AB_STR("AT+CMGR="); AB_U8(index);
    if (!AB_OK()) return AT_ERR_PARAM;
    return at_send_raw(AB_DONE(), 0, cb, user);
}

at_result_t at_gsm_cmgd(uint8_t index, uint8_t delflag, at_cb_t cb, void *user)
{
    char buf[18]; AB_INIT(buf, sizeof(buf));
    AB_STR("AT+CMGD="); AB_U8(index); AB_CHAR(','); AB_U8(delflag);
    if (!AB_OK()) return AT_ERR_PARAM;
    return at_send_raw(AB_DONE(), 25000U, cb, user);
}

at_result_t at_gsm_cmgl(const char *stat, at_cb_t cb, void *user)
{
    if (!stat) return AT_ERR_PARAM;
    char buf[32]; AB_INIT(buf, sizeof(buf));
    AB_STR("AT+CMGL="); AB_QSTR(stat);
    if (!AB_OK()) return AT_ERR_PARAM;
    return at_send_raw(AB_DONE(), 0, cb, user);
}

at_result_t at_gsm_cnmi(uint8_t mode, uint8_t mt, uint8_t bm,
                          uint8_t ds, uint8_t bfr, at_cb_t cb, void *user)
{
    char buf[24]; AB_INIT(buf, sizeof(buf));
    AB_STR("AT+CNMI=");
    AB_U8(mode); AB_CHAR(','); AB_U8(mt); AB_CHAR(',');
    AB_U8(bm);   AB_CHAR(','); AB_U8(ds); AB_CHAR(','); AB_U8(bfr);
    if (!AB_OK()) return AT_ERR_PARAM;
    return at_send_raw(AB_DONE(), 0, cb, user);
}

/* =========================================================================
 * Voice calls
 * ========================================================================= */

at_result_t at_gsm_dial(const char *number, bool voice, at_cb_t cb, void *user)
{
    if (!number) return AT_ERR_PARAM;
    if (!validate_phone_number(number)) return AT_ERR_PARAM;
    char buf[32]; AB_INIT(buf, sizeof(buf));
    AB_STR("ATD"); AB_STR(number);
    if (voice) AB_CHAR(';');
    if (!AB_OK()) return AT_ERR_PARAM;
    return at_send_raw(AB_DONE(), 30000U, cb, user);
}

at_result_t at_gsm_answer(at_cb_t cb, void *user) { return at_send_raw("ATA",    30000U, cb, user); }
at_result_t at_gsm_hangup(at_cb_t cb, void *user) { return at_send_raw("ATH",    0,      cb, user); }
at_result_t at_gsm_clcc(at_cb_t cb, void *user)   { return at_send_raw("AT+CLCC",0,      cb, user); }

/* =========================================================================
 * USSD
 * ========================================================================= */

at_result_t at_gsm_cusd(const char *ussd_str, uint8_t dcs, at_cb_t cb, void *user)
{
    if (!ussd_str) return AT_ERR_PARAM;
    char buf[80]; AB_INIT(buf, sizeof(buf));
    AB_STR("AT+CUSD=1,"); AB_QSTR(ussd_str); AB_CHAR(','); AB_U8(dcs);
    if (!AB_OK()) return AT_ERR_PARAM;
    return at_send_raw(AB_DONE(), 30000U, cb, user);
}

/* =========================================================================
 * GSM-7 encoder
 * =========================================================================
 *
 * GSM 03.38 Basic Character Set.  Each character is 7 bits; 8 characters
 * pack into 7 bytes (56 bits).  The table maps ASCII / Latin-1 code points
 * to GSM-7 septet values; unmappable characters are not supported (caller
 * should pre-screen or use UCS-2 DCS instead).
 */

/* Returns GSM-7 septet for an ASCII character, or 0xFF if not in alphabet. */
static uint8_t gsm7_char_to_septet(char c)
{
    /* Basic Character Set lookup — GSM 03.38 Table 1 */
    static const uint8_t lut[128] = {
        /* 0x00 */ 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
        /* 0x08 */ 0xFF,0xFF,0x0A,0xFF,0xFF,0x0D,0xFF,0xFF,
        /* 0x10 */ 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
        /* 0x18 */ 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
        /* 0x20 */ 0x20,0x21,0x22,0x23,0x02,0x25,0x26,0x27, /*  !"#$%&' */
        /* 0x28 */ 0x28,0x29,0x2A,0x2B,0x2C,0x2D,0x2E,0x2F, /* ()*+,-./ */
        /* 0x30 */ 0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37, /* 01234567 */
        /* 0x38 */ 0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F, /* 89:;<=>? */
        /* 0x40 */ 0x00,0x41,0x42,0x43,0x44,0x45,0x46,0x47, /* @ABCDEFG */
        /* 0x48 */ 0x48,0x49,0x4A,0x4B,0x4C,0x4D,0x4E,0x4F, /* HIJKLMNO */
        /* 0x50 */ 0x50,0x51,0x52,0x53,0x54,0x55,0x56,0x57, /* PQRSTUVW */
        /* 0x58 */ 0x58,0x59,0x5A,0x3C,0xFF,0x3E,0xFF,0x11, /* XYZ<\>^_ */
        /* 0x60 */ 0xFF,0x61,0x62,0x63,0x64,0x65,0x66,0x67, /* `abcdefg */
        /* 0x68 */ 0x68,0x69,0x6A,0x6B,0x6C,0x6D,0x6E,0x6F, /* hijklmno */
        /* 0x70 */ 0x70,0x71,0x72,0x73,0x74,0x75,0x76,0x77, /* pqrstuvw */
        /* 0x78 */ 0x78,0x79,0x7A,0xFF,0xFF,0xFF,0xFF,0xFF, /* xyz      */
    };
    if ((uint8_t)c >= 128U) return 0xFFU;
    return lut[(uint8_t)c];
}

at_gsm7_result_t at_gsm7_encode(const char *text,
                                  uint8_t    *out,
                                  size_t      out_sz,
                                  size_t     *out_len,
                                  size_t     *n_chars)
{
    if (!text || !out || !out_len || !n_chars) return AT_GSM7_ERR_NULL;

    size_t in_len = strlen(text);
    if (in_len > 160U) return AT_GSM7_ERR_TOO_LONG;

    /* Maximum packed bytes needed: ceil(in_len * 7 / 8) */
    size_t max_out = (in_len * 7U + 7U) / 8U;
    if (max_out > out_sz) return AT_GSM7_ERR_BUF_FULL;

    memset(out, 0, max_out);

    size_t bit_pos = 0U;
    for (size_t i = 0U; i < in_len; i++) {
        uint8_t sept = gsm7_char_to_septet(text[i]);
        if (sept == 0xFFU) return AT_GSM7_ERR_CHAR;

        /* Pack 7-bit septet at bit_pos within out[] */
        size_t byte_off = bit_pos / 8U;
        size_t bit_off  = bit_pos % 8U;

        out[byte_off] |= (uint8_t)(sept << bit_off);
        if (bit_off > 1U && byte_off + 1U < out_sz) {
            out[byte_off + 1U] |= (uint8_t)(sept >> (8U - bit_off));
        }
        bit_pos += 7U;
    }

    *out_len = (bit_pos + 7U) / 8U;
    *n_chars = in_len;
    return AT_GSM7_OK;
}

/* =========================================================================
 * PDU mode CMGS
 * ========================================================================= */

/* Encode a phone number (E.164) as GSM semi-octet BCD.
 * Writes type-of-address byte + BCD digits into buf, returns bytes written.
 * buf must be at least 12 bytes (1 type + up to 11 BCD bytes for 20 digits).
 */
static size_t encode_address(const char *number, uint8_t *buf)
{
    /* Skip leading '+' */
    if (*number == '+') number++;
    size_t len = strlen(number);
    if (len > 20U) len = 20U;

    /* Type-of-address: 0x91 = international, 0x81 = unknown */
    buf[0] = 0x91U; /* assume international E.164 */
    size_t bcd_bytes = (len + 1U) / 2U;
    for (size_t i = 0U; i < bcd_bytes; i++) {
        uint8_t lo = (uint8_t)(number[2U * i]       - '0') & 0x0FU;
        uint8_t hi = (2U * i + 1U < len)
                   ? (uint8_t)((number[2U * i + 1U] - '0') & 0x0FU)
                   : 0x0FU; /* pad odd digit with F */
        buf[1U + i] = (uint8_t)((hi << 4U) | lo);
    }
    return 1U + bcd_bytes;
}

/* Hex-encode bytes → ASCII hex string in buf. Returns chars written. */
static size_t hex_encode(const uint8_t *src, size_t src_len, char *buf, size_t buf_sz)
{
    static const char hx[] = "0123456789ABCDEF";
    size_t out = 0U;
    for (size_t i = 0U; i < src_len && (out + 2U) < buf_sz; i++) {
        buf[out++] = hx[(src[i] >> 4U) & 0x0FU];
        buf[out++] = hx[ src[i]        & 0x0FU];
    }
    buf[out] = '\0';
    return out;
}

at_result_t at_gsm_cmgs_pdu(const char *smsc,
                              const char *number,
                              const char *text,
                              at_cb_t     cb,
                              void       *user)
{
    if (!number || !text) return AT_ERR_PARAM;
    if (!validate_phone_number(number)) return AT_ERR_PARAM;

    /* ── 1. Build PDU in a local buffer ──────────────────────────────── */
    /*
     * PDU layout (SUBMIT, no SMSC, no VP, GSM-7):
     *   [SMSC info] [MTI|VPF] [MR] [DA len] [DA type] [DA BCD...] [PID] [DCS] [UDL] [UD...]
     *
     * SMSC info: if smsc is NULL/"", write 0x00 (use SIM SMSC).
     *            otherwise: length byte + type + BCD.
     *
     * We use a static 256-byte raw PDU buffer.  Maximum:
     *   1 (smsc=0)  + 1 (mti) + 1 (mr) + 1 (da_len) + 12 (da) +
     *   1 (pid) + 1 (dcs) + 1 (udl) + 140 (ud) = 159 bytes.
     */
    uint8_t pdu[200U];
    size_t  p = 0U;

    /* SMSC field */
    if (!smsc || smsc[0] == '\0') {
        pdu[p++] = 0x00U; /* use stored SMSC */
    } else {
        uint8_t smsc_addr[14U];
        size_t  smsc_len = encode_address(smsc, smsc_addr);
        pdu[p++] = (uint8_t)smsc_len;
        memcpy(&pdu[p], smsc_addr, smsc_len);
        p += smsc_len;
    }

    /* MTI = SMS-SUBMIT (0x01), no VPF, no SRR, no UDHI, no RP */
    pdu[p++] = 0x01U;

    /* Message Reference — 0 = let modem assign */
    pdu[p++] = 0x00U;

    /* Destination Address */
    {
        const char *num = number;
        if (*num == '+') num++;
        size_t digits = strlen(num);
        if (digits > 20U) return AT_ERR_PARAM;

        pdu[p++] = (uint8_t)strlen(num); /* length in digits (original, not BCD bytes) */
        uint8_t da[14U];
        size_t da_len = encode_address(number, da);
        memcpy(&pdu[p], da, da_len);
        p += da_len;
    }

    /* PID = 0x00 (normal SMS) */
    pdu[p++] = 0x00U;

    /* DCS = 0x00 (GSM-7, no compression, class 0) */
    pdu[p++] = 0x00U;

    /* Encode GSM-7 */
    uint8_t ud[140U];
    size_t  ud_len = 0U, n_chars = 0U;
    at_gsm7_result_t grc = at_gsm7_encode(text, ud, sizeof(ud), &ud_len, &n_chars);
    if (grc != AT_GSM7_OK) return AT_ERR_PARAM;

    /* UDL = number of septets */
    pdu[p++] = (uint8_t)n_chars;

    /* UD = packed septets */
    memcpy(&pdu[p], ud, ud_len);
    p += ud_len;

    /* ── 2. TP-PDU length (bytes after SMSC field, used in AT+CMGS=<n>) */
    /* tpdu_len = total pdu bytes MINUS the smsc prefix bytes */
    uint8_t smsc_field_len = pdu[0]; /* 0 = just 1 byte (the 0x00), else 1+smsc_field_len */
    size_t  smsc_prefix    = 1U + (smsc_field_len == 0U ? 0U : smsc_field_len);
    size_t  tpdu_len       = p - smsc_prefix;

    /* ── 3. Hex-encode the PDU ──────────────────────────────────────── */
    char hex[400U + 1U]; /* 200 bytes × 2 hex chars + NUL */
    hex_encode(pdu, p, hex, sizeof(hex));

    /* ── 4. Build AT+CMGS=<tpdu_len> command ──────────────────────── */
    char cmd[24U]; AB_INIT(cmd, sizeof(cmd));
    AB_STR("AT+CMGS="); AB_U32((uint32_t)tpdu_len);
    if (!AB_OK()) return AT_ERR_PARAM;

    /* ── 5. Enqueue: CMGS header → GSM "> " prompt → hex body + Ctrl-Z */
    at_cmd_desc_t d = {
        .cmd        = cmd,
        .body       = hex, /* engine sends this after the '>' prompt */
        .timeout_ms = AT_CFG_PROMPT_TIMEOUT_MS,
        .prompt     = AT_PROMPT_SMS,
        .cb         = cb,
        .user       = user,
    };
    return at_send(&d);
}
