/*
 * fob_proto.h - 遥控器 <-> 主机的应用层帧（协议草案 v1）
 *
 * 命令帧（遥控器 -> 主机）:
 *   0xA5 | ver | 0x01 | opcode | seq_lo | seq_hi | crc8
 *
 * ACK 帧（主机 -> 遥控器）:
 *   0x5A | ver | 0x02 | opcode | seq_lo | seq_hi | result | crc8
 *
 * 状态帧（主机 -> 遥控器）:
 *   0x5A | ver | 0x03 | protection | battery | rsv | crc8
 */
#ifndef FOB_PROTO_H
#define FOB_PROTO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FOB_PROTO_MAGIC_CMD 0xA5u
#define FOB_PROTO_MAGIC_HOST 0x5Au
#define FOB_PROTO_VERSION 0x01u

#define FOB_FRAME_CMD 0x01u
#define FOB_FRAME_ACK 0x02u
#define FOB_FRAME_STATE 0x03u

#define FOB_FRAME_CMD_LEN 7u
#define FOB_FRAME_ACK_LEN 8u
#define FOB_FRAME_STATE_LEN 7u

typedef struct {
    uint8_t opcode;
    uint16_t seq;
} fob_proto_cmd_t;

typedef struct {
    uint8_t opcode;
    uint16_t seq;
    uint8_t result;
} fob_proto_ack_t;

typedef struct {
    uint8_t protection;
    uint8_t battery;
    uint8_t flags;
} fob_proto_state_t;

typedef struct {
    uint8_t type;
    union {
        fob_proto_cmd_t cmd;
        fob_proto_ack_t ack;
        fob_proto_state_t state;
    } u;
} fob_proto_frame_t;

/* CRC-8, poly 0x07, init 0x00 */
uint8_t fob_proto_crc8(const uint8_t *data, uint16_t len);

/* 构造命令帧，返回帧长度 */
uint16_t fob_proto_build_cmd(uint8_t opcode, uint16_t seq,
                             uint8_t out[FOB_FRAME_CMD_LEN]);

/* 以下两个用于主机侧模拟 / 联调 / 测试 */
uint16_t fob_proto_build_ack(uint8_t opcode, uint16_t seq, uint8_t result,
                             uint8_t out[FOB_FRAME_ACK_LEN]);
uint16_t fob_proto_build_state(uint8_t protection, uint8_t battery,
                               uint8_t flags,
                               uint8_t out[FOB_FRAME_STATE_LEN]);

/* 解析一帧。返回 0 成功，负数失败（格式 / 版本 / CRC） */
int fob_proto_parse(const uint8_t *buf, uint16_t len, fob_proto_frame_t *out);

#ifdef __cplusplus
}
#endif

#endif /* FOB_PROTO_H */
