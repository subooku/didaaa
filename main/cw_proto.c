// main/cw_proto.c —— 16 字节二进制帧的编解码实现。
// 网络字节序为大端，与服务端 writeUInt16BE / writeUInt32BE 对应。
#include "cw_proto.h"
#include <string.h>

static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t get_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void put_header(uint8_t *p, uint8_t type) {
    p[0] = CW_PROTO_MAGIC0;
    p[1] = CW_PROTO_MAGIC1;
    p[2] = CW_PROTO_VER;
    p[3] = type;
}

size_t cw_proto_build_key(uint8_t out[CW_PROTO_LEN], uint16_t uid, uint16_t seq,
                          uint32_t freq, int on, uint8_t flags, uint16_t dev_ms) {
    if (!out) return 0;
    memset(out, 0, CW_PROTO_LEN);
    put_header(out, CW_TYPE_KEY);
    put_u16(out + 4, uid);
    put_u16(out + 6, seq);
    put_u32(out + 8, freq);
    out[12] = on ? 1 : 0;
    out[13] = flags;
    // 设备端时钟（低 16 位）。服务端下发的 key 帧这两字节是 s/wpm 之类，不能复用，
    // 所以只有上行（设备 → 服务端）才写它。
    put_u16(out + 14, dev_ms);
    return CW_PROTO_LEN;
}

size_t cw_proto_build_hello(uint8_t out[CW_PROTO_LEN], const char *call, uint32_t freq) {
    if (!out) return 0;
    memset(out, 0, CW_PROTO_LEN);
    put_header(out, CW_TYPE_HELLO);
    // 先用 strlen 定长再逐字节取，避免读到 call 缓冲区之外（呼号不足 8 位时的越界读）。
    size_t clen = call ? strlen(call) : 0;
    for (int i = 0; i < CW_CALL_LEN; i++) {
        char c = ((size_t)i < clen) ? call[i] : ' ';   // 不足 8 位补空格
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');   // 呼号一律大写
        if (c < 0x20 || c > 0x7e) c = ' ';
        out[4 + i] = (uint8_t)c;
    }
    put_u32(out + 12, freq);
    return CW_PROTO_LEN;
}

size_t cw_proto_build_heartbeat(uint8_t out[CW_PROTO_LEN], uint16_t uid, uint32_t freq) {
    if (!out) return 0;
    memset(out, 0, CW_PROTO_LEN);
    put_header(out, CW_TYPE_HEARTBEAT);
    put_u16(out + 4, uid);
    put_u16(out + 6, 0);
    put_u32(out + 8, freq);
    return CW_PROTO_LEN;
}

size_t cw_proto_build_auth(uint8_t out[CW_PROTO_LEN], const uint8_t guid[CW_GUID_LEN],
                           uint32_t freq) {
    if (!out) return 0;
    memset(out, 0, CW_PROTO_LEN);
    put_header(out, CW_TYPE_AUTH);
    // 8 字节原样搬过去：MAC 的 6 字节 + 2 字节保留（默认 0）。
    // 不做任何规范化（大小写、分隔符）—— 它就是一串二进制 ID，不是给人看的。
    for (int i = 0; i < CW_GUID_LEN; i++) out[4 + i] = guid ? guid[i] : 0;
    put_u32(out + 12, freq);
    return CW_PROTO_LEN;
}

int cw_proto_occupy_ok(const uint8_t *buf, size_t len) {
    if (!buf || len < 10) return 0;
    if (buf[0] != CW_PROTO_MAGIC0 || buf[1] != CW_PROTO_MAGIC1) return 0;
    if (buf[2] != CW_PROTO_VER || buf[3] != CW_TYPE_OCCUPY) return 0;
    if (buf[9] == 0 || buf[9] > CW_OCC_MAX_ITEMS) return 0;
    return len >= (size_t)10 + 3 * buf[9];
}

int cw_proto_parse_occupy(const uint8_t *buf, size_t len, cw_frame_t *out) {
    if (!out) return 0;
    if (!cw_proto_occupy_ok(buf, len)) return 0;
    memset(out, 0, sizeof(*out));
    out->type         = CW_TYPE_OCCUPY;
    out->ts           = get_u32(buf + 4);
    out->bin_khz_x100 = buf[8];
    out->n            = buf[9];     // 条目数，不是格子总数
    out->bins         = buf + 10;   // 三元组流：[idx_lo, idx_hi, cnt]
    return 1;
}

int cw_proto_parse(const uint8_t *buf, size_t len, cw_frame_t *out) {
    if (!buf || !out || len < CW_PROTO_LEN) return 0;
    if (buf[0] != CW_PROTO_MAGIC0 || buf[1] != CW_PROTO_MAGIC1) return 0;
    if (buf[2] != CW_PROTO_VER) return 0;

    memset(out, 0, sizeof(*out));
    out->type = buf[3];
    out->a = get_u16(buf + 4);
    out->b = get_u16(buf + 6);
    out->c = get_u32(buf + 8);

    switch (out->type) {
    case CW_TYPE_KEY:
        // 设备发出的 key 帧：[12]=on [13]=flags [14..16)=设备端毫秒时钟
        out->on    = buf[12] ? 1 : 0;
        out->flags = buf[13];
        out->dev_ms = get_u16(buf + 14);
        // 服务端下发的 key 帧：[12..14)=pitch [14]=s [15]=on
        out->pitch = get_u16(buf + 12);
        out->s     = buf[14];
        // 方向由调用方决定：只有从服务端收到的帧才读 [15]
        break;
    case CW_TYPE_HELLO:
        for (int i = 0; i < CW_CALL_LEN; i++) out->call[i] = (char)buf[4 + i];
        out->call[CW_CALL_LEN] = '\0';
        break;
    case CW_TYPE_HEARTBEAT:
        break;
    case CW_TYPE_AUTH:
        for (int i = 0; i < CW_GUID_LEN; i++) out->guid[i] = buf[4 + i];
        break;
    case CW_TYPE_AUTHACK:
        // 与上行 auth 同构，但方向相反：[4..12) 是服务端下发的虚拟呼号，
        // uid 挪到了 [12..14)（通用头部解析把 [4..6) 当成了 uid，这里覆盖掉）。
        // 呼号不足 8 位补的是空格，调用方要自己剔（见 cw_net 的 on_authack）。
        for (int i = 0; i < CW_CALL_LEN; i++) out->call[i] = (char)buf[4 + i];
        out->call[CW_CALL_LEN] = '\0';
        out->a      = get_u16(buf + 12);
        out->status = buf[14];
        break;
    case CW_TYPE_OCCUPY:
        if (!cw_proto_occupy_ok(buf, len)) return 0;
        out->ts             = get_u32(buf + 4);
        out->bin_khz_x100   = buf[8];
        out->n              = buf[9];
        out->bins           = buf + 10;
        break;
    default:
        return 0;
    }
    return 1;
}

// 服务端下发的 key 帧用 [15] 表示 on；单独提供一个读取入口，避免和上行帧混淆。
int cw_proto_parse_down_key(const uint8_t *buf, size_t len, cw_frame_t *out) {
    uint16_t pitch;
    if (!cw_proto_parse(buf, len, out) || out->type != CW_TYPE_KEY) return 0;
    // 服务端下发的 key 帧：[12..14)=pitch（大端），[14]=s，[15]=on，
    // 扩展的 20 字节版本还带 [16]=发报方 wpm、[17]=flags（bit0 = 丢弃该元素）。
    pitch = (uint16_t)((buf[12] << 8) | buf[13]);
    out->pitch = pitch;
    out->s     = buf[14];
    out->on    = buf[15] ? 1 : 0;
    out->wpm   = len >= 17 ? buf[16] : 0;
    out->flags = len >= 18 ? buf[17] : 0;
    return 1;
}
