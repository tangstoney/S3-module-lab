#include "fob_proto.h"

#include <stddef.h>

uint8_t fob_proto_crc8(const uint8_t *data, uint16_t len)
{
    uint8_t crc = 0x00u;
    uint16_t i;
    uint8_t bit;

    if (data == NULL) {
        return 0x00u;
    }

    for (i = 0u; i < len; i++) {
        crc ^= data[i];
        for (bit = 0u; bit < 8u; bit++) {
            if ((crc & 0x80u) != 0u) {
                crc = (uint8_t)((crc << 1) ^ 0x07u);
            } else {
                crc = (uint8_t)(crc << 1);
            }
        }
    }
    return crc;
}

uint16_t fob_proto_build_cmd(uint8_t opcode, uint16_t seq,
                             uint8_t out[FOB_FRAME_CMD_LEN])
{
    out[0] = (uint8_t)FOB_PROTO_MAGIC_CMD;
    out[1] = (uint8_t)FOB_PROTO_VERSION;
    out[2] = (uint8_t)FOB_FRAME_CMD;
    out[3] = opcode;
    out[4] = (uint8_t)(seq & 0xFFu);
    out[5] = (uint8_t)((seq >> 8) & 0xFFu);
    out[6] = fob_proto_crc8(out, 6u);
    return (uint16_t)FOB_FRAME_CMD_LEN;
}

uint16_t fob_proto_build_ack(uint8_t opcode, uint16_t seq, uint8_t result,
                             uint8_t out[FOB_FRAME_ACK_LEN])
{
    out[0] = (uint8_t)FOB_PROTO_MAGIC_HOST;
    out[1] = (uint8_t)FOB_PROTO_VERSION;
    out[2] = (uint8_t)FOB_FRAME_ACK;
    out[3] = opcode;
    out[4] = (uint8_t)(seq & 0xFFu);
    out[5] = (uint8_t)((seq >> 8) & 0xFFu);
    out[6] = result;
    out[7] = fob_proto_crc8(out, 7u);
    return (uint16_t)FOB_FRAME_ACK_LEN;
}

uint16_t fob_proto_build_state(uint8_t protection, uint8_t battery,
                               uint8_t flags,
                               uint8_t out[FOB_FRAME_STATE_LEN])
{
    out[0] = (uint8_t)FOB_PROTO_MAGIC_HOST;
    out[1] = (uint8_t)FOB_PROTO_VERSION;
    out[2] = (uint8_t)FOB_FRAME_STATE;
    out[3] = protection;
    out[4] = battery;
    out[5] = flags;
    out[6] = fob_proto_crc8(out, 6u);
    return (uint16_t)FOB_FRAME_STATE_LEN;
}

int fob_proto_parse(const uint8_t *buf, uint16_t len, fob_proto_frame_t *out)
{
    uint8_t type;

    if ((buf == NULL) || (out == NULL) || (len < 4u)) {
        return -1;
    }
    if (buf[1] != FOB_PROTO_VERSION) {
        return -2;
    }

    type = buf[2];

    if (type == FOB_FRAME_CMD) {
        if ((len < FOB_FRAME_CMD_LEN) ||
            (buf[0] != FOB_PROTO_MAGIC_CMD)) {
            return -1;
        }
        if (fob_proto_crc8(buf, 6u) != buf[6]) {
            return -3;
        }
        out->type = FOB_FRAME_CMD;
        out->u.cmd.opcode = buf[3];
        out->u.cmd.seq = (uint16_t)((uint16_t)buf[4] |
                                    ((uint16_t)buf[5] << 8));
        return 0;
    }

    if ((buf[0] != FOB_PROTO_MAGIC_HOST) || (buf[1] != FOB_PROTO_VERSION)) {
        return -1;
    }

    if (type == FOB_FRAME_ACK) {
        if (len < FOB_FRAME_ACK_LEN) {
            return -1;
        }
        if (fob_proto_crc8(buf, 7u) != buf[7]) {
            return -3;
        }
        out->type = FOB_FRAME_ACK;
        out->u.ack.opcode = buf[3];
        out->u.ack.seq = (uint16_t)((uint16_t)buf[4] |
                                    ((uint16_t)buf[5] << 8));
        out->u.ack.result = buf[6];
        return 0;
    }

    if (type == FOB_FRAME_STATE) {
        if (len < FOB_FRAME_STATE_LEN) {
            return -1;
        }
        if (fob_proto_crc8(buf, 6u) != buf[6]) {
            return -3;
        }
        out->type = FOB_FRAME_STATE;
        out->u.state.protection = buf[3];
        out->u.state.battery = buf[4];
        out->u.state.flags = buf[5];
        return 0;
    }

    return -4;
}
