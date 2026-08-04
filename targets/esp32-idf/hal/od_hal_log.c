/* od_hal_log for ESP-IDF. See od_hal_log.h for why this exists and what it deliberately is
 * not.
 *
 * Both backends are lifted from compat/, unchanged in behaviour: the UART one from
 * compat/HardwareSerial.h (which existed for exactly one caller, main.cpp's OPENDISPLAY_LOG_UART
 * path, and is deleted with this commit), the stdout one from compat/arduino_compat.h's
 * SerialCompat. Nothing about the bytes on the wire changes -- this is the same IDF calls with
 * the Arduino class removed from between them and od_log.
 */

#include "od_hal_log.h"

#include <limits.h>
#include <stdio.h>

#ifdef OPENDISPLAY_LOG_UART
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"   /* pdMS_TO_TICKS -- driver/uart.h pulls it in, but the flush
                                  * timeout should not depend on that staying true */

/* UART 0, not 1. OPENDISPLAY_LOG_UART_TX/RX are 43/44, which ARE UART0's default pins on the
 * S3, and IDF's own console is UART0 -- opening UART1 and remapping it onto those pads makes
 * the GPIO matrix override UART0's IO_MUX function, so every ESP_LOGx line goes to a UART
 * nothing is connected to. Same reasoning, and the same default, as the define main.cpp used
 * to carry; it moved here with the port it describes. */
#ifndef OPENDISPLAY_LOG_UART_NUM
#define OPENDISPLAY_LOG_UART_NUM 0
#endif
#ifndef OPENDISPLAY_LOG_UART_RX
#define OPENDISPLAY_LOG_UART_RX 44
#endif
#ifndef OPENDISPLAY_LOG_UART_TX
#define OPENDISPLAY_LOG_UART_TX 43
#endif
#ifndef OPENDISPLAY_LOG_UART_BAUD
#define OPENDISPLAY_LOG_UART_BAUD 115200
#endif

/* TX-only ring: this is a log sink, nothing reads from it. The driver rejects an RX buffer of
 * 0, so it gets the minimum it will accept. */
#define OD_LOG_UART_TX_BUF 2048
#define OD_LOG_UART_RX_BUF 256

static bool s_open = false;

void od_hal_log_open(void)
{
    if (s_open) {
        return;
    }

    uart_config_t cfg = {
        .baud_rate  = OPENDISPLAY_LOG_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    if (uart_param_config(OPENDISPLAY_LOG_UART_NUM, &cfg) != ESP_OK) {
        return;
    }
    if (uart_set_pin(OPENDISPLAY_LOG_UART_NUM, OPENDISPLAY_LOG_UART_TX, OPENDISPLAY_LOG_UART_RX,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        return;
    }
    if (uart_driver_install(OPENDISPLAY_LOG_UART_NUM, OD_LOG_UART_RX_BUF, OD_LOG_UART_TX_BUF,
                            0, NULL, 0) != ESP_OK) {
        return;
    }
    s_open = true;
}

bool od_hal_log_is_open(void)
{
    return s_open;
}

int od_hal_log_room(void)
{
    size_t free_bytes = 0;
    if (!s_open || uart_get_tx_buffer_free_size(OPENDISPLAY_LOG_UART_NUM, &free_bytes) != ESP_OK) {
        return 0;
    }
    return (int)free_bytes;
}

size_t od_hal_log_write(const uint8_t *b, size_t n)
{
    if (!s_open || b == NULL || n == 0) {
        return 0;
    }
    int w = uart_write_bytes(OPENDISPLAY_LOG_UART_NUM, (const char *)b, n);
    return (w > 0) ? (size_t)w : 0;
}

void od_hal_log_flush(void)
{
    if (s_open) {
        uart_wait_tx_done(OPENDISPLAY_LOG_UART_NUM, pdMS_TO_TICKS(100));
    }
}

#else  /* stdout backend -- USB-CDC on the S3 boards, via the IDF console driver */

static bool s_open = false;

/* Nothing to open: the console driver is already up by the time application code runs, which
 * is why the Arduino path's Serial.begin(115200) was a no-op too. The flag exists so
 * od_hal_log_is_open() means "logging was initialised" rather than "a port object is
 * non-NULL", which is the distinction od_log.c actually needs. */
void od_hal_log_open(void)
{
    s_open = true;
}

bool od_hal_log_is_open(void)
{
    return s_open;
}

int od_hal_log_room(void)
{
    return s_open ? INT_MAX : 0;
}

size_t od_hal_log_write(const uint8_t *b, size_t n)
{
    if (!s_open || b == NULL || n == 0) {
        return 0;
    }
    return fwrite(b, 1, n, stdout);
}

void od_hal_log_flush(void)
{
    if (s_open) {
        fflush(stdout);
    }
}

#endif /* OPENDISPLAY_LOG_UART */
