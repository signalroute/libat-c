/**
 * @file stm32_freertos.c
 * @brief Example: STM32 + FreeRTOS + DMA UART integration with libat-c.
 *
 * This file is NOT compiled as part of the library — it is an annotated
 * reference implementation that you can copy into your STM32 BSP.
 *
 * Hardware assumptions
 * ====================
 *   • STM32F4 / STM32L4 (HAL-based, adapts to any STM32 with minimal change)
 *   • USART2 @ 115200 8N1, DMA RX circular buffer, DMA TX
 *   • FreeRTOS v10+ (CMSIS-OS v2 wrappers)
 *   • Modem reset pin on PA4 (active-low)
 *
 * Integration steps
 * =================
 *   1.  Copy this file into your project's Src/ directory.
 *   2.  Add libat-c/src/at.c, at_gsm.c, at_platform_defaults.c to your build.
 *   3.  Add libat-c/include/ to your include paths.
 *   4.  Adjust the #defines below for your hardware.
 *   5.  Call at_task_create() once from main() after HAL_Init() + RTOS init.
 *
 * SPDX-License-Identifier: MIT
 */

/* ── Adjust these for your target ── */
#define AT_UART            huart2
#define AT_DMA_RX_BUF_SZ  256U
#define AT_TASK_STACK_SZ   512U   /* words */
#define AT_TASK_PRIORITY   3U

/* ─────────────────────────────────────────────────────────────────────────── */

/*
 * NOTE: All STM32 HAL and FreeRTOS headers are referenced here for
 * documentation purposes.  In a real project these would be real includes.
 *
 * #include "stm32f4xx_hal.h"
 * #include "FreeRTOS.h"
 * #include "task.h"
 * #include "semphr.h"
 * #include "at.h"
 * #include "at_gsm.h"
 * #include "at_platform.h"
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* =========================================================================
 * DMA ring-buffer tracking
 * ========================================================================= */

static uint8_t  s_dma_rx_buf[AT_DMA_RX_BUF_SZ];
static uint16_t s_dma_prev_head;    /* DMA NDTR shadow */

/* =========================================================================
 * Platform hook: at_platform_write()  (REQUIRED)
 * ========================================================================= */

/**
 * Transmit bytes over USART2 using DMA (non-blocking with completion poll).
 * Replace HAL_UART_Transmit_DMA with HAL_UART_Transmit for blocking mode.
 */
size_t at_platform_write(const uint8_t *data, size_t len)
{
    /*
     * In a real implementation:
     *   HAL_StatusTypeDef s = HAL_UART_Transmit(&AT_UART,
     *                             (uint8_t *)data, (uint16_t)len, 100);
     *   return (s == HAL_OK) ? len : 0;
     *
     * For DMA TX, use a semaphore to signal TX-complete in
     * HAL_UART_TxCpltCallback and wait here.
     */
    (void)data; (void)len;
    return len; /* stub — replace with real UART write */
}

/* =========================================================================
 * Platform hook: at_platform_time_ms()  (OPTIONAL)
 * ========================================================================= */

uint32_t at_platform_time_ms(void)
{
    /*
     * FreeRTOS tick → milliseconds:
     *   return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
     *
     * Or use HAL_GetTick() (SysTick-based, 1 ms resolution on STM32):
     *   return HAL_GetTick();
     */
    return 0; /* stub */
}

/* =========================================================================
 * Platform hook: at_platform_delay_ms()  (OPTIONAL)
 * ========================================================================= */

void at_platform_delay_ms(uint32_t ms)
{
    /*
     * FreeRTOS:  vTaskDelay(pdMS_TO_TICKS(ms));
     * HAL:       HAL_Delay(ms);  // only safe from non-ISR context
     */
    (void)ms; /* stub */
}

/* =========================================================================
 * Platform hook: RX notification  (OPTIONAL but recommended)
 * ========================================================================= */

static void *s_rx_sem; /* SemaphoreHandle_t in a real project */

void at_platform_notify_rx(void)
{
    /*
     * Called from the UART IDLE or DMA Half/Full-Transfer ISR.
     * Wakes the AT task so at_process() runs immediately.
     *
     * BaseType_t woken = pdFALSE;
     * xSemaphoreGiveFromISR((SemaphoreHandle_t)s_rx_sem, &woken);
     * portYIELD_FROM_ISR(woken);
     */
    (void)s_rx_sem;
}

/* =========================================================================
 * UART ISR / DMA callback — feeds received bytes into the AT engine
 * ========================================================================= */

/**
 * Call this from:
 *   HAL_UARTEx_RxEventCallback()  — for UART + DMA with IDLE detection
 *   HAL_UART_RxHalfCpltCallback() + HAL_UART_RxCpltCallback() — for circular DMA
 */
void bsp_at_uart_rx_event(uint16_t dma_ndtr)
{
    /*
     * Calculate how many new bytes arrived since last callback.
     * DMA NDTR counts DOWN from AT_DMA_RX_BUF_SZ to 0 in circular mode.
     *
     * new_head = AT_DMA_RX_BUF_SZ - dma_ndtr   (absolute write position)
     * bytes_available = (new_head - prev_head + BUF_SZ) % BUF_SZ
     */
    uint16_t new_head = (uint16_t)(AT_DMA_RX_BUF_SZ - dma_ndtr);
    uint16_t avail;

    if (new_head >= s_dma_prev_head) {
        avail = (uint16_t)(new_head - s_dma_prev_head);
        /* Feed contiguous region */
        at_feed(s_dma_rx_buf + s_dma_prev_head, avail);
    } else {
        /* Wrap-around: two regions */
        avail = (uint16_t)(AT_DMA_RX_BUF_SZ - s_dma_prev_head);
        at_feed(s_dma_rx_buf + s_dma_prev_head, avail);
        at_feed(s_dma_rx_buf, new_head);
    }
    s_dma_prev_head = new_head;

    /* Wake the AT task */
    at_platform_notify_rx();
}

/* =========================================================================
 * SysTick hook — feeds at_tick()
 * ========================================================================= */

/**
 * Add this call to your SysTick_Handler() or HAL_SYSTICK_Callback().
 * at_tick(1) advances the engine's 1 ms countdown timer.
 */
void bsp_at_systick_hook(void)
{
    at_tick(1U);
}

/* =========================================================================
 * AT task — runs at_process() in a loop
 * ========================================================================= */

static void at_task(void *arg)
{
    (void)arg;

    /* Power-on modem stabilisation */
    at_platform_delay_ms(500U);

    /* Initialise engine */
    at_init();

    /* Optionally register a trace hook for UART sniffer during development */
#if AT_CFG_TRACE_ENABLED
    at_set_trace_hook(my_trace_cb, NULL);
#endif

    /* Send ATZ to reset modem to factory defaults */
    at_send_raw("ATZ",  500U, NULL, NULL);

    /* Enable verbose error codes */
    at_send_raw("AT+CMEE=2", 2000U, NULL, NULL);

    /* Main loop */
    for (;;) {
        at_process();

        /*
         * Wait for RX notification semaphore (up to 10 ms).
         * This eliminates busy-polling while still being responsive.
         *
         * xSemaphoreTake((SemaphoreHandle_t)s_rx_sem, pdMS_TO_TICKS(10));
         *
         * Without the semaphore, a simple delay works:
         *   vTaskDelay(pdMS_TO_TICKS(1));
         */
    }
}

/**
 * Call once from main() after FreeRTOS + HAL initialisation.
 */
void at_task_create(void)
{
    /*
     * Create the RX semaphore.
     * s_rx_sem = xSemaphoreCreateBinary();
     *
     * Create the AT task.
     * xTaskCreate(at_task, "at", AT_TASK_STACK_SZ, NULL, AT_TASK_PRIORITY, NULL);
     *
     * Start DMA circular RX on AT_UART.
     * HAL_UARTEx_ReceiveToIdle_DMA(&AT_UART, s_dma_rx_buf, AT_DMA_RX_BUF_SZ);
     */
    (void)at_task; /* stub — remove when integrating */
}

/* =========================================================================
 * Example: ICCID-based SMS routing  (issue #244)
 * ========================================================================= */

/**
 * Application callback — called when a +CMT URC arrives (new SMS).
 *
 * Register with:
 *   at_register_urc("+CMT", on_sms_received, NULL);
 */
static void on_sms_received(const char *line, void *user)
{
    (void)user;

    /*
     * Line format (text mode, AT+CMGF=1):
     *   +CMT: "+4915199999999",,"24/01/15,10:30:00+04"
     *   <next process call delivers the body>
     *
     * For ICCID-based routing, query the active SIM's ICCID once on startup
     * via AT+CCID or AT+ICCID (modem-specific), then route here:
     */
    const char *iccid = "89490200001234567890"; /* retrieved at startup */

    /* Route based on ICCID prefix (MCC+MNC = first 6-7 digits of ICCID) */
    if (strncmp(iccid, "894902", 6) == 0) {
        /* Vodafone DE — route to primary handler */
    } else if (strncmp(iccid, "894301", 6) == 0) {
        /* Telekom DE — route to secondary handler */
    }

    (void)line;
}

/* Silence unused-function warning in hosted builds */
static void _suppress_unused(void) __attribute__((used));
static void _suppress_unused(void) { (void)on_sms_received; }
