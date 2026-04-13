/**
 * @file at_platform_defaults.c
 * @brief Weak default implementations of optional platform hooks.
 *
 * These are compiled into the library as __attribute__((weak)) symbols.
 * Your BSP overrides them by providing a non-weak definition.
 *
 * SPDX-License-Identifier: MIT
 */
#include "at_platform.h"

/* =========================================================================
 * Timing defaults
 * ========================================================================= */

/**
 * Default at_platform_time_ms() — always returns 0.
 * The engine falls back to the at_tick() countdown path when this returns 0.
 * Override with a real monotonic clock in your BSP.
 */
__attribute__((weak))
uint32_t at_platform_time_ms(void)
{
    return 0U;
}

/**
 * Default at_platform_delay_ms() — busy-waits.
 * This works only when at_tick() is called from a SysTick ISR while this
 * function is spinning.  Replace with vTaskDelay / k_sleep on an RTOS.
 */
__attribute__((weak))
void at_platform_delay_ms(uint32_t ms)
{
    /* Spin — callers must ensure at_tick(1) fires from SysTick ISR */
    volatile uint32_t remaining = ms;
    while (remaining > 0U) {
        /* Cannot call at_tick() here (not ISR-safe from task context).
         * The BSP must wire SysTick → at_tick().  This loop just spins
         * until the external tick decrements the engine's internal timer.
         * On an RTOS, replace this entire function. */
        __asm__ volatile ("" ::: "memory"); /* prevent optimisation */
        (void)remaining;
        /* NOTE: without real tick injection this loop is infinite.
         * Always override at_platform_delay_ms() on RTOS targets. */
        break; /* safety: don't deadlock in tests / hosted builds */
    }
}

/* =========================================================================
 * Mutex defaults — no-ops (single-task usage)
 * ========================================================================= */

__attribute__((weak))
void at_platform_mutex_lock(AT_PLATFORM_MUTEX_T mutex)
{
    (void)mutex;
}

__attribute__((weak))
void at_platform_mutex_unlock(AT_PLATFORM_MUTEX_T mutex)
{
    (void)mutex;
}

__attribute__((weak))
AT_PLATFORM_MUTEX_T at_platform_mutex_create(void)
{
    return NULL;
}

__attribute__((weak))
void at_platform_mutex_destroy(AT_PLATFORM_MUTEX_T mutex)
{
    (void)mutex;
}

/* =========================================================================
 * RX notification default — no-op (polling mode)
 * ========================================================================= */

__attribute__((weak))
void at_platform_notify_rx(void)
{
    /* no-op: at_process() polls the ring buffer */
}
