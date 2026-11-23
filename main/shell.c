#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "config.h"
#include "esp_console.h"

#if !CONFIG_ESP_CONSOLE_UART_DEFAULT
#error "Select the default UART console (UART0 GPIO43/44) for CH340"
#endif
_Static_assert(CONFIG_ESP_CONSOLE_UART_NUM == LAB_CONSOLE_UART, "Console UART/config.h mismatch");
_Static_assert(CONFIG_ESP_CONSOLE_UART_BAUDRATE == LAB_CONSOLE_BAUD, "Console baud/config.h mismatch");

static const char radar_usage[] =
    "radar watch on|off | wake on|off | detect on|off | query fw|version|uuid|detect|range|out|on_time|period";
static const char uart_usage[] =
    "uart baud radar|voice <1200..921600>\n"
    "uart text radar|voice \"文本\" [--eol none|cr|lf|crlf]\n"
    "uart hex radar|voice <两位HEX字节...>";
static const char voice_usage[] =
    "voice status | next | volume [0..31] | play flash|usb <0..65535索引> | stop | query count|track|devices\n"
    "voice loop on|off  (on=单曲循环, off=单曲不循环)\n"
    "voice download [f1|f6]  (f1=片外Flash, f6=片内Flash, 默认自动尝试)";
static const char ble_usage[] =
    "ble status | scan on|off | pair\n"
    "ble connect fob|smoke|all | disconnect fob|smoke|all\n"
    "ble log fob|smoke|all on|off";

static int usage(const char *text) { printf("用法: %s\n", text); return 1; }
static int result(esp_err_t err)
{
    if (err != ESP_OK) printf("错误: %s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

/* Decimal only: reject signs, trailing garbage, overflow, empty strings. */
static bool number(const char *s, unsigned long min, unsigned long max, unsigned long *value)
{
    if (!*s) return false;
    for (const char *p = s; *p; ++p) if (*p < '0' || *p > '9') return false;
    errno = 0;
    char *end;
    unsigned long n = strtoul(s, &end, 10);
    if (errno || *end || n < min || n > max) return false;
    *value = n;
    return true;
}

static bool on_off(const char *s, bool *on)
{
    if (!strcmp(s, "on")) { *on = true; return true; }
    if (!strcmp(s, "off")) { *on = false; return true; }
    return false;
}

static int status_cmd(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) return usage("status");
    lab_status();
    return 0;
}

static int radar_cmd(int argc, char **argv)
{
    if (argc != 3) return usage(radar_usage);
    if (!strcmp(argv[1], "query")) {
        esp_err_t err = radar_query(argv[2]);
        return err == ESP_ERR_INVALID_ARG ? usage(radar_usage) : result(err);
    }
    bool on;
    if (!on_off(argv[2], &on)) return usage(radar_usage);
    if (!strcmp(argv[1], "watch")) {
        radar_set_watch(on);
        printf("radar watch %s\n", on ? "on" : "off");
        return 0;
    }
    if (!strcmp(argv[1], "wake")) {
        esp_err_t err = radar_set_wake(on);
        if (err == ESP_OK) printf("PB4=%s；完整唤醒时序待确认\n", on ? "低电平" : "高阻");
        return result(err);
    }
    if (!strcmp(argv[1], "detect")) return result(radar_detect(on));
    return usage(radar_usage);
}

static int nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int uart_cmd(int argc, char **argv)
{
    if (argc < 4) return usage(uart_usage);
    lab_uart_t *dev = !strcmp(argv[2], "radar") ? &lab_radar :
                     !strcmp(argv[2], "voice") ? &lab_voice : NULL;
    if (!dev) return usage(uart_usage);
    if (!strcmp(argv[1], "baud")) {
        unsigned long baud;
        if (argc != 4 || !number(argv[3], LAB_MIN_BAUD, LAB_MAX_BAUD, &baud))
            return usage(uart_usage);
        return result(lab_set_baud(dev, baud));
    }
    uint8_t bytes[LAB_MAX_SEND_BYTES];
    size_t count;
    if (!strcmp(argv[1], "hex")) {
        count = argc - 3;
        if (!count || count > sizeof(bytes)) return usage(uart_usage);
        for (size_t i = 0; i < count; ++i) {
            const char *s = argv[i + 3];
            if (strlen(s) != 2 || nibble(s[0]) < 0 || nibble(s[1]) < 0)
                return usage(uart_usage);
            bytes[i] = (nibble(s[0]) << 4) | nibble(s[1]);
        }
    } else if (!strcmp(argv[1], "text")) {
        if (argc != 4 && argc != 6) return usage(uart_usage);
        const char *eol = "";
        if (argc == 6) {
            if (strcmp(argv[4], "--eol")) return usage(uart_usage);
            if (!strcmp(argv[5], "cr")) eol = "\r";
            else if (!strcmp(argv[5], "lf")) eol = "\n";
            else if (!strcmp(argv[5], "crlf")) eol = "\r\n";
            else if (strcmp(argv[5], "none")) return usage(uart_usage);
        }
        size_t text_size = strlen(argv[3]), eol_size = strlen(eol);
        count = text_size + eol_size;
        if (!count || count > sizeof(bytes)) return usage(uart_usage);
        memcpy(bytes, argv[3], text_size);
        memcpy(bytes + text_size, eol, eol_size);
    } else return usage(uart_usage);
    return result(lab_send(dev, bytes, count));
}

static int voice_cmd(int argc, char **argv)
{
    if (argc < 2) return usage(voice_usage);
    if (!strcmp(argv[1], "status") && argc == 2) return result(voice_command(0xc2, NULL, 0));
    if (!strcmp(argv[1], "next") && argc == 2)
        return result(voice_command(LAB_VOICE_CMD_NEXT, NULL, 0));
    if (!strcmp(argv[1], "stop") && argc == 2) return result(voice_command(0xab, NULL, 0));
    if (!strcmp(argv[1], "volume")) {
        if (argc == 2) return result(voice_command(0xc1, NULL, 0));
        unsigned long volume;
        if (argc != 3 || !number(argv[2], 0, 31, &volume)) return usage(voice_usage);
        uint8_t param = volume;
        return result(voice_command(0xae, &param, 1));
    }
    if (!strcmp(argv[1], "play") && argc == 4) {
        unsigned long index;
        if (!number(argv[3], 0, 65535, &index)) return usage(voice_usage);
        uint8_t params[] = {index >> 8, index & 0xff};
        uint8_t cmd;
        if (!strcmp(argv[2], "flash")) cmd = 0xa0;
        else if (!strcmp(argv[2], "usb")) cmd = 0xa6;
        else return usage(voice_usage);
        return result(voice_command(cmd, params, sizeof(params)));
    }
    if (!strcmp(argv[1], "loop") && argc == 3) {
        uint8_t param;
        if (!strcmp(argv[2], "on")) param = LAB_VOICE_LOOP_SINGLE;
        else if (!strcmp(argv[2], "off")) param = LAB_VOICE_LOOP_OFF;
        else return usage(voice_usage);
        return result(voice_command(LAB_VOICE_CMD_LOOP, &param, 1));
    }
    if (!strcmp(argv[1], "query") && argc == 3) {
        uint8_t cmd;
        if (!strcmp(argv[2], "count")) cmd = 0xc3;
        else if (!strcmp(argv[2], "track")) cmd = 0xc9;
        else if (!strcmp(argv[2], "devices")) cmd = 0xca;
        else return usage(voice_usage);
        return result(voice_command(cmd, NULL, 0));
    }
    if (!strcmp(argv[1], "download")) {
        voice_flash_t flash = VOICE_FLASH_AUTO;
        if (argc == 3) {
            if (!strcmp(argv[2], "f1")) flash = VOICE_FLASH_EXTERNAL;
            else if (!strcmp(argv[2], "f6")) flash = VOICE_FLASH_INTERNAL;
            else return usage(voice_usage);
        } else if (argc != 2) return usage(voice_usage);
        return result(voice_download_audio(flash));
    }
    return usage(voice_usage);
}

static int alarm_cmd(int argc, char **argv)
{
    if (argc == 1) {
        printf("alarm %s，PANIC=%s，报警条件=%s\n",
               alarm_armed() ? "on（布防）" : "off（撤防）",
               alarm_panic_active() ? "是" : "否",
               alarm_condition() ? "成立" : "不成立");
        return 0;
    }
    if (argc != 2) return usage("alarm [on|off]");
    bool on;
    if (!on_off(argv[1], &on)) return usage("alarm [on|off]");
    alarm_set_armed(on);
    return 0;
}

static int ble_dev_arg(const char *s)
{
    if (!strcmp(s, "fob")) return BLE_DEV_FOB;
    if (!strcmp(s, "smoke")) return BLE_DEV_SMOKE;
    if (!strcmp(s, "all")) return BLE_DEV_ALL;
    return -1;
}

static int ble_cmd(int argc, char **argv)
{
    if (argc == 1 || (!strcmp(argv[1], "status") && argc == 2)) {
        ble_status();
        return 0;
    }
    if (!strcmp(argv[1], "scan") && argc == 3) {
        bool on;
        if (!on_off(argv[2], &on)) return usage(ble_usage);
        return result(ble_scan_set(on));
    }
    if (!strcmp(argv[1], "pair") && argc == 2)
        return result(ble_central_pairing_toggle());
    if ((!strcmp(argv[1], "connect") || !strcmp(argv[1], "disconnect")) &&
        argc == 3) {
        int dev = ble_dev_arg(argv[2]);
        if (dev < 0) return usage(ble_usage);
        return result(!strcmp(argv[1], "connect")
                      ? ble_connect_dev(dev) : ble_disconnect_dev(dev));
    }
    if (!strcmp(argv[1], "log") && argc == 4) {
        int dev = ble_dev_arg(argv[2]);
        bool on;
        if (dev < 0 || !on_off(argv[3], &on)) return usage(ble_usage);
        return result(ble_log_set(dev, on));
    }
    return usage(ble_usage);
}

static int key_cmd(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) return usage("key");
    uint8_t port1;
    esp_err_t err = key_read_port1(&port1);
    if (err != ESP_OK) return result(err);
    printf("XL9555 P1=0x%02X KEY0=%s KEY2=%s KEY3=%s\n", port1,
           (port1 & (1u << LAB_KEY0_PORT1_BIT)) ? "松开" : "按下",
           (port1 & (1u << LAB_KEY2_PORT1_BIT)) ? "松开" : "按下",
           (port1 & (1u << LAB_KEY3_PORT1_BIT)) ? "松开" : "按下");
    return 0;
}

void shell_init(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    cfg.prompt = "s3> ";
    cfg.max_cmdline_length = LAB_SHELL_LINE_BYTES;
    cfg.max_cmdline_args = LAB_SHELL_MAX_ARGS;
    cfg.task_stack_size = LAB_SHELL_STACK_BYTES;
    cfg.task_priority = LAB_SHELL_PRIORITY;
    /* IDF 6.1 local headers recommend stdio REPL. UART0 is selected in sdkconfig. */
    ESP_ERROR_CHECK(esp_console_new_repl_stdio(&cfg, &repl));
    const esp_console_cmd_t commands[] = {
        {.command="status", .help="显示引脚、实际波特率、OUT/BUSY状态", .func=status_cmd},
        {.command="radar", .help="OUT边沿日志/唤醒/检测开关/查询；watch on打印一次当前状态，之后仅变化打印；完整唤醒时序待确认",
         .hint=radar_usage, .func=radar_cmd},
        {.command="uart", .help="串口原始透传/ESP端波特率；HEX最多256字节，text默认无结尾",
         .hint=uart_usage, .func=uart_cmd},
        {.command="voice", .help="HM01语音命令，play使用16位大端文件索引；next=下一曲；download需启用CONFIG_LAB_VOICE_DOWNLOAD_AUDIO",
         .hint=voice_usage, .func=voice_cmd},
        {.command="alarm", .help="布防/撤防；on=雷达有人时循环播放Flash 8，off=报警静音",
         .hint="alarm [on|off]", .func=alarm_cmd},
        {.command="ble", .help="NimBLE主机：KEY3长按/ble pair配对S3-FOB；订阅EVENT并回ACK/STATE",
         .hint=ble_usage, .func=ble_cmd},
        {.command="key", .help="读取XL9555 P1并显示KEY0/KEY2/KEY3状态", .func=key_cmd},
    };
    /* The REPL already registers help. */
    for (size_t i = 0; i < sizeof(commands)/sizeof(commands[0]); ++i)
        ESP_ERROR_CHECK(esp_console_cmd_register(&commands[i]));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
