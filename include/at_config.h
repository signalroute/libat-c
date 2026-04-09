/**
 * @file at_config.h
 * @brief Compile-time configuration for the AT modem library.
 *
 * Override any of these by defining them before including this header
 * or by passing -DAT_CFG_xxx=yyy to your compiler.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AT_CONFIG_H
#define AT_CONFIG_H

/* -------------------------------------------------------------------------
 * Ring-buffer sizes (must be powers of two)
 * ------------------------------------------------------------------------- */

/** RX ring buffer capacity in bytes. Must be >= longest URC line. */
#ifndef AT_CFG_RX_BUF_SIZE
#  define AT_CFG_RX_BUF_SIZE  512U
#endif

/** TX scratch buffer for command serialisation. */
#ifndef AT_CFG_TX_BUF_SIZE
#  define AT_CFG_TX_BUF_SIZE  256U
#endif

/* -------------------------------------------------------------------------
 * Response accumulation
 * ------------------------------------------------------------------------- */

/** Maximum single-line length (including \r\n). */
#ifndef AT_CFG_LINE_MAX
#  define AT_CFG_LINE_MAX     128U
#endif

/**
 * Maximum number of response lines accumulated for one command.
 * Each slot is AT_CFG_LINE_MAX bytes — tune carefully on small targets.
 */
#ifndef AT_CFG_RESP_LINES_MAX
#  define AT_CFG_RESP_LINES_MAX 16U
#endif

/* -------------------------------------------------------------------------
 * Command queue
 * ------------------------------------------------------------------------- */

/** Depth of the static command queue. */
#ifndef AT_CFG_CMD_QUEUE_DEPTH
#  define AT_CFG_CMD_QUEUE_DEPTH 8U
#endif

/* -------------------------------------------------------------------------
 * URC dispatch table
 * ------------------------------------------------------------------------- */

/** Maximum number of registered URC handlers. */
#ifndef AT_CFG_URC_TABLE_SIZE
#  define AT_CFG_URC_TABLE_SIZE 16U
#endif

/* -------------------------------------------------------------------------
 * Timing
 * ------------------------------------------------------------------------- */

/** Default command timeout in milliseconds (can be overridden per-command). */
#ifndef AT_CFG_DEFAULT_TIMEOUT_MS
#  define AT_CFG_DEFAULT_TIMEOUT_MS 5000U
#endif

/** Timeout for interactive prompts (e.g. SMS "> " prompt) in ms. */
#ifndef AT_CFG_PROMPT_TIMEOUT_MS
#  define AT_CFG_PROMPT_TIMEOUT_MS 30000U
#endif

/* -------------------------------------------------------------------------
 * Behaviour flags
 * ------------------------------------------------------------------------- */

/**
 * Echo cancellation: if 1 the engine strips echoed command text from the
 * response stream.  Set to 0 if you issue ATE0 at startup and save cycles.
 */
#ifndef AT_CFG_ECHO_CANCEL
#  define AT_CFG_ECHO_CANCEL 1
#endif

/**
 * Enable verbose logging via AT_CFG_LOG().  0 = silent (default for
 * production).  1 = errors only.  2 = full trace.
 */
#ifndef AT_CFG_LOG_LEVEL
#  define AT_CFG_LOG_LEVEL 0
#endif

/* Plug in your platform logger here. */
#if AT_CFG_LOG_LEVEL > 0
#  ifndef AT_CFG_LOG
#    include <stdio.h>
#    define AT_CFG_LOG(lvl, ...) \
         do { if ((lvl) <= AT_CFG_LOG_LEVEL) { printf("[AT] "); printf(__VA_ARGS__); printf("\n"); } } while (0)
#  endif
#else
#  define AT_CFG_LOG(lvl, ...) ((void)0)
#endif

/* -------------------------------------------------------------------------
 * Static assertions — catch bad config at compile time
 * ------------------------------------------------------------------------- */
#include <stdint.h>

#define AT__IS_POW2(n) (((n) != 0U) && (((n) & ((n) - 1U)) == 0U))

_Static_assert(AT__IS_POW2(AT_CFG_RX_BUF_SIZE), "AT_CFG_RX_BUF_SIZE must be a power of 2");
_Static_assert(AT__IS_POW2(AT_CFG_TX_BUF_SIZE), "AT_CFG_TX_BUF_SIZE must be a power of 2");
_Static_assert(AT_CFG_LINE_MAX >= 8U,            "AT_CFG_LINE_MAX too small");
_Static_assert(AT_CFG_CMD_QUEUE_DEPTH >= 1U,     "AT_CFG_CMD_QUEUE_DEPTH must be >= 1");
_Static_assert(AT_CFG_URC_TABLE_SIZE  >= 1U,     "AT_CFG_URC_TABLE_SIZE must be >= 1");

#endif /* AT_CONFIG_H */
