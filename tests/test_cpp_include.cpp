/**
 * @file test_cpp_include.cpp
 * @brief C++ inclusion smoke-test for all public libat-c headers.
 *
 * Verifies that every public header can be included from a C++ translation
 * unit without errors.  All headers must be wrapped in extern "C" guards so
 * that symbols link correctly when the library is used from C++ projects.
 *
 * SPDX-License-Identifier: MIT
 */

#include "at_config.h"
#include "at_fmt.h"
#include "at.h"
#include "at_gsm.h"

int main(void)
{
    return 0;
}
