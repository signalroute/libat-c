/**
 * @file example_main.c
 * @brief Complete usage example for the AT modem library.
 *
 * Simulates a host-side platform so the example compiles and runs standalone.
 * On a real MCU replace the platform_write / UART ISR section with your BSP.
 *
 * Build:
 *   gcc -std=c11 -Wall -Wextra -I. \
 *       at.c at_gsm.c example_main.c -o at_example -lpthread
 *
 * SPDX-License-Identifier: MIT
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <pthread.h>

#include "at.h"
#include "at_gsm.h"

/* =========================================================================
 * Platform HAL  — replace with your BSP
 * ========================================================================= */

/*
 * In this simulation the "modem" response injector runs in a thread
 * and calls at_feed() directly, mimicking what a UART ISR would do.
 */

size_t at_platform_write(const uint8_t *data, size_t len)
{
    /* In a real BSP: write to UART TX register / DMA */
    /* Here: echo to stdout for visibility */
    printf("\033[36m[TX→modem] %.*s\033[0m\n", (int)len, (char *)data);
    (void)data; (void)len;
    return len;
}

/* =========================================================================
 * Simulated modem rx_thread
 *
 * A real ISR feeds bytes from hardware into at_feed().
 * We simulate the modem sending responses here.
 * ========================================================================= */

static volatile bool g_running = true;

/* Queue of raw modem responses to inject */
static const char *g_modem_script[] = {
    "\r\nOK\r\n",                            /* ATE0         */
    "\r\nOK\r\n",                            /* AT+CMEE=2    */
    "\r\n+CSQ: 18,0\r\nOK\r\n",             /* AT+CSQ       */
    "\r\n+CREG: 0,1\r\nOK\r\n",             /* AT+CREG?     */
    "\r\n+CPIN: READY\r\nOK\r\n",           /* AT+CPIN?     */
    "\r\n+COPS: 0,0,\"Vodafone DE\",7\r\nOK\r\n", /* AT+COPS? */
    "\r\n+CGPADDR: 1,\"10.0.0.42\"\r\nOK\r\n",    /* AT+CGPADDR */
    "\r\n> \r\n+CMGS: 5\r\nOK\r\n",         /* AT+CMGS send SMS */
    "\r\nOK\r\n",                            /* ATH          */
    NULL,
};
static int g_script_idx = 0;

static void *rx_thread(void *arg)
{
    (void)arg;
    while (g_running) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 80000000L };
        nanosleep(&ts, NULL);   /* 80 ms — simulates modem response latency */
        if (!g_modem_script[g_script_idx]) break;
        const char *resp = g_modem_script[g_script_idx++];
        size_t len = strlen(resp);
        printf("\033[33m[modem→RX] %s\033[0m\n",
               /* strip \r\n for readability */ resp + 2);
        at_feed((const uint8_t *)resp, len);
    }
    return NULL;
}

/* =========================================================================
 * Tick — simulates SysTick.  In production: call from 1 ms ISR.
 * ========================================================================= */

static void tick_1ms(uint32_t ms) { at_tick(ms); }

/* =========================================================================
 * Application callbacks
 * ========================================================================= */

static void on_csq(const at_response_t *resp, void *user)
{
    (void)user;
    at_csq_t csq;
    if (at_parse_csq(resp, &csq))
        printf("[APP] Signal: %d dBm, BER class %u\n", csq.rssi_dbm, csq.ber);
    else
        printf("[APP] CSQ parse failed — status=%s\n", at_result_str(resp->status));
}

static void on_creg(const at_response_t *resp, void *user)
{
    (void)user;
    at_reg_status_t s;
    if (at_parse_creg(resp, &s)) {
        static const char *names[] = {
            "Not registered", "Home", "Searching",
            "Denied", "Unknown", "Roaming",
        };
        printf("[APP] Registration: %s (%u)\n",
               s < 6U ? names[s] : "?", (unsigned)s);
    }
}

static void on_cpin(const at_response_t *resp, void *user)
{
    (void)user;
    at_cpin_t p;
    if (at_parse_cpin(resp, &p))
        printf("[APP] PIN state: %u\n", (unsigned)p);
}

static void on_cops(const at_response_t *resp, void *user)
{
    (void)user;
    at_cops_t cops;
    if (at_parse_cops(resp, &cops))
        printf("[APP] Operator: \"%s\" AcT=%u\n", cops.oper, cops.act);
}

static void on_cgpaddr(const at_response_t *resp, void *user)
{
    (void)user;
    char ip[40];
    if (at_parse_cgpaddr(resp, 1, ip, sizeof(ip)))
        printf("[APP] IP address: %s\n", ip);
}

static void on_sms_sent(const at_response_t *resp, void *user)
{
    (void)user;
    uint8_t mr;
    if (at_parse_cmgs(resp, &mr))
        printf("[APP] SMS sent, message reference: %u\n", mr);
    else
        printf("[APP] SMS send failed: %s\n", at_result_str(resp->status));
}

static void on_generic(const at_response_t *resp, void *user)
{
    const char *tag = (const char *)user;
    printf("[APP] %-12s → %s\n", tag, at_result_str(resp->status));
}

/* =========================================================================
 * URC handlers
 * ========================================================================= */

static void urc_creg(const char *line, void *user)
{
    (void)user;
    printf("[URC] Network registration change: %s\n", line);
}

static void urc_cmti(const char *line, void *user)
{
    (void)user;
    printf("[URC] New SMS indication: %s\n", line);
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(void)
{
    printf("=== AT modem library example ===\n\n");

    /* 1. Initialise engine */
    at_init();

    /* 2. Register URC handlers */
    at_register_urc("+CREG", urc_creg, NULL);
    at_register_urc("+CGREG", urc_creg, NULL);
    at_register_urc("+CMTI", urc_cmti, NULL);

    /* 3. Start simulated modem thread */
    pthread_t tid;
    pthread_create(&tid, NULL, rx_thread, NULL);

    /* 4. Issue startup sequence */
    at_gsm_echo(false,     on_generic, (void*)"ATE0");
    at_gsm_cmee(2,         on_generic, (void*)"CMEE");
    at_gsm_csq(            on_csq,     NULL);
    at_gsm_creg_query(     on_creg,    NULL);
    at_gsm_cpin_query(     on_cpin,    NULL);
    at_gsm_cops_query(     on_cops,    NULL);
    at_gsm_cgpaddr(1,      on_cgpaddr, NULL);

    /* 5. Send an SMS */
    at_gsm_cmgf(1, NULL, NULL);   /* switch to text mode (no CB needed) */
    at_gsm_cmgs("+491234567890", "Hello from embedded!", on_sms_sent, NULL);

    /* 6. Hang up any pending call */
    at_gsm_hangup(on_generic, (void*)"ATH");

    /* 7. Main loop — drive engine + simulate tick */
    uint32_t last_ms = 0;
    for (uint32_t t = 0; t < 5000U; t++) {
        struct timespec ts1ms = { .tv_sec = 0, .tv_nsec = 1000000L };
        nanosleep(&ts1ms, NULL);        /* 1 ms */
        tick_1ms(t - last_ms);
        last_ms = t;
        at_process();

        if (at_queue_depth() == 0 && at_state() == AT_STATE_IDLE && t > 500U)
            break;
    }

    g_running = false;
    pthread_join(tid, NULL);

    printf("\n=== Done ===\n");
    return 0;
}
