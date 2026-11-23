#include <stdio.h>
#include <inttypes.h>
#include "config.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/task.h"

lab_uart_t lab_radar = { .name = "radar", .port = LAB_RADAR_UART,
    .tx = LAB_RADAR_TX, .rx = LAB_RADAR_RX };
lab_uart_t lab_voice = { .name = "voice", .port = LAB_VOICE_UART,
    .tx = LAB_VOICE_TX, .rx = LAB_VOICE_RX };

void lab_dump(const char *name, const uint8_t *data, size_t size)
{
    /* Keep each HEX/TXT pair together, including when both RX tasks run. */
    for (size_t offset = 0; offset < size; offset += LAB_DUMP_CHUNK_BYTES) {
        size_t n = size - offset;
        if (n > LAB_DUMP_CHUNK_BYTES) n = LAB_DUMP_CHUNK_BYTES;
        flockfile(stdout);
        printf("\n[%s] HEX:", name);
        for (size_t i = 0; i < n; ++i) printf(" %02X", data[offset + i]);
        printf("\n[%s] TXT: ", name);
        for (size_t i = 0; i < n; ++i) {
            unsigned char c = data[offset + i];
            putchar(c >= 0x20 && c <= 0x7e ? c : '.');
        }
        putchar('\n');
        funlockfile(stdout);
    }
}

esp_err_t lab_uart_init(lab_uart_t *dev, int baud, uart_word_length_t bits,
                        uart_parity_t parity, uart_stop_bits_t stops)
{
    const uart_config_t cfg = { .baud_rate = baud, .data_bits = bits,
        .parity = parity, .stop_bits = stops, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT };
    dev->tx_lock = xSemaphoreCreateMutex();
    if (!dev->tx_lock) return ESP_ERR_NO_MEM;
    esp_err_t err = uart_param_config(dev->port, &cfg);
    if (err != ESP_OK) return err;
    err = uart_set_pin(dev->port, dev->tx, dev->rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;
    err = uart_driver_install(dev->port, LAB_RX_BUFFER_BYTES, 0,
        LAB_UART_EVENT_COUNT, &dev->events, 0);
    if (err != ESP_OK) return err;
    return ESP_OK;
}

esp_err_t lab_send(lab_uart_t *dev, const uint8_t *data, size_t size)
{
    if (!data || !size || size > LAB_MAX_SEND_BYTES) return ESP_ERR_INVALID_ARG;
    if (dev == &lab_voice && voice_downloading()) {
        puts("[voice] 音频下载进行中，拒绝其他语音发送");
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(dev->tx_lock, pdMS_TO_TICKS(LAB_LOCK_TIMEOUT_MS)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    esp_err_t err = ESP_OK;
    int64_t now = esp_timer_get_time();
    if (dev == &lab_voice && dev->last_tx_us &&
        now - dev->last_tx_us < LAB_VOICE_GAP_MS * 1000LL) {
        printf("[voice] 发码间隔至少 %d ms，请稍后重试\n", LAB_VOICE_GAP_MS);
        err = ESP_ERR_INVALID_STATE;
        goto done;
    }
    /* No TX ring buffer: uart_tx_chars never waits for FIFO space.
     * Bound the whole send, unlike uart_write_bytes which may block forever. */
    int64_t deadline = now + LAB_TX_TIMEOUT_MS * 1000LL;
    size_t sent = 0;
    while (sent < size) {
        if (esp_timer_get_time() >= deadline) { err = ESP_ERR_TIMEOUT; break; }
        int n = uart_tx_chars(dev->port, (const char *)data + sent, size - sent);
        if (n < 0) { err = ESP_FAIL; break; }
        sent += n;
        if (!n) vTaskDelay(1);
    }
    int64_t remaining = deadline - esp_timer_get_time();
    if (err == ESP_OK)
        err = remaining > 0 ? uart_wait_tx_done(dev->port,
            pdMS_TO_TICKS((remaining + 999) / 1000)) : ESP_ERR_TIMEOUT;
    if (dev == &lab_voice && sent) dev->last_tx_us = esp_timer_get_time();
    if (err != ESP_OK)
        printf("[%s] 发送失败 %s（已入FIFO %u/%u 字节，可能存在残帧）\n",
               dev->name, esp_err_to_name(err), (unsigned)sent, (unsigned)size);
done:
    xSemaphoreGive(dev->tx_lock);
    if (err == ESP_OK)
        printf("[%s] 已发送 %u 字节，模块执行结果以返回数据为准\n", dev->name, (unsigned)size);
    return err;
}

esp_err_t lab_send_bulk(lab_uart_t *dev, const uint8_t *data, size_t size)
{
    if (!data || !size || size > LAB_VOICE_BULK_MAX) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(dev->tx_lock, pdMS_TO_TICKS(LAB_LOCK_TIMEOUT_MS)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    esp_err_t err = ESP_OK;
    int64_t deadline = esp_timer_get_time() + LAB_TX_TIMEOUT_MS * 1000LL;
    size_t sent = 0;
    while (sent < size) {
        if (esp_timer_get_time() >= deadline) { err = ESP_ERR_TIMEOUT; break; }
        int n = uart_tx_chars(dev->port, (const char *)data + sent, size - sent);
        if (n < 0) { err = ESP_FAIL; break; }
        sent += n;
        if (!n) vTaskDelay(1);
    }
    int64_t remaining = deadline - esp_timer_get_time();
    if (err == ESP_OK)
        err = remaining > 0 ? uart_wait_tx_done(dev->port,
            pdMS_TO_TICKS((remaining + 999) / 1000)) : ESP_ERR_TIMEOUT;
    if (sent) dev->last_tx_us = esp_timer_get_time();
    xSemaphoreGive(dev->tx_lock);
    if (err != ESP_OK)
        printf("[%s] 批量发送失败 %s（已入FIFO %u/%u 字节）\n",
               dev->name, esp_err_to_name(err), (unsigned)sent, (unsigned)size);
    return err;
}

esp_err_t lab_set_baud(lab_uart_t *dev, int baud)
{
    if (baud < LAB_MIN_BAUD || baud > LAB_MAX_BAUD) return ESP_ERR_INVALID_ARG;
    if (dev == &lab_voice && voice_downloading())
        return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(dev->tx_lock, pdMS_TO_TICKS(LAB_LOCK_TIMEOUT_MS)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    esp_err_t err = uart_wait_tx_done(dev->port, pdMS_TO_TICKS(LAB_LOCK_TIMEOUT_MS));
    if (err == ESP_OK) err = uart_set_baudrate(dev->port, baud);
    xSemaphoreGive(dev->tx_lock);
    if (err == ESP_OK) {
        uint32_t actual = 0;
        err = uart_get_baudrate(dev->port, &actual);
        if (err == ESP_OK)
            printf("[%s] ESP端请求=%d，实际=%" PRIu32 "；模块端未修改\n", dev->name, baud, actual);
    }
    return err;
}

bool lab_rx_fault(lab_uart_t *dev)
{
    bool fault = false;
    uart_event_t event;
    while (xQueueReceive(dev->events, &event, 0) == pdTRUE) {
        if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL ||
            event.type == UART_FRAME_ERR || event.type == UART_PARITY_ERR ||
            event.type == UART_BREAK) {
            printf("[%s] UART接收错误 type=%d，丢弃缓冲并重同步\n", dev->name, event.type);
            fault = true;
        }
    }
    if (fault) {
        /* Called only from the owning RX task. */
        ESP_ERROR_CHECK(uart_flush_input(dev->port));
        xQueueReset(dev->events);
    }
    return fault;
}

void lab_status(void)
{
    uint32_t radar_baud = 0, voice_baud = 0;
    ESP_ERROR_CHECK(uart_get_baudrate(lab_radar.port, &radar_baud));
    ESP_ERROR_CHECK(uart_get_baudrate(lab_voice.port, &voice_baud));
    printf("UART0 CH340 TX=%d RX=%d baud=%d 日志/shell\n",
           LAB_CONSOLE_TX, LAB_CONSOLE_RX, LAB_CONSOLE_BAUD);
    printf("radar UART%d TX=%d RX=%d baud=%" PRIu32 " bits=%d parity=%d stop=%d\n",
        lab_radar.port, lab_radar.tx, lab_radar.rx, radar_baud,
        LAB_RADAR_DATA_BITS + 5, LAB_RADAR_PARITY, LAB_RADAR_STOP_BITS);
    printf("radar OUT GPIO%d=%d watch=%s; PB4 GPIO%d=%s\n",
        LAB_RADAR_OUT, gpio_get_level(LAB_RADAR_OUT), radar_watch_enabled() ? "on" : "off",
        LAB_RADAR_WAKE, radar_wake_enabled() ? "低电平/唤醒" : "高阻/释放");
    printf("voice UART%d TX=%d RX=%d baud=%" PRIu32 " 8N1 无流控\n",
        lab_voice.port, lab_voice.tx, lab_voice.rx, voice_baud);
    printf("voice BUSY GPIO%d=%d（高=正在播放）\n", LAB_VOICE_BUSY, gpio_get_level(LAB_VOICE_BUSY));
    printf("alarm %s；PANIC=%s；雷达有人=%d；KEY0=布防/撤防，KEY2=播放Flash %u\n",
           alarm_armed() ? "布防" : "撤防",
           alarm_panic_active() ? "是" : "否", radar_person_present(),
           (unsigned)LAB_ALARM_FLASH_INDEX);
}

void app_main(void)
{
    printf("\ns3_module_lab / %s\n", esp_get_idf_version());
    ESP_ERROR_CHECK(lab_uart_init(&lab_radar, LAB_RADAR_BAUD,
        LAB_RADAR_DATA_BITS, LAB_RADAR_PARITY, LAB_RADAR_STOP_BITS));
    ESP_ERROR_CHECK(lab_uart_init(&lab_voice, LAB_VOICE_BAUD,
        UART_DATA_8_BITS, UART_PARITY_DISABLE, UART_STOP_BITS_1));
    radar_init();
    voice_init();
    alarm_init();
    ESP_ERROR_CHECK(key_init());
    lab_status();
    puts("GPIO与摄像头/RGB屏共用，请断开这些外设；PB4完整唤醒时序待确认。");
    puts("语音115200（厂家确认实物固件）");
    esp_err_t ble_err = ble_central_init();
    if (ble_err != ESP_OK)
        printf("[ble] 初始化失败 %s；雷达/语音/按键继续运行\n",
               esp_err_to_name(ble_err));
    shell_init();
}
