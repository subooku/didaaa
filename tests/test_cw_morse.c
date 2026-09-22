// tests/test_cw_morse.c —— 摩斯码编解码与点划判定的主机测试（不依赖 ESP-IDF/LVGL）。
#include "cw_morse.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_table_roundtrip(void) {
    const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (const char *p = alphabet; *p; p++) {
        char code[CW_MORSE_MAX_CODE];
        size_t n = cw_morse_encode(*p, code);
        assert(n > 0);
        assert(cw_morse_decode(code) == *p);
    }
    char code[CW_MORSE_MAX_CODE];
    assert(cw_morse_encode('?', code) == 6);
    assert(strcmp(code, "..--..") == 0);
    assert(cw_morse_encode('/', code) == 5);
    // 小写自动折叠
    assert(cw_morse_encode('a', code) == 2);
    assert(strcmp(code, ".-") == 0);
    // 未知字符与空码
    assert(cw_morse_encode('~', code) == 0);
    assert(cw_morse_decode("") == '\0');
    assert(cw_morse_decode(".-.-.-.-.-.-.") == '\0');
}

static void test_element_and_dit(void) {
    assert(cw_morse_dit_ms(20) == 60);
    assert(cw_morse_dit_ms(12) == 100);
    assert(cw_morse_dit_ms(0) == cw_morse_dit_ms(5));     // 下界钳位
    assert(cw_morse_dit_ms(999) == cw_morse_dit_ms(40));  // 上界钳位

    int dit = cw_morse_dit_ms(20);                        // 60ms
    assert(cw_morse_element(30, dit) == '.');
    assert(cw_morse_element(119, dit) == '.');
    assert(cw_morse_element(120, dit) == '-');            // 2*dit 归划
    assert(cw_morse_element(180, dit) == '-');
    assert(cw_morse_element(-1, dit) == '\0');
    assert(cw_morse_element(60, 0) == '\0');
}

static char g_out[64];
static int  g_n;
static void collect(char ch, void *user) {
    (void)user;
    if (g_n < (int)sizeof(g_out) - 1) g_out[g_n++] = ch;
    g_out[g_n] = '\0';
}

// 用一个时间轴把一串点划喂进去，验证完整解码。
static void feed(cw_decoder_t *d, const char *code, int dit, int32_t *t) {
    for (const char *c = code; *c; c++) {
        cw_decoder_feed(d, 1, *t);
        *t += (*c == '-') ? dit * 3 : dit;
        cw_decoder_feed(d, 0, *t);
        *t += dit;                                   // 元素间隔 1 点
        if (c[1]) cw_decoder_tick(d, *t);
    }
}

static void test_decoder(void) {
    cw_decoder_t d;
    const int dit = 60;
    int32_t t = 1000;

    g_n = 0; g_out[0] = '\0';
    cw_decoder_init(&d, dit, collect, NULL);
    feed(&d, ".-", dit, &t);                    // A
    t += dit * 3;                               // 字符间隔
    feed(&d, "-", dit, &t);                     // T
    t += dit * 7;                               // 单词间隔
    feed(&d, "...", dit, &t);                   // S
    cw_decoder_tick(&d, t + dit * 10);          // 收尾
    assert(strcmp(g_out, "AT S") == 0);

    // 解不出的码给 '?'，不静默丢
    g_n = 0; g_out[0] = '\0';
    t += dit * 10;
    cw_decoder_init(&d, dit, collect, NULL);
    const char bogus[] = { '.', '.', '.', '.', '.', '.' };
    for (int i = 0; i < (int)sizeof(bogus); i++) {
        cw_decoder_feed(&d, 1, t); t += dit;
        cw_decoder_feed(&d, 0, t); t += dit;
    }
    cw_decoder_tick(&d, t + dit * 10);
    assert(g_n >= 1 && g_out[0] == '?');

    // 时间轴回退不应产生负间隔或崩溃
    cw_decoder_init(&d, dit, collect, NULL);
    cw_decoder_feed(&d, 1, 5000);
    cw_decoder_feed(&d, 0, 4000);
    cw_decoder_tick(&d, 3000);
    cw_decoder_reset(&d, 6000);
}

// 回归守卫：UI 层（cw_radio.c）必须复用同一个解码器，并在元素之间只靠 tick 收尾。
// 曾经的 bug 是每次松开电键都新建解码器、只喂一个元素就立刻收尾，于是任何字符
// 都只能解成 E（点）或 T（划）。下面两条用例把正确用法钉死。
static void test_persistent_decoder(void) {
    cw_decoder_t d;
    const int dit = cw_morse_dit_ms(20);      // 60ms
    int32_t t = 1000;

    // 一个字符的两个元素必须由同一个解码器接力接收，且元素之间 tick 不得提前收尾。
    g_n = 0; g_out[0] = '\0';
    cw_decoder_init(&d, dit, collect, NULL);
    cw_decoder_feed(&d, 1, t);  t += dit;          // 点
    cw_decoder_feed(&d, 0, t);  t += dit;          // 元素间隔 1 点
    cw_decoder_tick(&d, t);                        // 只过了一个点，不该收尾
    assert(g_n == 0);
    cw_decoder_feed(&d, 1, t);  t += dit * 3;      // 划
    cw_decoder_feed(&d, 0, t);
    cw_decoder_tick(&d, t + dit * 3);              // 静默满 3 点，收尾
    assert(strcmp(g_out, "A") == 0);               // 而不是 "ET"

    // 收尾只由 tick 驱动（UI 的 30ms 解码定时器）：连续 tick 直到该收时才收。
    g_n = 0; g_out[0] = '\0';
    t += dit * 10;
    cw_decoder_init(&d, dit, collect, NULL);
    cw_decoder_feed(&d, 1, t);  t += dit;
    cw_decoder_feed(&d, 0, t);  t += dit;
    cw_decoder_feed(&d, 1, t);  t += dit;
    cw_decoder_feed(&d, 0, t);
    for (int32_t k = 30; k < dit * 3; k += 30) cw_decoder_tick(&d, t + k);
    assert(g_n == 0);                              // 静默还差一点，不能提前吐
    cw_decoder_tick(&d, t + dit * 3);
    assert(strcmp(g_out, "I") == 0);
}

int main(void) {
    test_table_roundtrip();
    test_element_and_dit();
    test_decoder();
    test_persistent_decoder();
    printf("test_cw_morse: PASS\n");
    return 0;
}
