#include <stdio.h>
#include "config.h"
#include "esp_timer.h"
#include "freertos/task.h"

/* Board keys sit on the XL9555 expander, not on ESP32 pins:
 * KEY0..KEY3 = P1_7..P1_4, switch to GND, low when pressed. */
static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t expander;

esp_err_t key_read_port1(uint8_t *value)
{
    if (!value) return ESP_ERR_INVALID_ARG;
    uint8_t reg = LAB_XL9555_INPUT1;
    return i2c_master_transmit_receive(expander, &reg, 1, value, 1,
                                       LAB_KEY_I2C_TIMEOUT_MS);
}

static esp_err_t expander_write(uint8_t reg, uint8_t value)
{
    uint8_t frame[] = { reg, value };
    return i2c_master_transmit(expander, frame, sizeof(frame), LAB_KEY_I2C_TIMEOUT_MS);
}

typedef struct {
    uint8_t bit;
    bool stable;
    int64_t candidate_since;
    int64_t press_since;
    bool long_fired;
} key_state_t;

static void key_pressed(uint8_t bit)
{
    if (bit == LAB_KEY0_PORT1_BIT) {
        alarm_set_armed(!alarm_armed());
        return;
    }
    if (bit != LAB_KEY2_PORT1_BIT) {
        return;     /* KEY3 is long-press-only; other keys unused. */
    }
    /* Manual alarm is one-shot unless the automatic alarm is already looping. */
    if (!((alarm_armed() || alarm_panic_active()) && alarm_condition())) {
        uint8_t no_loop = LAB_VOICE_LOOP_OFF;
        if (voice_command(LAB_VOICE_CMD_LOOP, &no_loop, 1) == ESP_OK)
            vTaskDelay(pdMS_TO_TICKS(LAB_VOICE_GAP_MS + 50));
    }
    uint8_t params[] = {
        (uint8_t)(LAB_ALARM_FLASH_INDEX >> 8),
        (uint8_t)LAB_ALARM_FLASH_INDEX,
    };
    printf("\n[key] KEY2按下，播放Flash %u报警音频\n",
           (unsigned)LAB_ALARM_FLASH_INDEX);
    esp_err_t send = voice_command(LAB_VOICE_CMD_PLAY, params, sizeof(params));
    if (send != ESP_OK)
        printf("[key] 报警音频发送失败: %s\n", esp_err_to_name(send));
}

static void key_task(void *arg)
{
    (void)arg;
    key_state_t keys[] = {
        { .bit = LAB_KEY0_PORT1_BIT, .stable = true },
        { .bit = LAB_KEY2_PORT1_BIT, .stable = true },
        { .bit = LAB_KEY3_PORT1_BIT, .stable = true },
    };
    const size_t key_count = sizeof(keys) / sizeof(keys[0]);
    int64_t now = esp_timer_get_time();
    for (size_t i = 0; i < key_count; ++i) keys[i].candidate_since = now;
    bool error_shown = false;
    for (;;) {
        uint8_t port1;
        esp_err_t err = key_read_port1(&port1);
        now = esp_timer_get_time();
        if (err != ESP_OK) {
            if (!error_shown)
                printf("\n[key] XL9555读取失败 %s，重试中（KEY0/KEY2/KEY3不可用）\n",
                       esp_err_to_name(err));
            error_shown = true;
            for (size_t i = 0; i < key_count; ++i) keys[i].candidate_since = now;
            vTaskDelay(pdMS_TO_TICKS(LAB_KEY_POLL_MS));
            continue;
        }
        if (error_shown) {
            puts("[key] XL9555读取恢复");
            error_shown = false;
            for (size_t i = 0; i < key_count; ++i) keys[i].candidate_since = now;
        }
        for (size_t i = 0; i < key_count; ++i) {
            key_state_t *key = &keys[i];
            bool pressed = !(port1 & (1u << key->bit));
            if (pressed == key->stable) {
                key->candidate_since = now;
            } else if (now - key->candidate_since >= LAB_KEY_DEBOUNCE_MS * 1000LL) {
                key->stable = pressed;
                key->candidate_since = now;
                if (pressed) {
                    key->press_since = now;
                    key->long_fired = false;
                    key_pressed(key->bit);
                }
            }
            if (key->stable && key->bit == LAB_KEY3_PORT1_BIT &&
                !key->long_fired &&
                now - key->press_since >= LAB_KEY3_LONG_MS * 1000LL) {
                key->long_fired = true;
                printf("\n[key] KEY3长按，切换蓝牙配对模式\n");
                esp_err_t err = ble_central_pairing_toggle();
                if (err != ESP_OK)
                    printf("[key] 配对命令发送失败: %s\n", esp_err_to_name(err));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(LAB_KEY_POLL_MS));
    }
}

esp_err_t key_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = LAB_KEY_I2C_PORT,
        .sda_io_num = LAB_KEY_I2C_SDA,
        .scl_io_num = LAB_KEY_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) return err;
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = LAB_XL9555_ADDR,
        .scl_speed_hz = LAB_KEY_I2C_HZ,
    };
    err = i2c_master_bus_add_device(bus, &dev_cfg, &expander);
    if (err != ESP_OK) return err;
    /* Reset default is all inputs; only touch KEY0/KEY2 so other expander pins
     * keep whatever configuration they had. */
    uint8_t cfg;
    err = i2c_master_transmit_receive(expander,
        (uint8_t[]){ LAB_XL9555_CONFIG1 }, 1, &cfg, 1, LAB_KEY_I2C_TIMEOUT_MS);
    if (err != ESP_OK) return err;
    uint8_t wanted = cfg | (1u << LAB_KEY0_PORT1_BIT) | (1u << LAB_KEY2_PORT1_BIT) |
                     (1u << LAB_KEY3_PORT1_BIT);
    if (wanted != cfg) {
        err = expander_write(LAB_XL9555_CONFIG1, wanted);
        if (err != ESP_OK) return err;
    }
    err = xTaskCreate(key_task, "key", LAB_KEY_STACK_BYTES, NULL,
                      LAB_KEY_PRIORITY, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
    if (err == ESP_OK)
        printf("[key] XL9555 0x%02X P1_7/P1_5/P1_4 输入已就绪：KEY0布防/撤防，KEY2播放报警，KEY3长按配对\n",
               LAB_XL9555_ADDR);
    return err;
}
