#include "fob_host.h"

#include <stdio.h>
#include <string.h>
#include "config.h"
#include "esp_err.h"
#include "fob_proto.h"
#include "nvs.h"

#define FOB_HOST_CONN_NONE 0xFFFFu
#define FOB_HOST_NVS_NS "fob"

/* Same values as the fob's fob_types.h; fob_proto.h only defines the frames. */
#define FOB_CMD_ARM          0x01u
#define FOB_CMD_DISARM       0x02u
#define FOB_CMD_PANIC        0x03u
#define FOB_CMD_CLEAR_ALARM  0x04u
#define FOB_ACK_OK               0x00u
#define FOB_ACK_REJECTED_STATE   0x01u
#define FOB_ACK_REJECTED_AUTH    0x02u
#define FOB_ACK_UNKNOWN_CMD      0x03u

static fob_host_tx_fn s_tx;
static uint16_t s_conn = FOB_HOST_CONN_NONE;
static uint16_t s_host_handle;
static bool s_link_ready;
static uint16_t s_last_seq;
static uint8_t s_last_op;
static uint8_t s_last_result;
static bool s_have_last;
static uint8_t s_last_ack[FOB_FRAME_ACK_LEN];
static bool s_last_ack_valid;

static void fob_host_persist(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(FOB_HOST_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        printf("[fob] NVS打开失败 %s，seq不持久化\n", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u16(h, "seq", s_last_seq);
    if (err == ESP_OK) err = nvs_set_u8(h, "op", s_last_op);
    if (err == ESP_OK) err = nvs_set_u8(h, "res", s_last_result);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK)
        printf("[fob] seq持久化失败 %s\n", esp_err_to_name(err));
}

void fob_host_init(void)
{
    s_tx = NULL;
    s_conn = FOB_HOST_CONN_NONE;
    s_host_handle = 0;
    s_link_ready = false;
    s_last_seq = 0;
    s_last_op = 0;
    s_last_result = 0;
    s_have_last = false;
    s_last_ack_valid = false;

    nvs_handle_t h;
    if (nvs_open(FOB_HOST_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint16_t seq = 0;
    uint8_t op = 0;
    uint8_t res = 0;
    if (nvs_get_u16(h, "seq", &seq) == ESP_OK) {
        s_last_seq = seq;
        s_have_last = true;
    }
    if (nvs_get_u8(h, "op", &op) == ESP_OK) s_last_op = op;
    if (nvs_get_u8(h, "res", &res) == ESP_OK) s_last_result = res;
    nvs_close(h);
    if (s_have_last)
        printf("[fob] 已恢复seq=%u op=%u\n", (unsigned)s_last_seq,
               (unsigned)s_last_op);
}

void fob_host_set_tx(fob_host_tx_fn fn)
{
    s_tx = fn;
}

void fob_host_on_link_ready(uint16_t conn_handle, uint16_t host_handle)
{
    bool was_ready = s_link_ready;
    bool changed = (s_conn != conn_handle) || (s_host_handle != host_handle);
    s_conn = conn_handle;
    s_host_handle = host_handle;
    s_link_ready = true;
    if (!was_ready || changed) {
        printf("[fob] 链路就绪 conn=%u host=0x%04X，发送STATE\n",
               (unsigned)conn_handle, (unsigned)host_handle);
        fob_host_send_state();
    }
}

void fob_host_on_link_down(void)
{
    s_conn = FOB_HOST_CONN_NONE;
    s_host_handle = 0;
    s_link_ready = false;
}

void fob_host_send_state(void)
{
    if (!s_tx || s_conn == FOB_HOST_CONN_NONE || !s_host_handle) return;

    uint8_t protection;
    if (alarm_is_alarming()) protection = 3;
    else if (alarm_armed()) protection = 2;
    else protection = 1;

    uint8_t frame[FOB_FRAME_STATE_LEN];
    fob_proto_build_state(protection, 0xFF, 0, frame);
    s_tx(s_conn, s_host_handle, frame, sizeof(frame));
}

static uint8_t fob_host_execute(uint8_t opcode)
{
    switch (opcode) {
    case FOB_CMD_ARM:
        alarm_set_armed(true);
        return FOB_ACK_OK;
    case FOB_CMD_DISARM:
        if (!alarm_armed() && !alarm_panic_active())
            return FOB_ACK_REJECTED_STATE;
        alarm_set_panic(false);
        alarm_set_armed(false);
        return FOB_ACK_OK;
    case FOB_CMD_PANIC:
        alarm_set_panic(true);
        return FOB_ACK_OK;
    case FOB_CMD_CLEAR_ALARM:
        if (!alarm_panic_active())
            return FOB_ACK_REJECTED_STATE;
        alarm_set_panic(false);
        return FOB_ACK_OK;
    default:
        return FOB_ACK_UNKNOWN_CMD;
    }
}

void fob_host_on_event(uint16_t conn_handle, const uint8_t *data, size_t len)
{
    fob_proto_frame_t frame;
    if (!data || fob_proto_parse(data, (uint16_t)len, &frame) != 0 ||
        frame.type != FOB_FRAME_CMD) {
        printf("[fob] 命令帧解析失败 len=%u\n", (unsigned)len);
        return;
    }

    uint8_t opcode = frame.u.cmd.opcode;
    uint16_t seq = frame.u.cmd.seq;

    if (s_have_last && seq == s_last_seq && opcode == s_last_op) {
        if (!s_last_ack_valid) {
            /* Rebuild the ACK after a host reboot; the fob may retry. */
            fob_proto_build_ack(opcode, seq, s_last_result, s_last_ack);
            s_last_ack_valid = true;
        }
        printf("[fob] 重复命令 op=%u seq=%u，重发ACK\n",
               (unsigned)opcode, (unsigned)seq);
        if (s_tx && s_conn != FOB_HOST_CONN_NONE && s_host_handle)
            s_tx(s_conn, s_host_handle, s_last_ack, sizeof(s_last_ack));
        return;
    }
    if (s_have_last && (int16_t)(seq - s_last_seq) <= 0) {
        printf("[fob] 旧seq op=%u seq=%u（last=%u），忽略\n",
               (unsigned)opcode, (unsigned)seq, (unsigned)s_last_seq);
        return;
    }

    s_conn = conn_handle;
    uint8_t result = fob_host_execute(opcode);
    printf("[fob] 命令 op=%u seq=%u result=%u\n", (unsigned)opcode,
           (unsigned)seq, (unsigned)result);

    uint8_t ack[FOB_FRAME_ACK_LEN];
    fob_proto_build_ack(opcode, seq, result, ack);
    if (s_tx && s_host_handle)
        s_tx(s_conn, s_host_handle, ack, sizeof(ack));

    memcpy(s_last_ack, ack, sizeof(ack));
    s_last_ack_valid = true;
    s_last_seq = seq;
    s_last_op = opcode;
    s_last_result = result;
    s_have_last = true;

    fob_host_send_state();
    fob_host_persist();
}
