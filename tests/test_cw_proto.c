// tests/test_cw_proto.c —— 设备通道 16 字节帧的编解码主机测试。
// 断言与 cw-server/server.js 的 UDP 收发逐字节对齐；改任一侧都要同步本测试。
#include "cw_proto.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    uint8_t f[CW_PROTO_LEN];

    // —— key 上行帧（dev_ms = 设备端毫秒时钟低 16 位）——
    size_t n = cw_proto_build_key(f, 0x1234, 0x00A5, 7024200u, 1, CW_KEY_FLAG_CANCEL, 0x1234);
    assert(n == CW_PROTO_LEN);
    assert(f[0] == 'C' && f[1] == 'W' && f[2] == 1 && f[3] == CW_TYPE_KEY);
    assert(((f[4] << 8) | f[5]) == 0x1234);
    assert(((f[6] << 8) | f[7]) == 0x00A5);
    assert((((uint32_t)f[8] << 24) | ((uint32_t)f[9] << 16) | ((uint32_t)f[10] << 8) | f[11]) == 7024200u);
    assert(f[12] == 1 && f[13] == CW_KEY_FLAG_CANCEL);
    assert(((f[14] << 8) | f[15]) == 0x1234);

    cw_frame_t p;
    assert(cw_proto_parse(f, CW_PROTO_LEN, &p) == 1);
    assert(p.type == CW_TYPE_KEY && p.a == 0x1234 && p.b == 0x00A5 && p.c == 7024200u);
    assert(p.on == 1 && p.flags == CW_KEY_FLAG_CANCEL && p.dev_ms == 0x1234);

    // —— hello 帧：呼号补空格、非法字符替换 ——
    n = cw_proto_build_hello(f, "bh1abc", 7023500u);
    assert(n == CW_PROTO_LEN && f[3] == CW_TYPE_HELLO);
    assert(memcmp(f + 4, "BH1ABC  ", 8) == 0);
    assert(cw_proto_parse(f, CW_PROTO_LEN, &p) == 1);
    assert(strcmp(p.call, "BH1ABC  ") == 0);

    // —— heartbeat ——
    n = cw_proto_build_heartbeat(f, 7, 7100000u);
    assert(n == CW_PROTO_LEN && f[3] == CW_TYPE_HEARTBEAT);
    assert(cw_proto_parse(f, CW_PROTO_LEN, &p) == 1 && p.a == 7 && p.c == 7100000u);

    // —— auth / authack：Global UID 认证（设备首帧）与服务端的应答 ——
    uint8_t guid[CW_GUID_LEN] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77 };
    n = cw_proto_build_auth(f, guid, 7024200u);
    assert(n == CW_PROTO_LEN && f[3] == CW_TYPE_AUTH);
    assert(memcmp(f + 4, guid, CW_GUID_LEN) == 0);
    assert((((uint32_t)f[12] << 24) | ((uint32_t)f[13] << 16) | ((uint32_t)f[14] << 8) | f[15]) == 7024200u);
    assert(cw_proto_parse(f, CW_PROTO_LEN, &p) == 1);
    assert(p.type == CW_TYPE_AUTH && memcmp(p.guid, guid, CW_GUID_LEN) == 0);

    memset(f, 0, sizeof(f));
    f[0] = 'C'; f[1] = 'W'; f[2] = 1; f[3] = CW_TYPE_AUTHACK;
    memcpy(f + 4, "V3KQ9Z  ", 8);              // 虚拟呼号，不足补空格
    f[12] = 0x00; f[13] = 0x09;                // uid = 9
    f[14] = 1;                                  // 1 = 本次新分配
    assert(cw_proto_parse(f, CW_PROTO_LEN, &p) == 1);
    assert(p.type == CW_TYPE_AUTHACK && p.a == 9 && p.status == 1);
    assert(strcmp(p.call, "V3KQ9Z  ") == 0);
    // uid = 0 = 服务端要求重新认证
    f[12] = f[13] = 0;
    assert(cw_proto_parse(f, CW_PROTO_LEN, &p) == 1 && p.a == 0);

    // —— 下行 key 帧（服务端 20 字节扩展版）——
    uint8_t d[20];
    memset(d, 0, sizeof(d));
    d[0] = 'C'; d[1] = 'W'; d[2] = 1; d[3] = CW_TYPE_KEY;
    d[4] = 0x00; d[5] = 0x09;                 // from id
    d[6] = 0x00; d[7] = 0x11;                 // seq
    d[8] = 0x00; d[9] = 0x01; d[10] = 0x02; d[11] = 0x03;   // sts
    d[12] = 0x02; d[13] = 0xBC;               // pitch = 700
    d[14] = 9;                                 // S9
    d[15] = 1;                                 // on
    d[16] = 18;                                // wpm
    d[17] = CW_KEY_FLAG_CANCEL;
    assert(cw_proto_parse_down_key(d, sizeof(d), &p) == 1);
    assert(p.pitch == 700 && p.s == 9 && p.on == 1 && p.wpm == 18);
    assert(p.flags == CW_KEY_FLAG_CANCEL);
    // 旧版 16 字节帧不应读越界，wpm/flags 归零
    assert(cw_proto_parse_down_key(d, 16, &p) == 1);
    assert(p.pitch == 700 && p.wpm == 0 && p.flags == 0);

    // —— 占用表：长度不足要拒绝，不能越界读 ——
    uint8_t o[10 + 8];
    memset(o, 0, sizeof(o));
    o[0] = 'C'; o[1] = 'W'; o[2] = 1; o[3] = CW_TYPE_OCCUPY;
    o[8] = 20; o[9] = 8;                       // 一格 2kHz，共 8 格
    o[10] = 3; o[17] = 5;
    assert(cw_proto_occupy_ok(o, sizeof(o)) == 1);
    assert(cw_proto_parse(o, sizeof(o), &p) == 1);
    assert(p.n == 8 && p.bin_khz_x100 == 20 && p.bins == o + 10 && p.bins[7] == 5);
    assert(cw_proto_occupy_ok(o, 12) == 0);    // 声明 8 格但只有 2 字节数据
    assert(cw_proto_parse(o, 12, &p) == 0);

    // —— 魔数/版本/长度校验 ——
    uint8_t bad[CW_PROTO_LEN];
    memcpy(bad, f, CW_PROTO_LEN);
    bad[0] = 'X';
    assert(cw_proto_parse(bad, CW_PROTO_LEN, &p) == 0);
    memcpy(bad, f, CW_PROTO_LEN);
    bad[2] = 2;
    assert(cw_proto_parse(bad, CW_PROTO_LEN, &p) == 0);
    assert(cw_proto_parse(f, 8, &p) == 0);
    assert(cw_proto_parse(NULL, CW_PROTO_LEN, &p) == 0);

    printf("test_cw_proto: PASS\n");
    return 0;
}
