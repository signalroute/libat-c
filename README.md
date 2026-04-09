# libat-c

**Zero-malloc, ISR-safe AT command engine for embedded C (C11).**

Designed for production firmware on any MCU with a UART — Cortex-M, RISC-V, ESP32, nRF52, PIC32, and others. No dynamic allocation, no RTOS dependency, no external headers.

---

## Features

- **Zero malloc** — entire engine state lives in a single static struct (`g_at`) in BSS
- **ISR-safe** — SPSC ring buffer lets `at_feed()` run in a UART ISR / DMA callback while `at_process()` runs in the main loop; only a volatile head/tail pair is shared
- **3GPP TS 27.007 / 27.005** command helpers — signal quality, registration, SMS, voice, PDP, SIM/PIN, modem control
- **URC dispatch table** — register up to `AT_CFG_URC_TABLE_SIZE` unsolicited result handlers
- **Echo cancellation** — optionally strips modem echo before parsing responses
- **CMake + Makefile** friendly — single library target, no compile-time surprises
- **C11** — uses `_Static_assert`; works on GCC ≥ 4.7, Clang ≥ 3.1, IAR ≥ 8, Keil MDK ≥ 5

---

## Repository layout

```
libat-c/
├── include/
│   ├── at.h          # Engine public API
│   ├── at_config.h   # Compile-time tunables
│   ├── at_fmt.h      # Zero-dependency string builder + parsers
│   └── at_gsm.h      # 3GPP command helpers API
├── src/
│   ├── at.c          # Engine implementation
│   └── at_gsm.c      # 3GPP helpers implementation
├── tests/
│   ├── test_engine.c # Engine core unit tests (36 tests)
│   ├── test_gsm.c    # GSM parsers + builders unit tests (48 tests)
│   └── test_fmt.c    # at_fmt.h unit tests (36 tests)
├── examples/
│   └── host_simulation.c  # POSIX pthread demo (not built for MCU)
├── CMakeLists.txt
└── LICENSE           # MIT
```

---

## Quick start

### 1. Implement the platform write hook

The engine calls this whenever it needs to transmit bytes. Wire it to your UART TX:

```c
/* bsp_uart.c */
#include "at.h"

size_t at_platform_write(const uint8_t *data, size_t len)
{
    HAL_UART_Transmit(&huart1, (uint8_t *)data, (uint16_t)len, HAL_MAX_DELAY);
    return len;
}
```

### 2. Initialise and wire interrupts

```c
#include "at.h"
#include "at_gsm.h"

void app_init(void)
{
    at_init();
    /* Enable UART RX interrupt — ISR calls at_feed() */
}

/* UART receive ISR */
void USART1_IRQHandler(void)
{
    uint8_t byte = USART1->DR;
    at_feed(&byte, 1U);
}

/* SysTick ISR (1 ms) */
void SysTick_Handler(void)
{
    at_tick(1U);
}
```

### 3. Main loop

```c
void app_loop(void)
{
    /* Process any buffered modem responses */
    at_process();
}
```

### 4. Send commands

```c
static void on_csq(const at_response_t *resp, void *user)
{
    at_csq_t q;
    if (at_gsm_parse_csq(resp, &q)) {
        printf("RSSI: %d dBm\n", (int)q.rssi_dbm);
    }
}

at_gsm_csq(on_csq, NULL);   /* AT+CSQ */
at_gsm_creg_set(1, NULL, NULL);  /* AT+CREG=1 (enable URC) */
```

---

## Configuration

Override any of these at compile time with `-DAT_CFG_XXX=value` (CMake: `target_compile_definitions`):

| Macro | Default | Description |
|---|---|---|
| `AT_CFG_RX_BUF_SIZE` | 512 | Ring buffer size (bytes) |
| `AT_CFG_CMD_BUF_SIZE` | 128 | Max command string length |
| `AT_CFG_CMD_QUEUE_DEPTH` | 8 | Max queued commands |
| `AT_CFG_MAX_LINES` | 8 | Max response lines captured |
| `AT_CFG_LINE_BUF_SIZE` | 128 | Max line length |
| `AT_CFG_URC_TABLE_SIZE` | 8 | Max registered URC handlers |
| `AT_CFG_DEFAULT_TIMEOUT_MS` | 3000 | Command timeout (ms) |
| `AT_CFG_ECHO_CANCEL` | 1 | Strip modem echo (0 to disable) |
| `AT_CFG_LOG_LEVEL` | 0 | Verbosity (0=off, 1=errors, 2=debug) |

---

## CMake integration

### As a subdirectory

```cmake
add_subdirectory(libat-c)
target_link_libraries(my_firmware PRIVATE at::atc)
```

### Via FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(
    libat_c
    GIT_REPOSITORY https://github.com/signalroute/libat-c.git
    GIT_TAG        main
)
FetchContent_MakeAvailable(libat_c)
target_link_libraries(my_firmware PRIVATE at::atc)
```

Pass config overrides:
```cmake
target_compile_definitions(my_firmware PRIVATE
    AT_CFG_RX_BUF_SIZE=1024
    AT_CFG_CMD_QUEUE_DEPTH=16
)
```

---

## Makefile integration

```makefile
CFLAGS += -std=c11 -Ilibat-c/include
SRCS   += libat-c/src/at.c libat-c/src/at_gsm.c
```

---

## Running the tests

Requires CMake ≥ 3.16 and a C11-capable compiler. The Unity test framework is fetched automatically.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Expected output:

```
100% tests passed, 0 tests failed out of 3
```

| Suite | Tests | Coverage |
|---|---|---|
| `test_engine` | 36 | Core state machine, queue, URC dispatch, timeout, error codes |
| `test_gsm` | 48 | 3GPP parsers (CSQ, CESQ, CREG, CPIN, CMGS), command builders |
| `test_fmt` | 36 | String builders, integer builders, parsers |
| **Total** | **120** | |

---

## API reference

### Engine (`at.h`)

| Function | Description |
|---|---|
| `at_init()` | Reset engine to idle, flush queues |
| `at_feed(data, len)` | Feed received bytes (ISR-safe) |
| `at_tick(ms)` | Advance timeout timer (call from SysTick) |
| `at_process()` | Drain ring, dispatch lines, start next command |
| `at_send_raw(cmd, timeout, cb, user)` | Enqueue raw AT string |
| `at_abort()` | Cancel active command, fire callback with `AT_ERR_ABORTED` |
| `at_register_urc(prefix, cb, user)` | Register URC handler |
| `at_deregister_urc(prefix)` | Remove URC handler |
| `at_state()` | Current engine state (`AT_STATE_IDLE`, `_SENDING`, `_WAITING`, `_PROMPT`) |
| `at_queue_depth()` | Number of pending commands |

### GSM helpers (`at_gsm.h`)

| Function | AT command | Description |
|---|---|---|
| `at_gsm_at()` | `AT` | Basic check |
| `at_gsm_atz()` | `ATZ` | Factory reset |
| `at_gsm_echo(on)` | `ATE0/1` | Toggle echo |
| `at_gsm_cfun(fun, rst)` | `AT+CFUN` | Set functionality |
| `at_gsm_csq()` | `AT+CSQ` | Signal quality |
| `at_gsm_cesq()` | `AT+CESQ` | Extended signal quality |
| `at_gsm_creg_set(n)` | `AT+CREG=n` | Registration URC mode |
| `at_gsm_creg_query()` | `AT+CREG?` | Query registration |
| `at_gsm_cops_query()` | `AT+COPS?` | Query operator |
| `at_gsm_cmgf(mode)` | `AT+CMGF` | SMS format (text/PDU) |
| `at_gsm_cmgs(number, text)` | `AT+CMGS` | Send SMS |
| `at_gsm_cmgr(index)` | `AT+CMGR` | Read SMS |
| `at_gsm_cmgd(index)` | `AT+CMGD` | Delete SMS |
| `at_gsm_dial(number, voice)` | `ATD` | Dial |
| `at_gsm_answer()` | `ATA` | Answer call |
| `at_gsm_hangup()` | `ATH` | Hang up |
| `at_gsm_cpin_query()` | `AT+CPIN?` | SIM PIN status |
| `at_gsm_cpin_enter(pin)` | `AT+CPIN` | Enter PIN |
| `at_gsm_imei()` | `AT+CGSN` | Get IMEI |
| `at_gsm_imsi()` | `AT+CIMI` | Get IMSI |
| `at_gsm_cgmi()` | `AT+CGMI` | Manufacturer ID |
| `at_gsm_cgdcont(cid, apn)` | `AT+CGDCONT` | PDP context |
| `at_gsm_cgact(cid, act)` | `AT+CGACT` | Activate PDP |

---

## License

MIT — see [LICENSE](LICENSE).
