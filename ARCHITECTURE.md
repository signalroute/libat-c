# libat-c Architecture

## Overview

libat-c is a zero-malloc, single-static-instance AT command engine for
embedded GSM/LTE modems.  It targets bare-metal MCUs and RTOS-based
platforms (FreeRTOS, Zephyr, ThreadX, …) and provides a clean, callback-
driven API with no dynamic allocation.

---

## Component Map

```
┌─────────────────────────────────────────────────────────┐
│                    Application / GSM layer              │
│          at_gsm_csq()  at_gsm_cmgs()  at_gsm_creg()    │
└──────────────┬───────────────────────────┬─────────────┘
               │  at_send() / at_send_raw() │ at_register_urc()
┌──────────────▼───────────────────────────▼─────────────┐
│                    AT Engine  (at.c)                    │
│  cmd_queue  ·  rx_ringbuf  ·  line_fsm  ·  urc_table   │
└──────────────┬───────────────────────────┬─────────────┘
               │  at_platform_write()       │ at_feed()
┌──────────────▼───────────────────────────▼─────────────┐
│              Platform HAL  (user-provided)              │
│  UART ISR → at_feed()    ·    at_platform_write()       │
└─────────────────────────────────────────────────────────┘
```

| File              | Role                                          |
|-------------------|-----------------------------------------------|
| `include/at.h`    | Public API, types, threading contract         |
| `include/at_gsm.h`| GSM/LTE helper layer (3GPP TS 27.007/27.005)  |
| `include/at_config.h` | Compile-time tunables (buffer sizes, etc.) |
| `include/at_fmt.h`| Lightweight string builder / parser           |
| `src/at.c`        | Engine implementation                         |
| `src/at_gsm.c`    | GSM helper implementations                    |

---

## The ISR / Task Split

### ISR context (producer side)

| Function      | What it does                                              |
|---------------|-----------------------------------------------------------|
| `at_feed()`   | Pushes received bytes into the **lock-free SPSC ring buffer** |
| `at_tick()`   | Decrements the active command timeout counter             |

`at_feed()` and `at_tick()` are the **only** functions safe to call from
an interrupt or DMA callback.  They touch only volatile ring-buffer
indices or a volatile timer counter — no locks, no malloc, no blocking.

### Task context (consumer side)

| Function                        | What it does                              |
|---------------------------------|-------------------------------------------|
| `at_process()`                  | Drains the ring buffer, runs the line FSM, dispatches URCs, completes commands |
| `at_send()` / `at_send_raw()`  | Enqueues a command into the command queue |
| `at_gsm_*()`                    | Higher-level helpers; call `at_send()` internally |
| `at_register_urc()`             | Registers a URC prefix handler            |
| `at_abort()` / `at_reset()`    | Aborts or resets the engine               |

`at_process()` **must** be called from a single task only.  It is not
re-entrant.  All completion callbacks fire inside `at_process()`.

---

## The Ring Buffer

```
 ISR (producer)                       Task (consumer)
 ─────────────                        ───────────────
 at_feed(data, len)                   at_process()
      │                                    │
      ▼                                    ▼
 rxring.buf[head & MASK] = byte   byte = rxring.buf[tail & MASK]
 compiler_barrier()                  compiler_barrier()
 head++                              tail++
```

- **SPSC** (single-producer / single-consumer) — correct by construction.
- `head` is written only by the ISR; `tail` is written only by the task.
- A `compiler_barrier()` (`__asm__ volatile("" ::: "memory")`) prevents
  the compiler from reordering the data write relative to the index update.
- On platforms where 16-bit reads are not atomic, use `AT_CFG_RX_BUF_SIZE`
  values that are powers of two (the default) — wrap-around is handled by
  natural integer overflow, not a comparison.

No hardware memory barrier (`dmb`/`dsb`) is inserted because Cortex-M
guarantees that data writes are observed in program order by the same core.
If you port to a multi-core SoC with a different memory model, add the
appropriate `__DMB()` or equivalent after each index update.

---

## Command Queue

The engine holds a static circular array of `AT_CFG_CMD_QUEUE_DEPTH`
command entries (default: 8).  Each entry is a copy of the command string
and optional body so the caller's buffer may live on the stack.

```
at_send()          → writes into queue[q_head], increments q_count
at_process()       → reads from queue[q_tail] when state == IDLE
engine_finalize()  → fires callback, clears entry, increments q_tail
```

Commands are executed strictly in FIFO order.  The engine is always in
one of these states:

| State           | Meaning                                           |
|-----------------|---------------------------------------------------|
| `AT_STATE_IDLE` | No active command; ready to start next            |
| `AT_STATE_SENDING` | Writing command bytes via `at_platform_write()` |
| `AT_STATE_WAITING` | Waiting for a final result code from the modem |
| `AT_STATE_PROMPT` | Received `"> "`, sending body (SMS PDU path)   |
| `AT_STATE_COMPLETE` | Final result received, callback pending        |

---

## Integrating on an RTOS

### Minimal single-task setup (FreeRTOS example)

```c
/* BSP — called from UART RX ISR or DMA TC ISR */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *h)
{
    at_feed(&rx_byte, 1);
    HAL_UART_Receive_IT(h, &rx_byte, 1); /* re-arm */
}

/* BSP — called from SysTick (1 ms period) */
void SysTick_Handler(void)
{
    at_tick(1);
}

/* AT task — single task drives the engine */
void at_task(void *arg)
{
    at_init();
    at_platform_write_init();  /* your BSP init */

    for (;;) {
        at_process();
        vTaskDelay(pdMS_TO_TICKS(1));  /* or use a semaphore posted by at_feed */
    }
}
```

### Multi-task command issuing

If more than one task needs to enqueue AT commands, protect `at_send()`
with a mutex.  `at_process()` still runs from a single task:

```c
static SemaphoreHandle_t s_at_mutex;

at_result_t my_at_send(const at_cmd_desc_t *desc)
{
    xSemaphoreTake(s_at_mutex, portMAX_DELAY);
    at_result_t r = at_send(desc);
    xSemaphoreGive(s_at_mutex);
    return r;
}
```

### Waking `at_process()` from ISR

To avoid busy-polling, post a binary semaphore from `at_feed()` and pend
on it in the AT task:

```c
/* In at_feed() wrapper (BSP layer) */
void my_uart_rx_isr(uint8_t byte)
{
    at_feed(&byte, 1);
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_rx_sem, &woken);
    portYIELD_FROM_ISR(woken);
}

/* AT task */
void at_task(void *arg)
{
    at_init();
    for (;;) {
        xSemaphoreTake(s_rx_sem, pdMS_TO_TICKS(5)); /* wake on data or 5 ms */
        at_process();
    }
}
```

### `at_platform_write()` implementation

The engine calls `at_platform_write()` from **task context only** — it is
never called from an ISR.  You may use a blocking UART transmit:

```c
size_t at_platform_write(const uint8_t *data, size_t len)
{
    HAL_StatusTypeDef st = HAL_UART_Transmit(&huart2,
                                              (uint8_t *)data, (uint16_t)len,
                                              HAL_MAX_DELAY);
    return (st == HAL_OK) ? len : 0U;
}
```

Return 0 (or any value < `len`) to signal a transport error; the engine
will immediately complete the active command with `AT_ERR_IO` instead of
waiting for a timeout.

---

## Configuration

All compile-time tunables are in `include/at_config.h`:

| Macro                       | Default | Description                         |
|-----------------------------|---------|-------------------------------------|
| `AT_CFG_RX_BUF_SIZE`        | 512     | RX ring buffer size (bytes, power of 2) |
| `AT_CFG_TX_BUF_SIZE`        | 512     | TX command string max length        |
| `AT_CFG_CMD_QUEUE_DEPTH`    | 8       | Maximum queued commands             |
| `AT_CFG_RESP_LINES_MAX`     | 16      | Max response lines per command      |
| `AT_CFG_LINE_MAX`           | 128     | Max characters per response line    |
| `AT_CFG_URC_TABLE_SIZE`     | 16      | Max registered URC handlers         |
| `AT_CFG_DEFAULT_TIMEOUT_MS` | 10 000  | Default command timeout (ms)        |
| `AT_CFG_ECHO_CANCEL`        | 1       | Strip modem echo from RX stream     |

Override any macro by defining it before including `at_config.h`, or by
passing `-DAT_CFG_RX_BUF_SIZE=1024` in your compiler flags.
