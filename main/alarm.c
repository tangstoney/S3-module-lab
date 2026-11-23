#include <stdio.h>
#include <stdatomic.h>
#include "config.h"
#include "freertos/task.h"

static atomic_bool armed = LAB_ALARM_ARMED_ON_BOOT;
static atomic_bool smoke_alarm;
static atomic_bool panic_alarm;
static atomic_bool stop_request;

bool alarm_armed(void)
{
    return atomic_load(&armed);
}

bool alarm_smoke_active(void)
{
    return atomic_load(&smoke_alarm);
}

bool alarm_panic_active(void)
{
    return atomic_load(&panic_alarm);
}

bool alarm_condition(void)
{
    return radar_person_present() || alarm_smoke_active() ||
           alarm_panic_active();
}

bool alarm_is_alarming(void)
{
    return alarm_panic_active() || (alarm_armed() && alarm_condition());
}

void alarm_set_smoke_alarm(bool on)
{
    bool was = atomic_exchange(&smoke_alarm, on);
    if (was != on)
        printf("\n[alarm] 烟感%s\n", on ? "报警：报警条件成立" : "解除");
}

void alarm_set_panic(bool on)
{
    bool was = atomic_exchange(&panic_alarm, on);
    if (was != on)
        printf("\n[alarm] 遥控器PANIC%s\n", on ? "触发报警" : "清除");
}

void alarm_set_armed(bool on)
{
    bool was = atomic_exchange(&armed, on);
    if (!on) {
        atomic_store(&panic_alarm, false);
        atomic_store(&stop_request, true);
    }
    if (was != on)
        printf("\n[alarm] %s\n", on ? "布防：有人时循环报警" : "撤防：报警静音");
}

static void alarm_task(void *arg)
{
    (void)arg;
    bool last_person = false;
    bool looping = false;
    for (;;) {
        bool person = radar_person_present();
        bool trigger = alarm_condition();
        bool on = atomic_load(&armed);
        bool effective_armed = on || alarm_panic_active();
        if (person != last_person) {
            printf("\n[alarm] 雷达%s\n", person ? "有人" : "无人");
            last_person = person;
        }
        if (voice_downloading()) {
            /* The download owns the voice port and has already stopped audio. */
            looping = false;
            vTaskDelay(pdMS_TO_TICKS(LAB_ALARM_POLL_MS));
            continue;
        }
        if (atomic_load(&stop_request)) {
            /* Disarm must also stop a manual KEY2 one-shot, not just our loop. */
            esp_err_t err = voice_command(LAB_VOICE_CMD_STOP, NULL, 0);
            if (err == ESP_OK) {
                atomic_store(&stop_request, false);
                looping = false;
                puts("[alarm] 报警停止");
            }
        }
        if (effective_armed && trigger) {
            if (!looping) {
                uint8_t single = LAB_VOICE_LOOP_SINGLE;
                esp_err_t err = voice_command(LAB_VOICE_CMD_LOOP, &single, 1);
                if (err == ESP_OK) {
                    /* Hx V2.03: keep 200-300 ms between commands. */
                    vTaskDelay(pdMS_TO_TICKS(LAB_VOICE_GAP_MS + 50));
                    if ((atomic_load(&armed) || alarm_panic_active()) &&
                        alarm_condition()) {
                        uint8_t params[] = {
                            (uint8_t)(LAB_ALARM_FLASH_INDEX >> 8),
                            (uint8_t)LAB_ALARM_FLASH_INDEX,
                        };
                        err = voice_command(LAB_VOICE_CMD_PLAY, params,
                                            sizeof(params));
                        if (err == ESP_OK) {
                            looping = true;
                            printf("[alarm] 报警条件成立，单曲循环Flash %u\n",
                                   (unsigned)LAB_ALARM_FLASH_INDEX);
                        }
                    }
                }
            }
        } else if (looping) {
            esp_err_t err = voice_command(LAB_VOICE_CMD_STOP, NULL, 0);
            if (err == ESP_OK) {
                looping = false;
                puts("[alarm] 报警停止");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(LAB_ALARM_POLL_MS));
    }
}

void alarm_init(void)
{
    printf("[alarm] 上电%s，报警音频Flash %u（模块单曲循环）\n",
           alarm_armed() ? "布防" : "撤防", (unsigned)LAB_ALARM_FLASH_INDEX);
    ESP_ERROR_CHECK(xTaskCreate(alarm_task, "alarm", LAB_ALARM_STACK_BYTES, NULL,
        LAB_ALARM_PRIORITY, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
