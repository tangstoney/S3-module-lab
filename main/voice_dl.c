#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "config.h"
#include "sdkconfig.h"
#include "esp_timer.h"
#include "freertos/task.h"

static atomic_bool downloading;

bool voice_downloading(void)
{
    return atomic_load(&downloading);
}

#if CONFIG_LAB_VOICE_DOWNLOAD_AUDIO

/* target_add_binary_data(... RENAME_TO "voice_0001_wav") in CMakeLists.txt. */
extern const uint8_t _binary_voice_0001_wav_start[];
extern const uint8_t _binary_voice_0001_wav_end[];

#define AUDIO_DATA _binary_voice_0001_wav_start
#define AUDIO_SIZE ((size_t)(_binary_voice_0001_wav_end - _binary_voice_0001_wav_start))

static const char voice_file_name[] = "0001.wav";
static uint8_t bulk_packet[LAB_VOICE_BULK_MAX];

static size_t build_frame(uint8_t *out, uint8_t cmd, const uint8_t *params, size_t count)
{
    out[0] = 0x7e;
    out[1] = (uint8_t)(count + 3);
    out[2] = cmd;
    uint8_t sum = out[1] + cmd;
    for (size_t i = 0; i < count; ++i) {
        out[3 + i] = params[i];
        sum += params[i];
    }
    out[3 + count] = sum;
    out[4 + count] = 0xef;
    return count + 5;
}

static esp_err_t send_frame(uint8_t cmd, const uint8_t *params, size_t count)
{
    uint8_t frame[32];
    if (count + 5 > sizeof(frame)) return ESP_ERR_INVALID_ARG;
    size_t n = build_frame(frame, cmd, params, count);
    return lab_send_bulk(&lab_voice, frame, n);
}

/* F0: length byte 00, 2-byte payload length, up to 512 bytes (Hx V2.03 5.3.30). */
static esp_err_t send_packet(const uint8_t *data, size_t count)
{
    uint8_t *p = bulk_packet;
    p[0] = 0x7e;
    p[1] = 0x00;
    p[2] = 0xf0;
    p[3] = (uint8_t)(count >> 8);
    p[4] = (uint8_t)count;
    memcpy(p + 5, data, count);
    uint8_t sum = p[2] + p[3] + p[4];
    for (size_t i = 0; i < count; ++i) sum += data[i];
    p[5 + count] = sum;
    p[6 + count] = 0xef;
    return lab_send_bulk(&lab_voice, p, count + 7);
}

/* The module answers each packet with 7E 04 F0 00 F4 EF and, after the last
 * one, echoes the FD frame (length 08) to report the upgrade succeeded. */
static bool wait_packet_ack(bool *complete)
{
    int64_t deadline = esp_timer_get_time() + LAB_VOICE_ACK_TIMEOUT_MS * 1000LL;
    for (;;) {
        int64_t remain = deadline - esp_timer_get_time();
        if (remain <= 0) return false;
        lab_voice_frame_t ev;
        if (!voice_take_frame(0, 0, (int)((remain + 999) / 1000), &ev)) return false;
        if (ev.cmd == 0xf0 && ev.len == 4 && ev.params[0] == 0) return true;
        if (ev.cmd == 0xfd && ev.len == 8 && ev.params[0] == 0) {
            *complete = true;
            return true;
        }
    }
}

static esp_err_t create_file(uint8_t cmd)
{
    esp_err_t err = send_frame(cmd, (const uint8_t *)voice_file_name,
                               sizeof(voice_file_name) - 1);
    if (err != ESP_OK) return err;
    lab_voice_frame_t ev;
    if (!voice_take_frame(cmd, 4, LAB_VOICE_CREATE_TIMEOUT_MS, &ev))
        return ESP_ERR_TIMEOUT;
    return ev.params[0] == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t announce_size(size_t size)
{
    uint8_t params[5] = { 0, (uint8_t)(size >> 24), (uint8_t)(size >> 16),
                          (uint8_t)(size >> 8), (uint8_t)size };
    esp_err_t err = send_frame(0xfd, params, sizeof(params));
    if (err != ESP_OK) return err;
    /* Immediate ACK 7E 04 FD 00 01 EF; the final echo uses length 08. */
    lab_voice_frame_t ev;
    if (voice_take_frame(0xfd, 4, 500, &ev) && ev.params[0] != 0)
        return ESP_FAIL;
    return ESP_OK;
}

esp_err_t voice_download_audio(voice_flash_t flash)
{
    if (atomic_exchange(&downloading, true)) return ESP_ERR_INVALID_STATE;

    const size_t size = AUDIO_SIZE;
    uint8_t create_cmds[2];
    size_t create_count = 0;
    if (flash == VOICE_FLASH_EXTERNAL) create_cmds[create_count++] = 0xf1;
    else if (flash == VOICE_FLASH_INTERNAL) create_cmds[create_count++] = 0xf6;
    else {
        create_cmds[create_count++] = 0xf1;
        create_cmds[create_count++] = 0xf6;
    }

    esp_err_t err = ESP_OK;
    bool complete = false;
    bool created = false;
    size_t sent = 0;
    unsigned packets = 0;
    int64_t start = esp_timer_get_time();

    voice_set_quiet(true);
    voice_flush_frames();
    printf("[voice] 文件下载开始：%s，%u字节\n", voice_file_name, (unsigned)size);

    err = send_frame(0xab, NULL, 0);            /* Stop playback first. */
    if (err != ESP_OK) goto done;
    vTaskDelay(pdMS_TO_TICKS(50));

    for (size_t i = 0; i < create_count; ++i) {
        voice_flush_frames();
        err = create_file(create_cmds[i]);
        if (err == ESP_OK) { created = true; break; }
        printf("[voice] %s创建文件失败：%s\n",
               create_cmds[i] == 0xf1 ? "片外Flash(F1)" : "片内Flash(F6)",
               esp_err_to_name(err));
        (void)send_frame(0xf7, NULL, 0);        /* Leave download state. */
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!created) { err = ESP_FAIL; goto done; }
    vTaskDelay(pdMS_TO_TICKS(50));

    err = announce_size(size);
    if (err != ESP_OK) goto done;

    while (sent < size) {
        size_t chunk = size - sent;
        if (chunk > LAB_VOICE_XFER_BYTES) chunk = LAB_VOICE_XFER_BYTES;
        err = send_packet(AUDIO_DATA + sent, chunk);
        if (err != ESP_OK) break;
        sent += chunk;
        ++packets;
        if (!wait_packet_ack(&complete)) { err = ESP_ERR_TIMEOUT; break; }
        if (complete) break;
        if ((packets % 16) == 0)
            printf("[voice] 已发送 %u/%u 字节（%u%%）\n", (unsigned)sent,
                   (unsigned)size, (unsigned)(sent * 100 / size));
    }

    if (err == ESP_OK && !complete) {
        /* Success echo can arrive just after the last packet ACK. */
        err = voice_take_frame(0xfd, 8, LAB_VOICE_FINAL_TIMEOUT_MS, NULL)
            ? ESP_OK : ESP_ERR_TIMEOUT;
    }

done:
    if (err != ESP_OK) {
        printf("[voice] 下载未完成：%s，发送F7退出下载状态\n", esp_err_to_name(err));
        (void)send_frame(0xf7, NULL, 0);
    } else {
        int64_t ms = (esp_timer_get_time() - start) / 1000;
        printf("[voice] 下载完成：%s，%u字节，用时%lld.%03lld秒\n", voice_file_name,
               (unsigned)size, (long long)(ms / 1000), (long long)(ms % 1000));
    }
    lab_voice.last_tx_us = esp_timer_get_time();
    voice_set_quiet(false);
    atomic_store(&downloading, false);
    return err;
}

#else

esp_err_t voice_download_audio(voice_flash_t flash)
{
    (void)flash;
    puts("[voice] 未编译音频下载：在sdkconfig设置 CONFIG_LAB_VOICE_DOWNLOAD_AUDIO=y 后重新构建");
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
