/**
 * @file at_platform.h
 * @brief Optional platform abstraction layer for the AT engine.
 *
 * This header documents the full set of platform hooks that can be
 * provided by the BSP (Board Support Package) to give the AT engine
 * portable timing, delay, and RTOS synchronisation primitives.
 *
 * All hooks are **optional** — the engine has safe defaults:
 *
 *  Hook                    Default behaviour when not implemented
 *  ──────────────────────  ─────────────────────────────────────────────────
 *  at_platform_write()     **REQUIRED** — must always be implemented.
 *  at_platform_time_ms()   Returns 0 (engine relies on at_tick() instead).
 *  at_platform_delay_ms()  Busy-loops on at_tick(); not recommended on RTOS.
 *  at_platform_mutex_*()   Omit if at_send() is called from one task only.
 *
 * To provide a hook, define it in your BSP source file.  The engine
 * declares each optional function as `weak` on GCC/Clang targets so that
 * the linker prefers your implementation when present.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AT_PLATFORM_H
#define AT_PLATFORM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Required — must be implemented in every BSP
 * ========================================================================= */

/**
 * @brief Write bytes to the modem UART/SPI/USB-CDC.
 *
 * May be blocking or DMA-backed.  Called from task context only (never ISR).
 *
 * @param data  Bytes to transmit.
 * @param len   Number of bytes.
 * @return      Number of bytes actually written (must equal len on success).
 */
extern size_t at_platform_write(const uint8_t *data, size_t len);

/* =========================================================================
 * Optional — timing
 * ========================================================================= */

/**
 * @brief Return a monotonic millisecond timestamp.
 *
 * Used by the engine for deadline-based timeout tracking when available.
 * If not implemented (or returns 0 constantly), the engine falls back to
 * the at_tick() countdown mechanism.
 *
 * Example implementations:
 *   - Bare-metal:  return SysTick_ms_counter;
 *   - FreeRTOS:    return (uint32_t)xTaskGetTickCount();
 *   - Zephyr:      return (uint32_t)(k_uptime_get() & 0xFFFFFFFFu);
 *
 * @return Monotonic milliseconds.  Wraps at UINT32_MAX (every ~49 days).
 */
uint32_t at_platform_time_ms(void);

/**
 * @brief Block the current task for at least @p ms milliseconds.
 *
 * The AT engine does NOT call this internally.  It is provided for
 * application code that needs to insert delays between AT commands (e.g.
 * a 100 ms power-on stabilisation wait before sending ATZ).
 *
 * On bare-metal the default implementation spins on at_tick(), which
 * requires at_tick() to be called from a SysTick ISR while this function
 * is running.  On an RTOS, provide a proper task-sleep implementation:
 *
 *   FreeRTOS:  vTaskDelay(pdMS_TO_TICKS(ms));
 *   Zephyr:    k_sleep(K_MSEC(ms));
 *   CMSIS-OS:  osDelay(ms);
 *
 * @param ms  Minimum delay in milliseconds.
 */
void at_platform_delay_ms(uint32_t ms);

/* =========================================================================
 * Optional — RTOS mutex (for multi-task at_send() sharing)
 * ========================================================================= */

/**
 * @brief Opaque mutex handle — set to whatever type your RTOS uses.
 *
 * Override by defining AT_PLATFORM_MUTEX_T before including this header:
 *
 *   #define AT_PLATFORM_MUTEX_T  SemaphoreHandle_t   // FreeRTOS
 *   #define AT_PLATFORM_MUTEX_T  struct k_mutex       // Zephyr
 */
#ifndef AT_PLATFORM_MUTEX_T
#define AT_PLATFORM_MUTEX_T  void *
#endif

/**
 * @brief Acquire the AT engine's command-queue mutex (blocking).
 *
 * Called by at_send() / at_send_raw() before modifying the command queue.
 * Only needed when multiple RTOS tasks share the AT engine.
 *
 * If you only call at_send*() from one task, leave this unimplemented.
 *
 * @param mutex  Mutex handle returned by at_platform_mutex_create().
 */
void at_platform_mutex_lock(AT_PLATFORM_MUTEX_T mutex);

/**
 * @brief Release the AT engine's command-queue mutex.
 *
 * @param mutex  Mutex handle.
 */
void at_platform_mutex_unlock(AT_PLATFORM_MUTEX_T mutex);

/**
 * @brief Create and return a new mutex.
 *
 * Called once during at_init() when AT_CFG_USE_MUTEX is defined.
 * Return NULL to disable mutex support at runtime.
 *
 * @return  Mutex handle, or NULL on failure / unsupported.
 */
AT_PLATFORM_MUTEX_T at_platform_mutex_create(void);

/**
 * @brief Destroy a mutex created by at_platform_mutex_create().
 *
 * Called during at_deinit().
 *
 * @param mutex  Mutex handle.
 */
void at_platform_mutex_destroy(AT_PLATFORM_MUTEX_T mutex);

/* =========================================================================
 * Optional — async notification (ISR → task wakeup)
 * ========================================================================= */

/**
 * @brief Signal that new RX data is available in the ring buffer.
 *
 * at_feed() calls this (if implemented) after pushing bytes into the ring
 * buffer from an ISR.  Use it to unblock a task waiting on a semaphore so
 * that at_process() runs promptly instead of on a fixed poll interval.
 *
 * Example (FreeRTOS):
 *   static SemaphoreHandle_t s_rx_sem;
 *   void at_platform_notify_rx(void) {
 *       BaseType_t woken = pdFALSE;
 *       xSemaphoreGiveFromISR(s_rx_sem, &woken);
 *       portYIELD_FROM_ISR(woken);
 *   }
 *
 * Leave unimplemented to use a polling loop in at_process().
 */
void at_platform_notify_rx(void);

#ifdef __cplusplus
}
#endif

#endif /* AT_PLATFORM_H */
