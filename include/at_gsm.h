/**
 * @file at_gsm.h
 * @brief GSM/LTE command helpers over the AT engine (3GPP TS 27.007 / 27.005).
 *
 * Each helper serialises the appropriate AT command string and enqueues it
 * through at_send().  Callers are notified via callback exactly as with
 * raw at_send() calls.
 *
 * Thread-safety
 * =============
 *   All at_gsm_*() helpers call at_send() internally.  They therefore
 *   share the same threading rules as at_send():
 *
 *   • NOT ISR-SAFE — do not call from interrupt context.
 *   • Call from a single task, OR protect all at_gsm_*() / at_send()
 *     calls with a mutex if multiple tasks need to issue commands.
 *   • Completion callbacks are invoked from at_process() (task context).
 *
 * Command coverage
 * ================
 *   General        AT, ATZ, ATE, AT+CMEE, AT+GCAP
 *   SIM / PIN      AT+CPIN, AT+CLCK, AT+CPWD
 *   Network        AT+CREG, AT+CGREG, AT+CEREG, AT+COPS, AT+CSQ, AT+CESQ
 *   Packet data    AT+CGDCONT, AT+CGACT, AT+CGPADDR
 *   SMS            AT+CMGF, AT+CMGS, AT+CMGR, AT+CMGD, AT+CMGL, AT+CNMI
 *   Voice calls    ATD, ATA, ATH, AT+CHUP, AT+CLCC
 *   Clock          AT+CCLK
 *   USSD           AT+CUSD
 *   Power          AT+CFUN
 *   Identification AT+CGSN, AT+CGMI, AT+CGMM, AT+CGMR, AT+CIMI
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AT_GSM_H
#define AT_GSM_H

#include <stdint.h>
#include <stdbool.h>
#include "at.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Parsed structure types
 * ========================================================================= */

/** Signal quality from AT+CSQ. */
typedef struct {
    int16_t rssi_dbm;   /**< dBm (−113 to −51, or −999 = not known)     */
    uint8_t ber;        /**< Bit error rate class 0–7, 99 = not known     */
} at_csq_t;

/** Extended signal quality from AT+CESQ. */
typedef struct {
    int16_t rxlev;       /**< GSM received level (0–63, 99=unknown)       */
    uint8_t ber;         /**< Bit error rate class                        */
    int16_t rscp;        /**< WCDMA received signal code power            */
    int16_t ecno;        /**< WCDMA Ec/No                                 */
    int16_t rsrq;        /**< LTE RSRQ                                    */
    int16_t rsrp;        /**< LTE RSRP                                    */
} at_cesq_t;

/** Network registration status from AT+CREG/CGREG/CEREG. */
typedef enum {
    AT_REG_NOT        = 0,   /**< Not registered, not searching           */
    AT_REG_HOME       = 1,   /**< Registered, home network                */
    AT_REG_SEARCHING  = 2,   /**< Not registered, searching               */
    AT_REG_DENIED     = 3,   /**< Registration denied                     */
    AT_REG_UNKNOWN    = 4,   /**< Unknown                                 */
    AT_REG_ROAMING    = 5,   /**< Registered, roaming                     */
    AT_REG_HOME_SMS   = 6,   /**< Registered for SMS only, home           */
    AT_REG_ROAM_SMS   = 7,   /**< Registered for SMS only, roaming        */
    AT_REG_EMERG      = 8,   /**< Attached for emergency bearer services  */
} at_reg_status_t;

/** SIM PIN status. */
typedef enum {
    AT_CPIN_READY          = 0,
    AT_CPIN_SIM_PIN        = 1,
    AT_CPIN_SIM_PUK        = 2,
    AT_CPIN_PH_SIM_PIN     = 3,
    AT_CPIN_PH_FSIM_PIN    = 4,
    AT_CPIN_PH_FSIM_PUK    = 5,
    AT_CPIN_SIM_PIN2       = 6,
    AT_CPIN_SIM_PUK2       = 7,
    AT_CPIN_PH_NET_PIN     = 8,
    AT_CPIN_PH_NET_PUK     = 9,
    AT_CPIN_PH_NETSUB_PIN  = 10,
    AT_CPIN_PH_NETSUB_PUK  = 11,
    AT_CPIN_PH_SP_PIN      = 12,
    AT_CPIN_PH_SP_PUK      = 13,
    AT_CPIN_PH_CORP_PIN    = 14,
    AT_CPIN_PH_CORP_PUK    = 15,
    AT_CPIN_UNKNOWN        = 0xFF,
} at_cpin_t;

/** Operator information from AT+COPS. */
typedef struct {
    uint8_t  mode;                  /**< 0=auto,1=manual,2=dereg,4=man+auto  */
    uint8_t  format;                /**< 0=long,1=short,2=numeric             */
    char     oper[24];              /**< Operator name/number                 */
    uint8_t  act;                   /**< Access technology (0=GSM,7=LTE,…)   */
} at_cops_t;

/** PDP context definition. */
typedef struct {
    uint8_t  cid;                   /**< Context identifier 1–15              */
    char     pdp_type[8];           /**< "IP","IPV6","IPV4V6","PPP"           */
    char     apn[64];               /**< Access point name                    */
    char     addr[40];              /**< Requested PDP address (may be empty) */
} at_cgdcont_t;

/** SMS message (text mode). */
typedef struct {
    char     oa[20];                /**< Originating address                  */
    char     scts[24];              /**< Service centre timestamp             */
    char     text[161];             /**< Message text (NUL-terminated)        */
    uint8_t  index;                 /**< Message index in storage             */
    uint8_t  stat;                  /**< 0=REC UNREAD,1=REC READ,2=STO UNSENT,3=STO SENT */
} at_sms_t;

/* =========================================================================
 * Parser helpers (return false on parse failure)
 * ========================================================================= */

bool at_parse_csq(const at_response_t *resp, at_csq_t *out);
bool at_parse_cesq(const at_response_t *resp, at_cesq_t *out);
bool at_parse_creg(const at_response_t *resp, at_reg_status_t *out);
bool at_parse_cpin(const at_response_t *resp, at_cpin_t *out);
bool at_parse_cops(const at_response_t *resp, at_cops_t *out);
bool at_parse_cgpaddr(const at_response_t *resp, uint8_t cid,
                      char *ip_buf, size_t ip_buf_sz);
bool at_parse_sms_read(const at_response_t *resp, at_sms_t *out);
bool at_parse_cmgs(const at_response_t *resp, uint8_t *mr_out);

/* =========================================================================
 * General
 * ========================================================================= */

/** Send basic AT (ping). */
at_result_t at_gsm_at(at_cb_t cb, void *user);

/** Factory reset (ATZ). */
at_result_t at_gsm_atz(at_cb_t cb, void *user);

/** Echo mode: enable=true → ATE1, false → ATE0. */
at_result_t at_gsm_echo(bool enable, at_cb_t cb, void *user);

/** Enable extended error codes: mode 0=disabled,1=numeric,2=verbose. */
at_result_t at_gsm_cmee(uint8_t mode, at_cb_t cb, void *user);

/** Query modem capabilities (AT+GCAP). */
at_result_t at_gsm_gcap(at_cb_t cb, void *user);

/* =========================================================================
 * Identification
 * ========================================================================= */

at_result_t at_gsm_imei(at_cb_t cb, void *user);   /**< AT+CGSN  */
at_result_t at_gsm_imsi(at_cb_t cb, void *user);   /**< AT+CIMI  */
at_result_t at_gsm_cgmi(at_cb_t cb, void *user);   /**< Manufacturer  */
at_result_t at_gsm_cgmm(at_cb_t cb, void *user);   /**< Model         */
at_result_t at_gsm_cgmr(at_cb_t cb, void *user);   /**< Revision      */

/**
 * Read ICCID (AT+CCID or AT+ICCID — modem-specific; tries +CCID first).
 * Response contains a 19–20 digit ICCID string.
 */
at_result_t at_gsm_ccid(at_cb_t cb, void *user);

/**
 * Read subscriber phone number(s) (AT+CNUM).
 * Response lines contain the MSISDN stored on the SIM.
 */
at_result_t at_gsm_cnum(at_cb_t cb, void *user);

/* =========================================================================
 * Character set and phonebook
 * ========================================================================= */

/**
 * Set TE character set (AT+CSCS).
 *
 * Common values: "GSM" (GSM-7), "UCS2" (UCS-2 BE hex), "IRA" (ASCII),
 * "8859-1" (Latin-1), "PCCP437" (PC437).
 *
 * @param charset  NUL-terminated charset name (e.g. "GSM", "UCS2").
 */
at_result_t at_gsm_cscs_set(const char *charset, at_cb_t cb, void *user);

/** Query current TE character set (AT+CSCS?). */
at_result_t at_gsm_cscs_query(at_cb_t cb, void *user);

/**
 * Select phonebook memory storage (AT+CPBS).
 *
 * Common storage tags: "SM" (SIM), "ME" (modem/device), "ON" (own numbers),
 * "FD" (fixed-dial), "MC" (missed calls), "RC" (received calls).
 *
 * @param storage  2-character storage tag (e.g. "SM", "ME").
 */
at_result_t at_gsm_cpbs_set(const char *storage, at_cb_t cb, void *user);

/** Query selected phonebook storage and capacity (AT+CPBS?). */
at_result_t at_gsm_cpbs_query(at_cb_t cb, void *user);

/* =========================================================================
 * Power
 * ========================================================================= */

/**
 * Set phone functionality.
 * fun: 0=min,1=full,4=disable RF,7=offline
 * rst: 0=no reset,1=reset before setting
 */
at_result_t at_gsm_cfun(uint8_t fun, uint8_t rst, at_cb_t cb, void *user);

/* =========================================================================
 * SIM / Security
 * ========================================================================= */

/** Query PIN status. */
at_result_t at_gsm_cpin_query(at_cb_t cb, void *user);

/** Enter PIN. */
at_result_t at_gsm_cpin_enter(const char *pin, at_cb_t cb, void *user);

/** Enter PUK and new PIN. */
at_result_t at_gsm_cpin_puk(const char *puk, const char *new_pin, at_cb_t cb, void *user);

/** Facility lock query/set.  fac e.g. "SC"=SIM, "AO"=BAOC. */
at_result_t at_gsm_clck_query(const char *fac, at_cb_t cb, void *user);
at_result_t at_gsm_clck_set(const char *fac, uint8_t mode,
                             const char *passwd, at_cb_t cb, void *user);

/** Change password. */
at_result_t at_gsm_cpwd(const char *fac, const char *old_pw,
                         const char *new_pw, at_cb_t cb, void *user);

/* =========================================================================
 * Network registration & operator
 * ========================================================================= */

/** AT+CREG=<n> (0=disable URC,1=enable,2=+lac/ci). */
at_result_t at_gsm_creg_set(uint8_t n, at_cb_t cb, void *user);
at_result_t at_gsm_creg_query(at_cb_t cb, void *user);

/** AT+CGREG (GPRS). */
at_result_t at_gsm_cgreg_set(uint8_t n, at_cb_t cb, void *user);
at_result_t at_gsm_cgreg_query(at_cb_t cb, void *user);

/** AT+CEREG (EPS / LTE). */
at_result_t at_gsm_cereg_set(uint8_t n, at_cb_t cb, void *user);
at_result_t at_gsm_cereg_query(at_cb_t cb, void *user);

/** AT+COPS=? (scan), AT+COPS? (query), AT+COPS=<mode>[,<format>[,<oper>]]. */
at_result_t at_gsm_cops_query(at_cb_t cb, void *user);
at_result_t at_gsm_cops_auto(at_cb_t cb, void *user);
at_result_t at_gsm_cops_manual(const char *oper, uint8_t act,
                                at_cb_t cb, void *user);

/* =========================================================================
 * Signal quality
 * ========================================================================= */

at_result_t at_gsm_csq(at_cb_t cb, void *user);
at_result_t at_gsm_cesq(at_cb_t cb, void *user);

/* =========================================================================
 * Clock
 * ========================================================================= */

/** Query clock (AT+CCLK?). */
at_result_t at_gsm_cclk_query(at_cb_t cb, void *user);

/** Set clock — time_str format: "yy/MM/dd,hh:mm:ss±zz". */
at_result_t at_gsm_cclk_set(const char *time_str, at_cb_t cb, void *user);

/* =========================================================================
 * Packet data (GPRS/LTE)
 * ========================================================================= */

/** Define PDP context (AT+CGDCONT). */
at_result_t at_gsm_cgdcont(const at_cgdcont_t *ctx, at_cb_t cb, void *user);

/** Activate/deactivate PDP context (AT+CGACT). */
at_result_t at_gsm_cgact(uint8_t cid, bool activate, at_cb_t cb, void *user);

/** Query PDP address (AT+CGPADDR). */
at_result_t at_gsm_cgpaddr(uint8_t cid, at_cb_t cb, void *user);

/* =========================================================================
 * SMS
 * ========================================================================= */

/** Set SMS format: 0=PDU, 1=text. */
at_result_t at_gsm_cmgf(uint8_t mode, at_cb_t cb, void *user);

/**
 * Send SMS in text mode (AT+CMGS).
 * Enqueues two commands internally: CMGS header + body via "> " prompt.
 */
at_result_t at_gsm_cmgs(const char *number, const char *text,
                         at_cb_t cb, void *user);

/** Select SMS memory storage (AT+CPMS).
 *
 * @param mem1  Memory for read/delete ops (e.g. "SM", "ME", "MT").
 * @param mem2  Memory for write/send ops (may be NULL to keep current).
 * @param mem3  Memory for received messages (may be NULL to keep current).
 */
at_result_t at_gsm_cpms(const char *mem1, const char *mem2, const char *mem3,
                         at_cb_t cb, void *user);

/**
 * List SMS messages (AT+CMGL).
 *
 * @param stat  0=REC UNREAD, 1=REC READ, 2=STO UNSENT, 3=STO SENT, 4=ALL
 *              In PDU mode these are the numeric stat values.
 */
at_result_t at_gsm_cmgl(uint8_t stat, at_cb_t cb, void *user);

/**
 * Read a single SMS by index (AT+CMGR).
 *
 * @param index  Message index (1-based per 3GPP TS 27.005 §3.5.5).
 */
at_result_t at_gsm_cmgr(uint8_t index, at_cb_t cb, void *user);

/**
 * Delete SMS message(s) (AT+CMGD).
 *
 * @param index  Message index to delete (1-based).
 * @param delflag  0=delete index, 1=delete all read, 2=delete all read+sent,
 *                 3=delete all read+sent+unsent, 4=delete all.
 */
at_result_t at_gsm_cmgd(uint8_t index, uint8_t delflag, at_cb_t cb, void *user);

/* =========================================================================
 * PDU mode SMS
 * ========================================================================= */

/**
 * GSM-7 alphabet encoder result codes.
 */
typedef enum {
    AT_GSM7_OK           = 0,  /**< Encoding succeeded                    */
    AT_GSM7_ERR_NULL     = 1,  /**< NULL input or output pointer          */
    AT_GSM7_ERR_TOO_LONG = 2,  /**< Input exceeds 160 characters          */
    AT_GSM7_ERR_BUF_FULL = 3,  /**< Output buffer too small               */
    AT_GSM7_ERR_CHAR     = 4,  /**< Character not in GSM-7 alphabet       */
} at_gsm7_result_t;

/**
 * Encode a NUL-terminated ASCII/GSM-7 string into packed GSM-7 septets.
 *
 * @param text     Input string (ASCII, GSM-7 subset).
 * @param out      Output buffer for packed bytes.
 * @param out_sz   Size of @p out in bytes.
 * @param out_len  Bytes written to @p out (packed byte count, not septets).
 * @param n_chars  Number of GSM-7 characters encoded (septet count).
 * @return AT_GSM7_OK on success, error code otherwise.
 *
 * Maximum output: ceil(160 * 7 / 8) = 140 bytes → out_sz >= 140 suffices.
 *
 * Note: '[', ']', '{', '}', '\', '|', '~', '^' are NOT in the GSM-7 basic
 * character set (3GPP TS 23.038 Table 1). They exist only in the extension
 * table (prefixed by 0x1B) and will cause AT_GSM7_ERR_CHAR from this function.
 */
at_gsm7_result_t at_gsm7_encode(const char *text,
                                  uint8_t    *out,
                                  size_t      out_sz,
                                  size_t     *out_len,
                                  size_t     *n_chars);

/**
 * Decode packed GSM-7 septets back into a NUL-terminated ASCII string.
 *
 * Characters that have no ASCII equivalent (e.g. £, ¥, ¡, accented letters)
 * are replaced with '?' in the output.
 *
 * @param packed    Input packed-septet buffer.
 * @param packed_len  Number of packed bytes (NOT septet count).
 * @param n_chars   Number of septets expected (= original character count).
 * @param out       Output buffer for ASCII string (NUL-terminated).
 * @param out_sz    Size of @p out in bytes (must be >= n_chars + 1).
 * @return AT_GSM7_OK on success, AT_GSM7_ERR_NULL or AT_GSM7_ERR_BUF_FULL otherwise.
 */
at_gsm7_result_t at_gsm7_decode(const uint8_t *packed,
                                  size_t         packed_len,
                                  size_t         n_chars,
                                  char          *out,
                                  size_t         out_sz);

/**
 * Check whether all characters in @p text are in the GSM-7 basic character set.
 *
 * @param text  NUL-terminated input string.
 * @return true if every character maps to the GSM-7 basic alphabet; false otherwise.
 */
bool at_gsm7_is_valid(const char *text);

/**
 * Send SMS in PDU mode (AT+CMGF=0 must be active).
 *
 * Builds a minimal SUBMIT PDU (no SMSC number, TP-DA from @p number,
 * TP-DCS=0x00 for GSM-7, TP-VP absent) and issues AT+CMGS=<tpdu_len>.
 *
 * @param smsc    SMSC address string (E.164, e.g. "+4912345678").
 *                Pass NULL or "" to use the SIM's stored SMSC.
 * @param number  Destination number (E.164).
 * @param text    Message text (GSM-7 printable, max 160 chars).
 * @param cb      Completion callback.
 * @param user    User data for @p cb.
 */
at_result_t at_gsm_cmgs_pdu(const char *smsc,
                              const char *number,
                              const char *text,
                              at_cb_t     cb,
                              void       *user);

/** Configure new message indications (AT+CNMI). */
at_result_t at_gsm_cnmi(uint8_t mode, uint8_t mt, uint8_t bm,
                         uint8_t ds, uint8_t bfr, at_cb_t cb, void *user);

/* =========================================================================
 * Multi-part (concatenated) SMS — 3GPP TS 23.040 §9.2.3.24.1
 * ========================================================================= */

/** Maximum number of PDU parts for a single long SMS (up to 255 × 153 chars). */
#define AT_GSM_MAX_PARTS 8U

/**
 * Split and send a long message as concatenated SMS PDUs (PDU mode).
 *
 * Messages longer than 160 GSM-7 chars are automatically split into
 * 153-char segments with a User Data Header (UDH) for reassembly.
 * Messages ≤ 160 chars are sent as a single-part PDU (no UDH).
 *
 * All parts share the same @p ref_id (concatenation reference number).
 * The caller must ensure @p ref_id is unique per destination per session.
 *
 * @param smsc    SMSC address (E.164) or NULL/"" for SIM default.
 * @param number  Destination number (E.164).
 * @param text    Message text (GSM-7 printable, max 153 × AT_GSM_MAX_PARTS chars).
 * @param ref_id  Concatenation reference number (0–255); unique per message.
 * @param cb      Completion callback (called once per part).
 * @param user    User data for @p cb.
 * @return AT_OK if all parts were enqueued successfully; AT_ERR_PARAM on bad args;
 *         AT_ERR_BUSY if the queue cannot fit all parts.
 */
at_result_t at_gsm_send_long(const char *smsc,
                              const char *number,
                              const char *text,
                              uint8_t     ref_id,
                              at_cb_t     cb,
                              void       *user);

/**
 * Return the number of PDU parts required to send @p text.
 *
 * @param text  NUL-terminated GSM-7 string.
 * @return      1 for ≤160 chars; ceil(len/153) for longer; 0 on NULL or
 *              if any character is outside the GSM-7 basic set.
 */
uint8_t at_gsm_part_count(const char *text);

/* =========================================================================
 * Voice calls
 * ========================================================================= */

/** Dial (ATD<number>;  — voice call, ; terminates for voice mode). */
at_result_t at_gsm_dial(const char *number, bool voice, at_cb_t cb, void *user);

/** Answer (ATA). */
at_result_t at_gsm_answer(at_cb_t cb, void *user);

/** Hang up (ATH). */
at_result_t at_gsm_hangup(at_cb_t cb, void *user);

/** List current calls (AT+CLCC). */
at_result_t at_gsm_clcc(at_cb_t cb, void *user);

/**
 * Send DTMF tone(s) during a call (AT+VTS).
 *
 * @param tones  String of DTMF digits (0-9, *, #, A-D).
 *               Each character is sent as a separate tone.
 */
at_result_t at_gsm_vts(const char *tones, at_cb_t cb, void *user);

/**
 * Call Hold and Multiparty (AT+CHLD).
 *
 * @param n  0=release all/held+waiting, 1=release active+accept held,
 *           2=place active on hold+accept held, 3=multiparty conference,
 *           4=ECT (explicit call transfer).
 */
at_result_t at_gsm_chld(uint8_t n, at_cb_t cb, void *user);

/* =========================================================================
 * Supplementary services (call-related)
 * ========================================================================= */

/**
 * Calling Line Identification Presentation (AT+CLIP).
 *
 * @param mode  0=disable URC, 1=enable "+CLIP: ..." URC on incoming calls.
 */
at_result_t at_gsm_clip_set(uint8_t mode, at_cb_t cb, void *user);
at_result_t at_gsm_clip_query(at_cb_t cb, void *user);

/**
 * Calling Line Identification Restriction (AT+CLIR).
 *
 * @param mode  0=CLIR per subscription default, 1=invocation (suppress CLI),
 *              2=suppression (show CLI).
 */
at_result_t at_gsm_clir_set(uint8_t mode, at_cb_t cb, void *user);
at_result_t at_gsm_clir_query(at_cb_t cb, void *user);

/**
 * Call Waiting (AT+CCWA).
 *
 * @param enable   0=disable, 1=enable.
 * @param mode     0=disable query, 1=enable URC notification, 2=query status.
 * @param class_x  Bit field of service class (1=voice, 2=data, 4=fax, …).
 */
at_result_t at_gsm_ccwa_set(uint8_t enable, uint8_t mode, uint8_t class_x,
                              at_cb_t cb, void *user);
at_result_t at_gsm_ccwa_query(at_cb_t cb, void *user);

/* =========================================================================
 * SIM file access (AT+CRSM — 3GPP TS 27.007 §8.18)
 * ========================================================================= */

/**
 * Restricted SIM access command (AT+CRSM).
 *
 * Allows reading/writing SIM Elementary Files via the ISO 7816-4 command set.
 *
 * @param command   SIM command code:
 *                  176=READ BINARY, 178=READ RECORD, 192=GET RESPONSE,
 *                  214=UPDATE BINARY, 220=UPDATE RECORD, 242=STATUS.
 * @param fileid    EF identifier (e.g. 0x6F07 for EF_IMSI, 0x6FAD for EF_AD).
 * @param p1,p2,p3 Command parameters per ISO 7816-4 / ETSI TS 102.221.
 * @param data      Optional hex-encoded data string (for UPDATE commands), or NULL.
 */
at_result_t at_gsm_crsm(uint8_t command, uint16_t fileid,
                          uint8_t p1, uint8_t p2, uint8_t p3,
                          const char *data, at_cb_t cb, void *user);

/* =========================================================================
 * USSD
 * ========================================================================= */

/** Send USSD string (AT+CUSD=1,"<str>",<dcs>). */
at_result_t at_gsm_cusd(const char *ussd_str, uint8_t dcs,
                         at_cb_t cb, void *user);

/** Cancel pending USSD session (AT+CUSD=2). */
at_result_t at_gsm_cusd_cancel(at_cb_t cb, void *user);

/**
 * List all supported AT commands (AT+CLAC).
 * Response is a multi-line list of command names.
 */
at_result_t at_gsm_clac(at_cb_t cb, void *user);

/**
 * Report mobile equipment error cause (AT+CEER).
 * Returns a vendor-specific string describing the last call-release cause.
 */
at_result_t at_gsm_ceer(at_cb_t cb, void *user);

/* =========================================================================
 * SMS timestamp (SCTS) parser
 * ========================================================================= */

/** Parsed Service Centre Time Stamp from +CMGR / +CMT responses. */
typedef struct {
    uint8_t year;       /**< Year (00–99, offset from 2000)        */
    uint8_t month;      /**< Month (1–12)                          */
    uint8_t day;        /**< Day   (1–31)                          */
    uint8_t hour;       /**< Hour  (0–23)                          */
    uint8_t minute;     /**< Minute (0–59)                         */
    uint8_t second;     /**< Second (0–59)                         */
    int8_t  tz_quarter; /**< Timezone offset in quarter-hours (-48..+48) */
} at_scts_t;

/**
 * Parse a GSM SCTS timestamp string (3GPP TS 23.040 §9.2.3.11).
 *
 * Accepts the canonical form: "yy/MM/dd,hh:mm:ss±zz"
 * where ±zz is the timezone offset in quarter-hours.
 *
 * @param scts_str  NUL-terminated timestamp string.
 * @param out       Output structure (filled on success).
 * @return true on success, false if @p scts_str is NULL or malformed.
 */
bool at_parse_scts(const char *scts_str, at_scts_t *out);

#ifdef __cplusplus
}
#endif

#endif /* AT_GSM_H */
