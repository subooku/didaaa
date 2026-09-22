// main/cw_proto.h —— CW 设备通道的 16 字节二进制帧编解码（纯逻辑，可主机测试）。
// 与 cw-server/server.js 的 UDP 设备通道逐字节对齐；改动任一侧都要同步另一侧。
//
// 帧头（所有帧共 10 字节定长部分之外的布局见下）：
//   [0..1] 'C' 'W'   魔数
//   [2]    版本号 = 1
//   [3]    帧类型
// 设备 → 服务端
//   1 = key      [4..6) uid  [6..8) seq  [8..12) freq  [12] on  [13] flags
//                [14..16) 设备端毫秒时钟低 16 位 —— 点划时长由它算，见下
//   2 = hello    [4..12) 呼号(8B ASCII)  [12..16) freq
//   4 = heartbeat[4..6) uid              [8..12) freq
//   5 = auth     [4..12) Global UID(8B)  [12..16) freq      ← 身份认证，开机第一帧
// 服务端 → 设备
//   1 = key      [4..6) from [6..8) seq [8..12) sts [12..14) pitch [14] s [15] on
//   2 = ack      [4..6) uid  [8..12) fmin  [12..16) fmax
//   3 = occupy   [4..8) ts   [8] bin/100   [9] n      [10..10+n) 每格台站数
//   6 = authack  [4..12) 虚拟呼号(8B) [12..14) uid [14] status（0=已有 1=新分配）
//                uid = 0 表示"我不认识你，请重新认证"（服务端重启后会出现）
#pragma once

#include <stddef.h>
#include <stdint.h>

#define CW_PROTO_LEN     16
#define CW_PROTO_MAGIC0  'C'
#define CW_PROTO_MAGIC1  'W'
#define CW_PROTO_VER     1

#define CW_TYPE_KEY       1
#define CW_TYPE_HELLO     2
#define CW_TYPE_OCCUPY    3
#define CW_TYPE_HEARTBEAT 4
#define CW_TYPE_AUTH      5
#define CW_TYPE_AUTHACK   6

// Global UID 长度：出厂 MAC 的 48 位放在前 6 字节，后 2 字节留作产线扩展（默认 0）。
// 它是这块板子唯一不会变的东西，服务端拿它做"呼号永久绑定"的键。
#define CW_GUID_LEN       8

// ★ 上行 key 帧为什么要带设备端时钟（[14..16)，低 16 位、65.5 秒回绕一次）：
//   以前点划时长是服务端按"收到 keydown → 收到 keyup"算的，中间夹着设备端的
//   按键队列、LVGL 锁等待（最多 200ms）和两次 Wi-Fi 延迟抖动。18 WPM 的点只有
//   66ms、划 200ms，判定线在 133ms —— 抖动吃掉 70ms 就把点判成划，
//   表现正是"我发的是 E，服务端显示 T"。带上设备端时刻后，解码端用同一个时基
//   做差，网络与服务端抖动被完全排除，长短只由手决定。
#define CW_KEY_HAS_DEV_MS  1

// key 帧 flags 位：该元素作废（例如 OK 长按被用作菜单键），收报端只停音不解字符。
#define CW_KEY_FLAG_CANCEL 0x01

#define CW_CALL_LEN 8

typedef struct {
    uint8_t  type;
    uint16_t a;        // key: uid / ack: uid
    uint16_t b;        // key: seq
    uint32_t c;        // key: freq 或 sts
    uint16_t pitch;    // 服务端下发的收听音调(Hz)，零拍时 = 700（等于侧音）
    uint8_t  s;        // S 表 0..9
    uint8_t  on;       // 1=按下 0=松开
    uint8_t  flags;
    uint8_t  wpm;      // 服务端下发帧：[16] 发报方速度，收报端据此定夺点长
    char     call[CW_CALL_LEN + 1];
    uint32_t fmin, fmax;
    uint32_t ts;
    uint8_t  bin_khz_x100;   // 占用表一格代表的 Hz/100
    uint8_t  n;              // 占用表格数
    const uint8_t *bins;     // 指向帧内数据区，非拷贝
    uint16_t dev_ms;         // 上行 key 帧：设备端毫秒时钟低 16 位（0 = 老固件，没有）
    uint8_t  guid[CW_GUID_LEN];   // auth 帧：设备的 Global UID
    uint8_t  status;         // authack 帧：0 = 已有呼号  1 = 本次新分配
} cw_frame_t;

// —— 组帧（返回写入的字节数，固定 16）——
// dev_ms 传设备端 esp_timer_get_time()/1000 的低 16 位；传 0 表示不带（兼容老服务端）。
size_t cw_proto_build_key(uint8_t out[CW_PROTO_LEN], uint16_t uid, uint16_t seq,
                          uint32_t freq, int on, uint8_t flags, uint16_t dev_ms);
size_t cw_proto_build_hello(uint8_t out[CW_PROTO_LEN], const char *call, uint32_t freq);
size_t cw_proto_build_heartbeat(uint8_t out[CW_PROTO_LEN], uint16_t uid, uint32_t freq);
// 认证帧：把 Global UID 交给服务端换虚拟呼号。未认证前不发键控、不进名单。
size_t cw_proto_build_auth(uint8_t out[CW_PROTO_LEN], const uint8_t guid[CW_GUID_LEN],
                           uint32_t freq);

// —— 解帧：成功返回 1，魔数/版本/长度不合法返回 0 ——
int cw_proto_parse(const uint8_t *buf, size_t len, cw_frame_t *out);

// 解析【服务端下发】的 key 帧：on 在 [15]，另外读 [12..14) 为收听音调、[14] 为 S 值。
int cw_proto_parse_down_key(const uint8_t *buf, size_t len, cw_frame_t *out);

// 占用表帧的数据区长度是否足够（防越界读）。
int cw_proto_occupy_ok(const uint8_t *buf, size_t len);
