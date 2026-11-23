#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "config.h"
#include "esp_timer.h"
#include "freertos/task.h"

static QueueHandle_t frames;
static atomic_bool quiet;

static void voice_publish(const uint8_t *frame)
{
    lab_voice_frame_t ev = {
        .len = frame[1], .cmd = frame[2], .count = (uint8_t)(frame[1] - 3),
    };
    size_t n = ev.count > sizeof(ev.params) ? sizeof(ev.params) : ev.count;
    memcpy(ev.params, frame + 3, n);
    if (frames) xQueueSend(frames, &ev, 0);
}

void voice_flush_frames(void)
{
    if (frames) xQueueReset(frames);
}

void voice_set_quiet(bool on)
{
    atomic_store(&quiet, on);
}

bool voice_take_frame(uint8_t cmd, uint8_t len, int timeout_ms, lab_voice_frame_t *out)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        if (!frames || (int32_t)(deadline - now) <= 0) return false;
        lab_voice_frame_t ev;
        if (xQueueReceive(frames, &ev, deadline - now) != pdTRUE) return false;
        if ((!cmd || ev.cmd == cmd) && (!len || ev.len == len)) {
            if (out) *out = ev;
            return true;
        }
    }
}

/* HM01 V1.04: length = length+command+parameters+checksum (3+parameter count).
 * Total frame size is length+2; checksum excludes 7E, checksum itself and EF. */
esp_err_t voice_command(uint8_t cmd, const uint8_t *params, size_t count)
{
    uint8_t frame[LAB_MAX_SEND_BYTES];
    if (count > 252 || count + 5 > sizeof(frame) || (count && !params))
        return ESP_ERR_INVALID_ARG;
    frame[0] = 0x7e;
    frame[1] = (uint8_t)(count + 3);
    frame[2] = cmd;
    uint8_t sum = frame[1] + cmd;
    for (size_t i = 0; i < count; ++i) {
        frame[3 + i] = params[i];
        sum += params[i];
    }
    frame[3 + count] = sum;
    frame[4 + count] = 0xef;
    return lab_send(&lab_voice, frame, count + 5);
}

static void voice_report(const uint8_t *frame)
{
    unsigned cmd = frame[2], count = frame[1] - 3;
    const uint8_t *p = frame + 3;
    printf("[voice] FRAME OK cmd=%02X params=%u\n", cmd, count);
    if (cmd == 0xc2 && count == 1) {
        printf("[voice] 状态=%02X %s\n", p[0], p[0] == 1 ? "播放" :
            p[0] == 2 ? "停止" : p[0] == 3 ? "暂停" : "待确认");
    } else if (cmd == 0xc1 && count == 1) {
        printf("[voice] 音量=%u%s\n", p[0], p[0] > 31 ? "（超出手册范围）" : "");
    } else if ((cmd == 0xc3 || cmd == 0xc9) && count == 2) {
        printf("[voice] %s=%u\n", cmd == 0xc3 ? "Flash音频数" : "当前曲目索引",
            ((unsigned)p[0] << 8) | p[1]);
    } else if (cmd == 0xca && count == 1) {
        printf("[voice] 外设=%02X PC=%u U盘=%u SPI-Flash=%u（其他位含义待确认/HM01）\n",
            p[0], !!(p[0] & 8), !!(p[0] & 4), !!(p[0] & 1));
    } else if ((cmd == 0xa0 || cmd == 0xa6 || cmd == 0xae || cmd == 0xab) && count == 1) {
        const char *result = p[0] == 0 ? "执行成功" : p[0] == 1 ? "命令出错" :
            p[0] == 2 ? "无此文件" : p[0] == 5 ? "设备不在线" : "待确认/HM01";
        printf("[voice] 写命令返回=%02X %s\n", p[0], result);
    } else {
        puts("[voice] 未封装的返回内容或参数长度，含义待确认；见原始HEX");
    }
}

typedef enum { WAIT_HEAD, WAIT_LENGTH, WAIT_BODY } parser_state_t;
typedef struct {
    uint8_t bytes[257];  /* Entire one-byte-length protocol range, including head/tail. */
    size_t used;
    int64_t last_byte_us;
    parser_state_t state;
} voice_parser_t;

static void parser_drop(voice_parser_t *p, size_t count)
{
    memmove(p->bytes, p->bytes + count, p->used - count);
    p->used -= count;
}

/* Scan only from a candidate head. Embedded 7E/EF bytes in payload are legal.
 * On failure slide by one byte, retaining any possible subsequent frame head. */
static void parser_scan(voice_parser_t *p)
{
    for (;;) {
        p->state = WAIT_HEAD;
        while (p->used && p->bytes[0] != 0x7e) parser_drop(p, 1);
        if (!p->used) return;
        p->state = WAIT_LENGTH;
        if (p->used < 2) return;
        unsigned len = p->bytes[1];
        if (len < 3) {
            printf("[voice] 错误帧：非法长度 %u\n", len);
            parser_drop(p, 1);
            continue;
        }
        p->state = WAIT_BODY;
        size_t total = len + 2;
        if (p->used < total) return;
        uint8_t sum = 0;
        for (size_t i = 1; i < total - 2; ++i) sum += p->bytes[i];
        if (p->bytes[total - 1] != 0xef || sum != p->bytes[total - 2]) {
            puts("[voice] 错误帧：帧尾或校验错误，滑动重同步");
            parser_drop(p, 1);
            continue;
        }
        voice_publish(p->bytes);
        if (!atomic_load(&quiet)) voice_report(p->bytes);
        parser_drop(p, total);
    }
}

static void parser_expire(voice_parser_t *p, int64_t now)
{
    if (p->used && now - p->last_byte_us >= LAB_VOICE_STALE_MS * 1000LL) {
        puts("[voice] 残帧字节间超时，重同步（本机超时策略，非厂家时序）");
        /* A corrupted length may have hidden valid complete frames in the suffix. */
        while (p->used) {
            parser_drop(p, 1);
            parser_scan(p);
        }
        p->state = WAIT_HEAD;
    }
}

static void parser_feed(voice_parser_t *p, const uint8_t *bytes, size_t count, int64_t now)
{
    parser_expire(p, now);
    for (size_t i = 0; i < count; ++i) {
        if (p->used == sizeof(p->bytes)) {
            puts("[voice] 解析缓存溢出，重同步");
            parser_drop(p, 1);
        }
        p->bytes[p->used++] = bytes[i];
        p->last_byte_us = now;
        parser_scan(p);
    }
}

static void voice_rx_task(void *arg)
{
    (void)arg;
    uint8_t bytes[LAB_RX_CHUNK_BYTES];
    voice_parser_t parser = {0};
    for (;;) {
        if (lab_rx_fault(&lab_voice)) memset(&parser, 0, sizeof(parser));
        int n = uart_read_bytes(lab_voice.port, bytes, sizeof(bytes), pdMS_TO_TICKS(LAB_RX_POLL_MS));
        if (n < 0) {
            puts("[voice] UART读取失败，重置解析器");
            memset(&parser, 0, sizeof(parser));
            vTaskDelay(1);
            continue;
        }
        int64_t now = esp_timer_get_time();
        /* Parse before slow UART0 printing; timeout is measured at read, not after dump. */
        parser_feed(&parser, bytes, n, now);
        if (!atomic_load(&quiet)) lab_dump("voice", bytes, n);
    }
}

void voice_init(void)
{
    gpio_config_t cfg = { .pin_bit_mask = 1ULL << LAB_VOICE_BUSY, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    frames = xQueueCreate(32, sizeof(lab_voice_frame_t));
    ESP_ERROR_CHECK(frames ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(voice_rx_task, "voice_rx", LAB_RX_STACK_BYTES, NULL,
        LAB_TASK_PRIORITY, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
