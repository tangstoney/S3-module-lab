#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "config.h"
#include "freertos/task.h"

static atomic_bool watch = LAB_RADAR_WATCH;
static bool wake = LAB_RADAR_WAKE_ON_BOOT;
typedef struct {
    int level;
    bool snapshot;
} out_event_t;
/* This is a state monitor: coalesce pending edges to the latest level. */
static QueueHandle_t out_events;

bool radar_watch_enabled(void) { return atomic_load(&watch); }
bool radar_person_present(void)
{
    /* Vendor default positive logic: OUT high = occupied. AT+OUT_MODE can
     * invert it; this project does not change that setting. */
    return gpio_get_level(LAB_RADAR_OUT) == 1;
}
void radar_set_watch(bool on)
{
    atomic_store(&watch, on);
    if (out_events) {
        const out_event_t event = { .level = gpio_get_level(LAB_RADAR_OUT), .snapshot = on };
        xQueueOverwrite(out_events, &event);
    }
}
bool radar_wake_enabled(void) { return wake; }

static void radar_out_isr(void *arg)
{
    (void)arg;
    const out_event_t event = { .level = gpio_get_level(LAB_RADAR_OUT) };
    BaseType_t higher_priority_task_woken = pdFALSE;
    /* Never print from the GPIO interrupt. */
    xQueueOverwriteFromISR(out_events, &event, &higher_priority_task_woken);
    if (higher_priority_task_woken) portYIELD_FROM_ISR();
}

static void radar_out_task(void *arg)
{
    (void)arg;
    int previous = -1;
    out_event_t event;
    for (;;) {
        if (xQueueReceive(out_events, &event, portMAX_DELAY) == pdTRUE) {
            if (radar_watch_enabled() && (event.snapshot || event.level != previous)) {
                printf("\n[radar] OUT=%d %s（%s）\n", event.level,
                    event.level ? "有人" : "无人",
                    event.snapshot ? "当前状态" : event.level ? "上升沿" : "下降沿");
            }
            previous = event.level;
        }
    }
}

esp_err_t radar_set_wake(bool on)
{
    esp_err_t err = gpio_set_level(LAB_RADAR_WAKE, on ? 0 : 1);
    if (err == ESP_OK) wake = on;
    return err;
}

esp_err_t radar_detect(bool on)
{
    const char *cmd = on ? "AT+RADAR_DETECT_ONOFF=1\r\n" : "AT+RADAR_DETECT_ONOFF=0\r\n";
    return lab_send(&lab_radar, (const uint8_t *)cmd, strlen(cmd));
}

esp_err_t radar_query(const char *item)
{
    /* Vendor XLS, AT指令集 rows 23-32. No guessed commands. */
    static const struct { const char *item, *cmd; } queries[] = {
        {"fw", "AT+FW_INFO\r\n"}, {"version", "AT+VER\r\n"},
        {"uuid", "AT+UUID\r\n"}, {"detect", "AT+RADAR_DETECT_ONOFF\r\n"},
        {"range", "AT+MOVE_RANGE\r\n"}, {"out", "AT+OUT_MODE\r\n"},
        {"on_time", "AT+ON_TIME\r\n"}, {"period", "AT+FMCW_PERIOD\r\n"},
    };
    for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); ++i)
        if (!strcmp(item, queries[i].item))
            return lab_send(&lab_radar, (const uint8_t *)queries[i].cmd, strlen(queries[i].cmd));
    return ESP_ERR_INVALID_ARG;
}

static void radar_rx_task(void *arg)
{
    (void)arg;
    uint8_t bytes[LAB_RX_CHUNK_BYTES];
    char line[LAB_RADAR_LINE_BYTES];
    size_t used = 0;
    bool discard = false;
    for (;;) {
        if (lab_rx_fault(&lab_radar)) { used = 0; discard = false; }
        int n = uart_read_bytes(lab_radar.port, bytes, sizeof(bytes), pdMS_TO_TICKS(LAB_RX_POLL_MS));
        if (n < 0) { puts("[radar] UART读取失败"); vTaskDelay(1); continue; }
        lab_dump("radar", bytes, n);
        for (int i = 0; i < n; ++i) {
            unsigned char c = bytes[i];
            if (c == '\n') {
                if (!discard && used) { line[used] = 0; printf("[radar] LINE: %s\n", line); }
                used = 0; discard = false;
            } else if (c != '\r' && !discard) {
                if (used == sizeof(line) - 1) {
                    puts("[radar] 返回行过长，丢弃至下一个LF；原始字节仍输出");
                    used = 0; discard = true;
                } else line[used++] = c >= 0x20 && c <= 0x7e ? c : '.';
            }
        }
    }
}

void radar_init(void)
{
    gpio_config_t input = { .pin_bit_mask = 1ULL << LAB_RADAR_OUT, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE };
    ESP_ERROR_CHECK(gpio_config(&input));
    out_events = xQueueCreate(1, sizeof(out_event_t));
    ESP_ERROR_CHECK(out_events ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(radar_out_task, "out_watch", LAB_WATCH_STACK_BYTES, NULL,
        LAB_TASK_PRIORITY, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(gpio_isr_handler_add(LAB_RADAR_OUT, radar_out_isr, NULL));
    ESP_ERROR_CHECK(gpio_set_intr_type(LAB_RADAR_OUT, GPIO_INTR_ANYEDGE));
    ESP_ERROR_CHECK(gpio_intr_enable(LAB_RADAR_OUT));
    radar_set_watch(LAB_RADAR_WATCH);
    /* Preload released latch before enabling open-drain output. Never drive high. */
    ESP_ERROR_CHECK(gpio_set_level(LAB_RADAR_WAKE, 1));
    gpio_config_t output = { .pin_bit_mask = 1ULL << LAB_RADAR_WAKE, .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE };
    ESP_ERROR_CHECK(gpio_config(&output));
    ESP_ERROR_CHECK(radar_set_wake(LAB_RADAR_WAKE_ON_BOOT));
    ESP_ERROR_CHECK(xTaskCreate(radar_rx_task, "radar_rx", LAB_RX_STACK_BYTES, NULL,
        LAB_TASK_PRIORITY, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
