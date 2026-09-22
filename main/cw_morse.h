// main/cw_morse.h —— 摩斯码编解码与点划判定的纯逻辑层。
// 本文件不依赖 ESP-IDF / LVGL / FreeRTOS，可被 tests/ 下的主机测试直接编译运行。
#pragma once

#include <stddef.h>
#include <stdint.h>

// 一个字符最多 6 个元素（数字/标点），留 1 字节余量给结尾 '\0'。
#define CW_MORSE_MAX_CODE 7

// 字符 → 码。返回写入 out 的字符数；未知字符返回 0。
// out 需至少 CW_MORSE_MAX_CODE 字节。
size_t cw_morse_encode(char ch, char out[CW_MORSE_MAX_CODE]);

// 码 → 字符。未知返回 '\0'。
char cw_morse_decode(const char *code);

// 点划判定：元素时长 >= 2 个点长算划（标准比例 1:3，中点 2）。
// dit_ms <= 0 时返回 '\0'。
char cw_morse_element(int32_t dur_ms, int32_t dit_ms);

// WPM → 点长（毫秒）。PARIS 标准：dit = 1200 / wpm。
int32_t cw_morse_dit_ms(int32_t wpm);

// ---------------------------------------------------------------------------
// 收报解码器：把 (按下/松开, 时刻) 序列还原成字符。
// 时间轴由调用方提供（本地单调毫秒即可，不要求与服务端同步）。
// ---------------------------------------------------------------------------
typedef struct {
    void (*emit)(char ch, void *user);   // 解出一个字符（含空格）时回调
    void *user;
    int32_t dit_ms;                      // 点长，判定点划/间隔的基准
    char    buf[CW_MORSE_MAX_CODE];      // 当前字符已收到的元素
    size_t  len;
    int32_t down_ms;                     // 本次按下时刻；<0 = 当前松开
    int32_t up_ms;                       // 最近一次松开时刻
    int32_t last_ms;                     // 最近一次 feed 的时刻，用于 tick 判空
    int     pending_space;               // 出现过单词间隔：下一个字符前先补一个空格
} cw_decoder_t;

void cw_decoder_init(cw_decoder_t *d, int32_t dit_ms, void (*emit)(char, void *), void *user);

// on=1 按下、on=0 松开。now_ms 必须单调不减。
void cw_decoder_feed(cw_decoder_t *d, int on, int32_t now_ms);

// 周期性调用（例如每 100 ms），用于在没有新事件时把悬空的字符/单词收尾。
void cw_decoder_tick(cw_decoder_t *d, int32_t now_ms);

// 丢弃当前半成品字符（切频、进入菜单时调用）。
void cw_decoder_reset(cw_decoder_t *d, int32_t now_ms);
