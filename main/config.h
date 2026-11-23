#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
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
#define LAB_VOICE_BAUD         115200  /* 厂家确认实物固件115200；HM01手册默认9600。 */
#define LAB_VOICE_TX           GPIO_NUM_15
#define LAB_VOICE_RX           GPIO_NUM_16
#define LAB_VOICE_BUSY         GPIO_NUM_17
#define LAB_MIN_BAUD           1200
#define LAB_MAX_BAUD           921600
#define LAB_MAX_SEND_BYTES     256
#define LAB_LOCK_TIMEOUT_MS    200
#define LAB_TX_TIMEOUT_MS      3000
#define LAB_VOICE_GAP_MS       250  /* Application policy; Hx V2.03 says 200-300ms. */
#define LAB_VOICE_CMD_NEXT     0xac
#define LAB_VOICE_CMD_PLAY     0xa0
#define LAB_VOICE_CMD_STOP     0xab
#define LAB_VOICE_CMD_LOOP     0xaf
#define LAB_VOICE_LOOP_SINGLE  0x01
#define LAB_VOICE_LOOP_OFF     0x00
/* Temporary for the current module: the alarm voice is Flash index 8.
 * KEY2 plays it manually; the radar alarm loops it in the module itself. */
#define LAB_ALARM_FLASH_INDEX  8
#define LAB_ALARM_ARMED_ON_BOOT true
#define LAB_ALARM_POLL_MS      100
#define LAB_ALARM_STACK_BYTES  3072
#define LAB_ALARM_PRIORITY     4
/* BLE central (NimBLE). The C module never exposes NimBLE types here. */
#define LAB_BLE_CONNECT_TIMEOUT_MS 10000
#define LAB_BLE_DISC_TIMEOUT_MS    20000
#define LAB_BLE_SUB_TIMEOUT_MS     15000
#define LAB_BLE_BACKOFF_MIN_MS     1000
#define LAB_BLE_BACKOFF_MAX_MS     30000
#define LAB_BLE_MAINT_MS           500
#define LAB_BLE_MAX_SUBS           16
#define LAB_BLE_NOTIFY_MAX         64
#define LAB_BLE_EVT_QUEUE_DEPTH    16
#define LAB_BLE_TASK_STACK_BYTES   4096
#define LAB_BLE_TASK_PRIORITY      3
#define LAB_BLE_PEER_SVCS          16
#define LAB_BLE_PEER_CHRS          64
#define LAB_BLE_PEER_DSCS          64
#define LAB_BLE_SEC_FALLBACK_MS    500
#define LAB_BLE_SEC_TIMEOUT_MS     10000
#define LAB_BLE_PAIR_WINDOW_MS     120000
/* Remote-download: 512-byte F0 payload + 7 frame bytes. */
#define LAB_VOICE_XFER_BYTES   512
#define LAB_VOICE_BULK_MAX     (LAB_VOICE_XFER_BYTES + 16)
#define LAB_VOICE_CREATE_TIMEOUT_MS 1000
#define LAB_VOICE_ACK_TIMEOUT_MS    2000
#define LAB_VOICE_FINAL_TIMEOUT_MS  3000
/* Board keys KEY0..KEY3 are on XL9555 P1_7..P1_4, not ESP32 GPIO. KEY2=P1_5.
 * Schematic V1.4: SDA=IO41, SCL=IO42, A0..A2=GND (7-bit address 0x20). */
#define LAB_KEY_I2C_PORT       I2C_NUM_0
#define LAB_KEY_I2C_SDA        GPIO_NUM_41
#define LAB_KEY_I2C_SCL        GPIO_NUM_42
#define LAB_KEY_I2C_HZ         100000
#define LAB_KEY_I2C_TIMEOUT_MS 50
#define LAB_XL9555_ADDR        0x20
#define LAB_XL9555_INPUT1      0x01
#define LAB_XL9555_CONFIG1     0x07
#define LAB_KEY0_PORT1_BIT     7
#define LAB_KEY2_PORT1_BIT     5
#define LAB_KEY3_PORT1_BIT     4
#define LAB_KEY3_LONG_MS       3000
#define LAB_KEY_POLL_MS        10
#define LAB_KEY_DEBOUNCE_MS    30
#define LAB_KEY_STACK_BYTES    3072
#define LAB_KEY_PRIORITY       4
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

typedef struct {
    uint8_t len;        /* Frame length byte, e.g. 8 for the final FD echo. */
    uint8_t cmd;
    uint8_t count;
    uint8_t params[8];
} lab_voice_frame_t;

typedef enum {
    VOICE_FLASH_AUTO,       /* Try external F1, then internal F6. */
    VOICE_FLASH_EXTERNAL,   /* F1 */
    VOICE_FLASH_INTERNAL,   /* F6 */
} voice_flash_t;

typedef enum {
    BLE_DEV_FOB = 0,
    BLE_DEV_SMOKE = 1,
    BLE_DEV_COUNT = 2,
    BLE_DEV_ALL = 0xFF,
} ble_dev_id_t;

extern lab_uart_t lab_radar, lab_voice;
void lab_dump(const char *name, const uint8_t *data, size_t size);
esp_err_t lab_uart_init(lab_uart_t *dev, int baud, uart_word_length_t bits,
                        uart_parity_t parity, uart_stop_bits_t stops);
esp_err_t lab_send(lab_uart_t *dev, const uint8_t *data, size_t size);
esp_err_t lab_send_bulk(lab_uart_t *dev, const uint8_t *data, size_t size);
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
bool radar_person_present(void);
void voice_init(void);
esp_err_t voice_command(uint8_t cmd, const uint8_t *params, size_t count);
/* cmd==0 and len==0 match any frame; used by the file-download flow. */
bool voice_take_frame(uint8_t cmd, uint8_t len, int timeout_ms, lab_voice_frame_t *out);
void voice_flush_frames(void);
void voice_set_quiet(bool on);
bool voice_downloading(void);
esp_err_t voice_download_audio(voice_flash_t flash);
void alarm_init(void);
bool alarm_armed(void);
void alarm_set_armed(bool on);
bool alarm_smoke_active(void);
void alarm_set_smoke_alarm(bool on);
bool alarm_panic_active(void);
void alarm_set_panic(bool on);
bool alarm_is_alarming(void);
bool alarm_condition(void);
esp_err_t key_init(void);
esp_err_t key_read_port1(uint8_t *value);
esp_err_t ble_central_init(void);
bool ble_available(void);
void ble_status(void);
esp_err_t ble_scan_set(bool on);
esp_err_t ble_connect_dev(int dev);
esp_err_t ble_disconnect_dev(int dev);
esp_err_t ble_log_set(int dev, bool on);
esp_err_t ble_central_pairing_toggle(void);
bool ble_central_pairing_active(void);
void shell_init(void);
