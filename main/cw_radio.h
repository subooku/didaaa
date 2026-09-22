// main/cw_radio.h —— CW 网络电台应用：对外只暴露生命周期与按键接口，内部状态不外泄。
#pragma once

#include "bsp_button.h"
#include "esp_err.h"
#include <stdint.h>

// 固件版本号，ABOUT ME 与 FIRMWARE 两页显示，OTA 时也用它跟服务器上的版本比对。
// ★ 发新版时改这一处（tools/publish-fw.sh 会从这里抓版本号写进 version.json，
//   不用手写两遍）。
#define CW_FW_VERSION "1.1.3"

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
