// main/cw_net.h —— CW 电台的网络通道：UDP 键控（低延迟）+ MQTT 信令（presence/名单/占用表）。
// 键控走 UDP 是因为 CW 靠节奏比例解码，迟到但正确的包对实时音频毫无意义；
// 名单、在线状态、占用表这些"可丢可迟"的信令交给 MQTT，白拿 retain 与 LWT 遗嘱。
#pragma once

#include "esp_err.h"
#include "sdkconfig.h"
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

// ---------------------------------------------------------------------------
// 三条通路怎么由"一个服务器地址"派生出来
// ---------------------------------------------------------------------------
// 配网页（或 Kconfig）里只填一个 host —— IP 或域名都行。UDP 键控、MQTT 信令、
// 固件下载三处的端口和协议都由它加 CW_TLS 这一个开关决定，用户不用分别配端口。
//
//   CW_TLS 关（默认，局域网）：UDP:21303  mqtt://host:1883  http://host:21301
//   CW_TLS 开（公网推荐）    ：UDP:21303  mqtts://host:8883 https://host/fw/*（443）
//
// ★ 除了上面这两套，地址栏还可以直接带协议前缀，把三条路整个搬到 WebSocket 上：
//   ws://host   → 键控 /cw + MQTT over WS /mqtt（明文，只在本机调试时用）
//   wss://host  → 键控 /cw + MQTT over WSS /mqtt + https 固件，全部走 443
//   这么做的唯一目的是过 Cloudflare：它只代理 443 上的 HTTP/WebSocket，
//   UDP 一律不代理（要任意端口得买 Enterprise 的 Spectrum）。
//   运行时判断，不用改编译开关 —— 见 cw_net.c 的 parse_server_scheme()。
//
// ★ 明文 UDP 那一跳没有加密：UDP 之上没有 TLS（只有 DTLS，而 esp-tls 没包它）。
//   所以"安全"在这一路上不是靠加密，而是靠①限制谁能连（WireGuard 隧道 / 防火墙）
//   ②帧级签名。见 docs/deployment.md 的「UDP 那一跳」。
#if CONFIG_CW_TLS
#define CW_MQTT_SCHEME  "mqtts"
#define CW_MQTT_PORT    8883
#define CW_FW_SCHEME    "https"
#define CW_FW_PORT      443
#else
#define CW_MQTT_SCHEME  "mqtt"
#define CW_MQTT_PORT    1883
#define CW_FW_SCHEME    "http"
#define CW_FW_PORT      CONFIG_CW_FW_PORT
#endif

// 服务器证书的信任根：main/root_ca.pem，由 main/CMakeLists.txt 用 EMBED_TXTFILES
// 编进 .rodata（该模式会自动补 '\0'，可以直接当 C 字符串传给 mbedtls）。
// ★ 不再包在 CONFIG_CW_TLS 里：地址填 wss:// 时运行时才决定要校验，编译期判断不了。
extern const uint8_t cw_root_ca_pem_start[] asm("_binary_root_ca_pem_start");
extern const uint8_t cw_root_ca_pem_end[]   asm("_binary_root_ca_pem_end");
#define CW_ROOT_CA_PEM  ((const char *)cw_root_ca_pem_start)

// ---------------------------------------------------------------------------
// 服务器地址那一栏的"当前解释"（给 OTA / 界面用）
// ---------------------------------------------------------------------------
// 地址栏既可以填裸域名/IP，也可以带 ws:// wss:// 前缀。剥前缀这件事只在 cw_net 里做一次，
// 别处（OTA、MQTT、界面）一律问这三个函数要，避免各写各的解析规则。
bool        cw_net_use_ws(void);        // 这一栏是 ws:// 或 wss:// 吗
const char *cw_net_srv_host(void);      // 剥掉前缀后的主机名（IP / 域名，永不为空前缀）
int         cw_net_fw_port(void);       // 固件/网页端口：wss 443、ws 80，否则 CONFIG_CW_FW_PORT
bool        cw_net_fw_tls(void);        // 固件下载走 https 吗（wss:// 或 CONFIG_CW_TLS）

esp_err_t cw_net_start(uint32_t freq, int wpm);
void     cw_net_stop(void);
// 不联网、只把呼号与服务器地址取好（跳过配网后离线开机时用）。
void     cw_net_ids_init(void);

// 由 UI / 按键上下文调用：立即发出键控帧（不发则无所谓，UDP 允许丢）。
// t_ms 是这一下按下/松开发生的时刻（按键回调里打的时间戳，不是"现在"）：
// 它会被写进帧里，服务端据此算点划长度 —— 传 0 表示让函数自己取当前时刻。
void     cw_net_send_key(int on, uint8_t flags, int64_t t_ms);
// 频率或速度变化：UDP 帧里带频率，MQTT presence 也要更新（服务端靠它出名单）。
void     cw_net_tune(uint32_t freq);
void     cw_net_set_wpm(int wpm);
// 在线/离线（组合键切换）。离线 = 不发键控、不上报频率，并向服务器报一条 retain 的
// presence "0"，让别人名单里看不到我。心跳照发，保住 uid。
void     cw_net_set_online(bool on);
// 通联用的呼号（进协议和 MQTT 主题的那个）：虚拟呼号优先，没认证过之前拿 Ham 顶着，
// 两个都没有返回空串 —— 不拿 MAC 凑假号。
// ★ 屏幕上显示的顺序相反（Ham 优先），见 cw_radio.c 的 top_call()。
const char *cw_net_call(void);
// 服务端分配的 uid（认证的 ack 里带回，0 = 还没认证上）。
// 名单里认自己要用它：呼号可能被服务端换过，比字符串不靠谱。
uint16_t    cw_net_uid(void);
// 完成 Global UID 身份认证了吗。没认证 = 不进名单、不发键控、不收转发。
bool        cw_net_authed(void);

// ----- 双呼号 -----
// Ham：用户自己的真实呼号，配网页里填，本地可改。
// Virtual：服务器按 Global UID 下发的 6 位虚拟呼号（V + 5 位），永久绑定，本机改不了。
const char *cw_net_ham(void);
const char *cw_net_vcall(void);

// 设备身份（ABOUT ME 与 WI-FI SETUP 两页共用）。
// 出厂 MAC 是产线烧进 eFuse 的，是这块板子唯一不会变的东西。
// ★ 设备 ID 就是 MAC 的完整 48 位，跟 Global UID 是同一个身份的两种写法：
//   Device ID  = 12 位十六进制，屏幕上给人看（4C11AE2F8DC8）
//   Global UID = 线上那 8 字节（MAC + 2 字节保留），服务端拿它做绑定的键
//   服务端把身份键统一截到 48 位，所以两者指向同一条记录。
uint64_t    cw_net_device_id(void);      // MAC 的 48 位（高 16 位恒为 0）
const char *cw_net_device_id_hex(void);  // "4C11AE2F8DC8"，12 位大写十六进制
const char *cw_net_mac_str(void);        // "XX:XX:XX:XX:XX:XX"

// Global UID：8 字节的硬件唯一身份，服务端拿它做"呼号永久绑定"的键。
// 前 6 字节是出厂 MAC，后 2 字节留作产线扩展（当前填 0）。
const uint8_t *cw_net_guid(void);
const char *cw_net_guid_hex(void);      // 16 位大写十六进制，给人看 / 打印日志用
// 本机 IP。拿不到（没连上、还在等 DHCP）返回 "--"，不显示 0.0.0.0 这种假信息。
const char *cw_net_ip_str(void);
