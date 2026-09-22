// main/cw_radio.h —— CW 网络电台应用：对外只暴露生命周期与按键接口，内部状态不外泄。
#pragma once

#include "bsp_button.h"
#include "esp_err.h"
#include <stdint.h>

// ★ 1.1.14：设备端支持 WebSocket —— 服务器地址填 "wss://host" 就把键控、MQTT、固件
//   三条路全搬到 443 上，为的是过 Cloudflare（它不代理 UDP，也不代理 21301/1883 这类
//   自定义端口）。填裸域名或 IP 仍是原来的 UDP 直连，两种部署共用一份固件，改地址就能
//   来回切，不需要重新烧。改完看启动日志里"键控走 WebSocket"那一行确认进了哪个模式。
// 固件版本号，ABOUT ME 与 FIRMWARE 两页显示，OTA 时也用它跟服务器上的版本比对。
// ★ 发新版时改这一处（tools/publish-fw.sh 会从这里抓版本号写进 version.json，
//   不用手写两遍）。
// ★ OTA 只看"服务端版本 > 本机版本"，所以每次发版都必须抬版本号，否则服务端相同
//   版本会被判定"无需升级"直接跳过。历史：1.1.7 → 1.1.8（本轮新端口）。
// ★ 1.1.9：UDP 6000→21306、OTA 端口 8080→21301。因为老固件把端口写死在二进制里，
//   服务端换端口后老设备连不上、也拿不到新固件，无法 OTA 自救 —— 那一版只能串口烧录。
// ★ 1.1.13：BASE STATION 页的地址保持 20 号大字不变 —— 1.1.12 试过按长度自动降字号
//   （20→16→14），字缩到 14 是能一行放下，但跟别页的取值不是一个量级，看着突兀。
//   改成字号不动、接受长域名换两行，只把提示和按钮按地址的实际占高往下挪，避免压字。
// ★ 1.1.11：状态行不再写 ONLINE / OFFLINE —— 那两个词会被理解成"我上没上线、能不能
//   发报"，而能不能发报看的是屏幕外圈（UP+DOWN 那个开关），两件事撞名，排查"为什么
//   发不出去"时特别容易误判。现在统一写 STATION，只用颜色区分到服务器通不通：
//   绿 = 连上了（UDP 已注册 + MQTT 已连），黄 = 没连上。菜单里没有颜色，写作
//   STATION UP / STATION DOWN。
// ★ 1.1.10：UDP 21306→21303（与服务端对齐）。
//   ⚠ 端口必须改 sdkconfig 里的 CONFIG_CW_UDP_PORT —— 只改 Kconfig.projbuild 的 default
//   不生效：sdkconfig 里已有该键时 default 会被忽略。本版之前就栽在这里（default 写了
//   21303，编进固件的还是 21306，设备一直连不上）。改完看 build/config/sdkconfig.h 复核。
//   改完记得两条：① 重新 idf.py build；② 把 build/*.bin 发布到服务器的 /fw/ 下
//   （tools/publish-fw.sh 会顺便更新 version.json）。
#define CW_FW_VERSION "1.1.15"

void cw_radio_enter(void);
void cw_radio_exit(void);
void cw_radio_key(bsp_btn_t btn, bsp_btn_ev_t ev);
// 电键的起落时刻：由按键回调同步调用，只记时刻、不碰 LVGL。声音的节奏全靠它，
// 派发链路（队列 + LVGL 锁）再堵也不影响点划时长。
void cw_radio_key_edge(bsp_btn_t btn, bsp_btn_ev_t ev);
esp_err_t cw_radio_start(void);
esp_err_t cw_radio_stop(void);

// ----- 以下由 cw_net.c 回调（网络任务上下文，不得直接碰 LVGL）-----
typedef enum {
    CW_NET_IDLE = 0,
    CW_NET_WIFI,
    CW_NET_LINK,      // UDP 已注册 + MQTT 已连接
    CW_NET_ERROR,
} cw_net_state_t;

void cw_radio_on_net_state(cw_net_state_t state);
// wpm = 发报方速度（服务端带下来），收报端据此决定点长；<=0 时用本机设置兜底。
// flags = 服务端透传的键控标志位（CW_KEY_FLAG_CANCEL = 该元素作废）。
// from_uid = 发报方的服务端 uid。设备拿它在名单里查对方的频率，不同频的直接丢掉
// —— 名单可能滞后于服务端（presence 150ms 节流），这一层是最后一道保险。
void cw_radio_on_rx_key(int on, int pitch_hz, int s, int wpm, int flags, uint16_t from_uid);
void cw_radio_on_roster(const char *text);          // "CALL,freq;CALL,freq;..."
void cw_radio_on_occupy(const uint8_t *bins, int n);
void cw_radio_on_time(int64_t unix_ms);
void cw_radio_on_chat(const char *from, const char *msg);
