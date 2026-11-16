#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

/* All board parameters and application limits live here. TX/RX are ESP-side. */
#define LAB_CONSOLE_UART       UART_NUM_0
#define LAB_CONSOLE_BAUD       115200
#define LAB_CONSOLE_TX         GPIO_NUM_43
#define LAB_CONSOLE_RX         GPIO_NUM_44
#define LAB_RADAR_UART         UART_NUM_1
#define LAB_RADAR_BAUD         115200
#define LAB_RADAR_TX           GPIO_NUM_4
#define LAB_RADAR_RX           GPIO_NUM_5
#define LAB_RADAR_OUT          GPIO_NUM_6
#define LAB_RADAR_WAKE         GPIO_NUM_7
/* 8N1 is the requested starting configuration, not specified by radar docs. */
#define LAB_RADAR_DATA_BITS    UART_DATA_8_BITS
#define LAB_RADAR_PARITY       UART_PARITY_DISABLE
#define LAB_RADAR_STOP_BITS    UART_STOP_BITS_1
#define LAB_RADAR_WATCH        true
#define LAB_RADAR_WAKE_ON_BOOT false
#define LAB_VOICE_UART         UART_NUM_2
#define LAB_VOICE_BAUD         9600
#define LAB_VOICE_TX           GPIO_NUM_15
#define LAB_VOICE_RX           GPIO_NUM_16
#define LAB_VOICE_BUSY         GPIO_NUM_17
#define LAB_MIN_BAUD           1200
#define LAB_MAX_BAUD           921600
#define LAB_MAX_SEND_BYTES     256
#define LAB_LOCK_TIMEOUT_MS    200
#define LAB_TX_TIMEOUT_MS      3000
#define LAB_VOICE_GAP_MS       250  /* Application policy; Hx V2.03 says 200-300ms. */
#define LAB_VOICE_STALE_MS     500  /* Host inter-byte timeout, not a vendor timing. */
#define LAB_RX_BUFFER_BYTES    4096
#define LAB_RX_CHUNK_BYTES     128
#define LAB_DUMP_CHUNK_BYTES   32
#define LAB_RX_POLL_MS         50
#define LAB_UART_EVENT_COUNT   16
#define LAB_RADAR_LINE_BYTES   256
#define LAB_RX_STACK_BYTES     4096
#define LAB_WATCH_STACK_BYTES  3072
#define LAB_SHELL_STACK_BYTES  6144
#define LAB_TASK_PRIORITY      5
#define LAB_SHELL_PRIORITY     3
#define LAB_SHELL_LINE_BYTES   (LAB_MAX_SEND_BYTES * 3 + 128)
#define LAB_SHELL_MAX_ARGS     (LAB_MAX_SEND_BYTES + 8)

typedef struct {
    const char *name;
    uart_port_t port;
    gpio_num_t tx, rx;
    SemaphoreHandle_t tx_lock;
    QueueHandle_t events;
    int64_t last_tx_us;
} lab_uart_t;

extern lab_uart_t lab_radar, lab_voice;
void lab_dump(const char *name, const uint8_t *data, size_t size);
esp_err_t lab_uart_init(lab_uart_t *dev, int baud, uart_word_length_t bits,
                        uart_parity_t parity, uart_stop_bits_t stops);
esp_err_t lab_send(lab_uart_t *dev, const uint8_t *data, size_t size);
esp_err_t lab_set_baud(lab_uart_t *dev, int baud);
bool lab_rx_fault(lab_uart_t *dev);
void lab_status(void);
void radar_init(void);
void radar_set_watch(bool on);
bool radar_watch_enabled(void);
esp_err_t radar_set_wake(bool on);
bool radar_wake_enabled(void);
esp_err_t radar_detect(bool on);
esp_err_t radar_query(const char *item);
void voice_init(void);
esp_err_t voice_command(uint8_t cmd, const uint8_t *params, size_t count);
void shell_init(void);
