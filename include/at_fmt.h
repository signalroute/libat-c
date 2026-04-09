/**
 * @file at_fmt.h
 * @brief Zero-dependency string/integer serializers and parsers.
 *
 * Replaces snprintf/atoi throughout the AT library.
 * No libc beyond <stdint.h> / <stddef.h> / <stdbool.h>.
 *
 * ── Builder ──────────────────────────────────────────────────────────────
 *
 *   char buf[32];
 *   AB_INIT(buf, sizeof(buf));
 *   AB_STR("AT+CFUN="); AB_U8(fun); AB_CHAR(','); AB_U8(rst);
 *   if (!AB_OK()) return AT_ERR_PARAM;  // overflow guard
 *   at_send_raw(AB_DONE(), 0, cb, user);
 *
 * Every AB_* macro short-circuits silently on overflow; AB_OK() lets
 * you test once at the end rather than after every append.
 *
 * ── Parser ───────────────────────────────────────────────────────────────
 *
 *   const char *p = "+CME ERROR: 11";
 *   int32_t code = at__parse_int(&p);   // p advanced past digits
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AT_FMT_H
#define AT_FMT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* =========================================================================
 * Low-level append primitives
 * =========================================================================
 *
 * All functions write into buf[0..size-2] (leave room for NUL) and
 * advance *len.  They return false — and stop writing — on overflow.
 * The caller checks AB_OK() once after the whole sequence.
 */

/**
 * Append a single character.  O(1).
 */
static inline bool at__ab_char(char *buf, size_t size, size_t *len, char c)
{
    if (*len + 1U >= size) { return false; }
    buf[(*len)++] = c;
    return true;
}

/**
 * Append a NUL-terminated string.  O(n).
 */
static inline bool at__ab_str(char *buf, size_t size, size_t *len, const char *s)
{
    while (*s) {
        if (*len + 1U >= size) return false;
        buf[(*len)++] = *s++;
    }
    return true;
}

/**
 * Append a string wrapped in double quotes: "s".  O(n).
 */
static inline bool at__ab_qstr(char *buf, size_t size, size_t *len, const char *s)
{
    return at__ab_char(buf, size, len, '"')
        && at__ab_str (buf, size, len, s)
        && at__ab_char(buf, size, len, '"');
}

/**
 * Append an unsigned 32-bit integer as decimal ASCII.  O(10).
 * Writes digits directly into buf — no intermediate scratch buffer.
 */
static inline bool at__ab_u32(char *buf, size_t size, size_t *len, uint32_t v)
{
    /* Maximum uint32_t is 4294967295 — 10 digits. */
    char tmp[10];
    uint8_t n = 0;
    if (v == 0U) {
        return at__ab_char(buf, size, len, '0');
    }
    while (v > 0U) {
        tmp[n++] = (char)('0' + (v % 10U));
        v /= 10U;
    }
    /* tmp holds digits in reverse; write them forwards */
    while (n--) {
        if (!at__ab_char(buf, size, len, tmp[n])) return false;
    }
    return true;
}

/**
 * Append a signed 32-bit integer as decimal ASCII.  O(11).
 */
static inline bool at__ab_i32(char *buf, size_t size, size_t *len, int32_t v)
{
    if (v < 0) {
        if (!at__ab_char(buf, size, len, '-')) return false;
        /* Careful with INT32_MIN: negate via uint32_t to avoid UB */
        return at__ab_u32(buf, size, len, (uint32_t)(-(v + 1)) + 1U);
    }
    return at__ab_u32(buf, size, len, (uint32_t)v);
}

/* =========================================================================
 * Builder state + macros
 * =========================================================================
 *
 * Declare a builder local to the current scope with AB_INIT, then chain
 * AB_* appends.  AB_OK() tests for overflow.  AB_DONE() NUL-terminates
 * and returns a pointer to the buffer (for passing to at_send_raw).
 */

#define AB_INIT(buf_, size_)                        \
    char *const _ab_buf  = (buf_);                  \
    const size_t _ab_sz  = (size_);                 \
    size_t       _ab_len = 0U;                      \
    bool         _ab_ok  = true

#define AB_CHAR(c)    (_ab_ok = _ab_ok && at__ab_char (_ab_buf, _ab_sz, &_ab_len, (c)))
#define AB_STR(s)     (_ab_ok = _ab_ok && at__ab_str  (_ab_buf, _ab_sz, &_ab_len, (s)))
#define AB_QSTR(s)    (_ab_ok = _ab_ok && at__ab_qstr (_ab_buf, _ab_sz, &_ab_len, (s)))
#define AB_U32(v)     (_ab_ok = _ab_ok && at__ab_u32  (_ab_buf, _ab_sz, &_ab_len, (uint32_t)(v)))
#define AB_I32(v)     (_ab_ok = _ab_ok && at__ab_i32  (_ab_buf, _ab_sz, &_ab_len, (int32_t)(v)))

/* Convenience aliases for typed widths — all go through u32/i32 */
#define AB_U8(v)   AB_U32((uint8_t)(v))
#define AB_U16(v)  AB_U32((uint16_t)(v))
#define AB_I16(v)  AB_I32((int16_t)(v))

#define AB_OK()    (_ab_ok)

/**
 * NUL-terminate and return the buffer pointer.
 * Use only after AB_OK() confirms no overflow.
 */
#define AB_DONE()  (_ab_buf[_ab_len] = '\0', _ab_buf)

/* =========================================================================
 * Integer parser  (replaces atoi — detects overflow, advances pointer)
 * ========================================================================= */

/**
 * Parse a decimal integer from *p (skips leading whitespace and optional '+'/'-').
 * Advances *p past the last consumed digit.
 * Returns 0 on empty input; does not set errno (embedded — no errno).
 */
static inline int32_t at__parse_int(const char **p)
{
    while (**p == ' ' || **p == '\t') (*p)++;
    bool neg = false;
    if (**p == '-') { neg = true; (*p)++; }
    else if (**p == '+') { (*p)++; }

    int32_t v = 0;
    while (**p >= '0' && **p <= '9') {
        /* Saturate at INT32_MAX rather than wrapping */
        if (v <= (0x7FFFFFFF - 9) / 10)
            v = v * 10 + (**p - '0');
        else
            v = 0x7FFFFFFF;
        (*p)++;
    }
    return neg ? -v : v;
}

/**
 * Parse an unsigned decimal integer from *p (no sign, no whitespace skip).
 * Advances *p past the last consumed digit.
 */
static inline uint32_t at__parse_uint(const char **p)
{
    uint32_t v = 0U;
    while (**p >= '0' && **p <= '9') {
        if (v <= (0xFFFFFFFFU - 9U) / 10U)
            v = v * 10U + (uint32_t)(**p - '0');
        else
            v = 0xFFFFFFFFU;
        (*p)++;
    }
    return v;
}

/**
 * Advance *p past the next ',' (and optional surrounding spaces).
 * Returns false if no ',' was found before end-of-string.
 */
static inline bool at__skip_comma(const char **p)
{
    while (**p == ' ') (*p)++;
    if (**p != ',') return false;
    (*p)++;
    while (**p == ' ') (*p)++;
    return true;
}

#endif /* AT_FMT_H */
