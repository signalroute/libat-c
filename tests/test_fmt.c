/**
 * @file test_fmt.c
 * @brief Unit tests for at_fmt.h — builder macros + integer parsers.
 *
 * SPDX-License-Identifier: MIT
 */

#include "unity.h"
#include "at_fmt.h"

#include <string.h>
#include <stdint.h>

void setUp(void)    {}
void tearDown(void) {}

/* =========================================================================
 * at__ab_char
 * ========================================================================= */

void test_ab_char_single(void)
{
    char buf[4];
    AB_INIT(buf, sizeof(buf));
    AB_CHAR('X');
    TEST_ASSERT_TRUE(AB_OK());
    TEST_ASSERT_EQUAL_CHAR('X', buf[0]);
}

void test_ab_char_fills_exactly(void)
{
    char buf[3]; /* fits 2 chars + NUL */
    AB_INIT(buf, sizeof(buf));
    AB_CHAR('A'); AB_CHAR('B');
    TEST_ASSERT_TRUE(AB_OK());
    (void)AB_DONE();
    TEST_ASSERT_EQUAL_STRING("AB", buf);
}

void test_ab_char_overflow(void)
{
    char buf[2]; /* room for 1 char only */
    AB_INIT(buf, sizeof(buf));
    AB_CHAR('A'); AB_CHAR('B'); /* B overflows */
    TEST_ASSERT_FALSE(AB_OK());
}

/* =========================================================================
 * at__ab_str
 * ========================================================================= */

void test_ab_str_basic(void)
{
    char buf[16];
    AB_INIT(buf, sizeof(buf));
    AB_STR("AT+CMGF=");
    TEST_ASSERT_TRUE(AB_OK());
    (void)AB_DONE();
    TEST_ASSERT_EQUAL_STRING("AT+CMGF=", buf);
}

void test_ab_str_empty(void)
{
    char buf[4];
    AB_INIT(buf, sizeof(buf));
    AB_STR("");
    TEST_ASSERT_TRUE(AB_OK());
    (void)AB_DONE();
    TEST_ASSERT_EQUAL_STRING("", buf);
}

void test_ab_str_overflow_stops_early(void)
{
    char buf[5]; /* fits "ABCD" + NUL */
    AB_INIT(buf, sizeof(buf));
    AB_STR("ABCDE"); /* E overflows */
    TEST_ASSERT_FALSE(AB_OK());
}

/* =========================================================================
 * at__ab_qstr
 * ========================================================================= */

void test_ab_qstr_basic(void)
{
    char buf[16];
    AB_INIT(buf, sizeof(buf));
    AB_QSTR("+49171");
    TEST_ASSERT_TRUE(AB_OK());
    (void)AB_DONE();
    TEST_ASSERT_EQUAL_STRING("\"+49171\"", buf);
}

void test_ab_qstr_empty_string(void)
{
    char buf[4];
    AB_INIT(buf, sizeof(buf));
    AB_QSTR(""); /* produces "" — 2 chars */
    TEST_ASSERT_TRUE(AB_OK());
    (void)AB_DONE();
    TEST_ASSERT_EQUAL_STRING("\"\"", buf);
}

/* =========================================================================
 * at__ab_u32
 * ========================================================================= */

void test_ab_u32_zero(void)
{
    char buf[4];
    AB_INIT(buf, sizeof(buf));
    AB_U32(0U);
    (void)AB_DONE();
    TEST_ASSERT_EQUAL_STRING("0", buf);
}

void test_ab_u32_one(void)
{
    char buf[4];
    AB_INIT(buf, sizeof(buf));
    AB_U32(1U);
    (void)AB_DONE();
    TEST_ASSERT_EQUAL_STRING("1", buf);
}

void test_ab_u32_max(void)
{
    char buf[16];
    AB_INIT(buf, sizeof(buf));
    AB_U32(4294967295U); /* UINT32_MAX */
    (void)AB_DONE();
    TEST_ASSERT_EQUAL_STRING("4294967295", buf);
}

void test_ab_u32_typical(void)
{
    char buf[8];
    AB_INIT(buf, sizeof(buf));
    AB_U32(30000U);
    (void)AB_DONE();
    TEST_ASSERT_EQUAL_STRING("30000", buf);
}

void test_ab_u8_typical(void)
{
    char buf[4];
    AB_INIT(buf, sizeof(buf));
    AB_U8(7U);
    (void)AB_DONE();
    TEST_ASSERT_EQUAL_STRING("7", buf);
}

/* =========================================================================
 * at__ab_i32
 * ========================================================================= */

void test_ab_i32_positive(void)
{
    char buf[8];
    AB_INIT(buf, sizeof(buf));
    AB_I32(42);
    (void)AB_DONE();
    TEST_ASSERT_EQUAL_STRING("42", buf);
}

void test_ab_i32_negative(void)
{
    char buf[8];
    AB_INIT(buf, sizeof(buf));
    AB_I32(-100);
    (void)AB_DONE();
    TEST_ASSERT_EQUAL_STRING("-100", buf);
}

void test_ab_i32_zero(void)
{
    char buf[4];
    AB_INIT(buf, sizeof(buf));
    AB_I32(0);
    (void)AB_DONE();
    TEST_ASSERT_EQUAL_STRING("0", buf);
}

void test_ab_i32_min(void)
{
    char buf[16];
    AB_INIT(buf, sizeof(buf));
    AB_I32(-2147483648); /* INT32_MIN */
    TEST_ASSERT_TRUE(AB_OK());
    (void)AB_DONE();
    TEST_ASSERT_EQUAL_STRING("-2147483648", buf);
}

/* =========================================================================
 * Chained builder  (integration)
 * ========================================================================= */

void test_builder_chain_at_cfun(void)
{
    char buf[24];
    AB_INIT(buf, sizeof(buf));
    AB_STR("AT+CFUN="); AB_U8(1U); AB_CHAR(','); AB_U8(0U);
    TEST_ASSERT_TRUE(AB_OK());
    (void)AB_DONE();
    TEST_ASSERT_EQUAL_STRING("AT+CFUN=1,0", buf);
}

void test_builder_chain_cmgs(void)
{
    char buf[32];
    AB_INIT(buf, sizeof(buf));
    AB_STR("AT+CMGS="); AB_QSTR("+491711234567");
    TEST_ASSERT_TRUE(AB_OK());
    (void)AB_DONE();
    TEST_ASSERT_EQUAL_STRING("AT+CMGS=\"+491711234567\"", buf);
}

void test_builder_chain_overflow_stops_silently(void)
{
    char buf[8]; /* too small for the full string */
    AB_INIT(buf, sizeof(buf));
    AB_STR("AT+CFUN="); AB_U8(1U);
    /* Overflow on 'AT+CFUN=' (8 chars needs 9 with NUL) */
    TEST_ASSERT_FALSE(AB_OK());
}

/* =========================================================================
 * at__parse_int
 * ========================================================================= */

void test_parse_int_positive(void)
{
    const char *p = "123";
    TEST_ASSERT_EQUAL_INT32(123, at__parse_int(&p));
    TEST_ASSERT_EQUAL_CHAR('\0', *p);
}

void test_parse_int_negative(void)
{
    const char *p = "-55";
    TEST_ASSERT_EQUAL_INT32(-55, at__parse_int(&p));
}

void test_parse_int_zero(void)
{
    const char *p = "0";
    TEST_ASSERT_EQUAL_INT32(0, at__parse_int(&p));
}

void test_parse_int_leading_whitespace(void)
{
    const char *p = "  42";
    TEST_ASSERT_EQUAL_INT32(42, at__parse_int(&p));
}

void test_parse_int_plus_sign(void)
{
    const char *p = "+7";
    TEST_ASSERT_EQUAL_INT32(7, at__parse_int(&p));
}

void test_parse_int_stops_at_comma(void)
{
    const char *p = "5,3";
    TEST_ASSERT_EQUAL_INT32(5, at__parse_int(&p));
    TEST_ASSERT_EQUAL_CHAR(',', *p);
}

void test_parse_int_saturation(void)
{
    const char *p = "99999999999"; /* > INT32_MAX */
    int32_t v = at__parse_int(&p);
    TEST_ASSERT_EQUAL_INT32(0x7FFFFFFF, v);
}

/* =========================================================================
 * at__parse_uint
 * ========================================================================= */

void test_parse_uint_basic(void)
{
    const char *p = "99";
    TEST_ASSERT_EQUAL_UINT32(99U, at__parse_uint(&p));
}

void test_parse_uint_zero(void)
{
    const char *p = "0";
    TEST_ASSERT_EQUAL_UINT32(0U, at__parse_uint(&p));
}

void test_parse_uint_stops_at_non_digit(void)
{
    const char *p = "23abc";
    TEST_ASSERT_EQUAL_UINT32(23U, at__parse_uint(&p));
    TEST_ASSERT_EQUAL_CHAR('a', *p);
}

void test_parse_uint_saturation(void)
{
    const char *p = "99999999999"; /* > UINT32_MAX */
    uint32_t v = at__parse_uint(&p);
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFU, v);
}

/* =========================================================================
 * at__skip_comma
 * ========================================================================= */

void test_skip_comma_basic(void)
{
    const char *p = ",5";
    TEST_ASSERT_TRUE(at__skip_comma(&p));
    TEST_ASSERT_EQUAL_CHAR('5', *p);
}

void test_skip_comma_with_spaces(void)
{
    const char *p = " , 5";
    TEST_ASSERT_TRUE(at__skip_comma(&p));
    TEST_ASSERT_EQUAL_CHAR('5', *p);
}

void test_skip_comma_no_comma(void)
{
    const char *p = "abc";
    TEST_ASSERT_FALSE(at__skip_comma(&p));
}

void test_skip_comma_end_of_string(void)
{
    const char *p = "";
    TEST_ASSERT_FALSE(at__skip_comma(&p));
}

/* =========================================================================
 * Test runner
 * ========================================================================= */

int main(void)
{
    UNITY_BEGIN();

    /* ab_char */
    RUN_TEST(test_ab_char_single);
    RUN_TEST(test_ab_char_fills_exactly);
    RUN_TEST(test_ab_char_overflow);

    /* ab_str */
    RUN_TEST(test_ab_str_basic);
    RUN_TEST(test_ab_str_empty);
    RUN_TEST(test_ab_str_overflow_stops_early);

    /* ab_qstr */
    RUN_TEST(test_ab_qstr_basic);
    RUN_TEST(test_ab_qstr_empty_string);

    /* ab_u32 */
    RUN_TEST(test_ab_u32_zero);
    RUN_TEST(test_ab_u32_one);
    RUN_TEST(test_ab_u32_max);
    RUN_TEST(test_ab_u32_typical);
    RUN_TEST(test_ab_u8_typical);

    /* ab_i32 */
    RUN_TEST(test_ab_i32_positive);
    RUN_TEST(test_ab_i32_negative);
    RUN_TEST(test_ab_i32_zero);
    RUN_TEST(test_ab_i32_min);

    /* builder chain */
    RUN_TEST(test_builder_chain_at_cfun);
    RUN_TEST(test_builder_chain_cmgs);
    RUN_TEST(test_builder_chain_overflow_stops_silently);

    /* parse_int */
    RUN_TEST(test_parse_int_positive);
    RUN_TEST(test_parse_int_negative);
    RUN_TEST(test_parse_int_zero);
    RUN_TEST(test_parse_int_leading_whitespace);
    RUN_TEST(test_parse_int_plus_sign);
    RUN_TEST(test_parse_int_stops_at_comma);
    RUN_TEST(test_parse_int_saturation);

    /* parse_uint */
    RUN_TEST(test_parse_uint_basic);
    RUN_TEST(test_parse_uint_zero);
    RUN_TEST(test_parse_uint_stops_at_non_digit);
    RUN_TEST(test_parse_uint_saturation);

    /* skip_comma */
    RUN_TEST(test_skip_comma_basic);
    RUN_TEST(test_skip_comma_with_spaces);
    RUN_TEST(test_skip_comma_no_comma);
    RUN_TEST(test_skip_comma_end_of_string);

    return UNITY_END();
}
