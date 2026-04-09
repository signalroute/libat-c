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
    if (!number || !text)      return AT_ERR_PARAM;
    if (strlen(number) > 15U)  return AT_ERR_PARAM;
    if (strlen(text)   > 160U) return AT_ERR_PARAM;
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
