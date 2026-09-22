// main/cw_morse.c —— 摩斯码编解码与点划判定的纯逻辑实现。
// 与 cw-server 的 MORSE 表保持一致（字母 A-Z、数字 0-9、少量常用标点）。
#include "cw_morse.h"

// 表按字符升序排列，索引即 ch - '!'。'!' = 0x21。
static const char *const MORSE_TABLE[] = {
    /* ! 0x21 */ "-.-.--", /* " 0x22 */ ".-..-.", /* # */ NULL,     /* $ */ NULL,
    /* % */ NULL,          /* & */ ".-...",       /* ' */ ".----.", /* ( */ "-.--.",
    /* ) */ "-.--.-",      /* * */ NULL,          /* + */ ".-.-.",  /* , */ "--..--",
    /* - */ "-....-",      /* . */ ".-.-.-",      /* / */ "-..-.",  /* 0 */ "-----",
    /* 1 */ ".----",       /* 2 */ "..---",       /* 3 */ "...--",  /* 4 */ "....-",
    /* 5 */ ".....",       /* 6 */ "-....",       /* 7 */ "--...",  /* 8 */ "---..",
    /* 9 */ "----.",       /* : */ "---...",      /* ; */ "-.-.-.", /* < */ NULL,
    /* = */ "-...-",       /* > */ NULL,          /* ? */ "..--..", /* @ */ ".--.-.",
    /* A */ ".-",          /* B */ "-...",        /* C */ "-.-.",   /* D */ "-..",
    /* E */ ".",           /* F */ "..-.",        /* G */ "--.",    /* H */ "....",
    /* I */ "..",          /* J */ ".---",        /* K */ "-.-",    /* L */ ".-..",
    /* M */ "--",          /* N */ "-.",          /* O */ "---",    /* P */ ".--.",
    /* Q */ "--.-",        /* R */ ".-.",         /* S */ "...",    /* T */ "-",
    /* U */ "..-",         /* V */ "...-",        /* W */ ".--",    /* X */ "-..-",
    /* Y */ "-.--",        /* Z */ "--..",
};
#define MORSE_TABLE_FIRST '!'
#define MORSE_TABLE_LAST  'Z'

size_t cw_morse_encode(char ch, char out[CW_MORSE_MAX_CODE]) {
    if (!out) return 0;
    out[0] = '\0';
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    if (ch < MORSE_TABLE_FIRST || ch > MORSE_TABLE_LAST) return 0;
    const char *code = MORSE_TABLE[ch - MORSE_TABLE_FIRST];
    if (!code) return 0;

    size_t n = 0;
    while (code[n] && n + 1 < CW_MORSE_MAX_CODE) { out[n] = code[n]; n++; }
    out[n] = '\0';
    return n;
}

char cw_morse_decode(const char *code) {
    if (!code || !code[0]) return '\0';
    for (int i = 0; i <= MORSE_TABLE_LAST - MORSE_TABLE_FIRST; i++) {
        const char *c = MORSE_TABLE[i];
        if (!c) continue;
        size_t n = 0;
        for (; c[n] && code[n]; n++) if (c[n] != code[n]) break;
        if (c[n] == '\0' && code[n] == '\0') return (char)(MORSE_TABLE_FIRST + i);
    }
    return '\0';
}

int32_t cw_morse_dit_ms(int32_t wpm) {
    if (wpm < 5) wpm = 5;
    if (wpm > 40) wpm = 40;
    return 1200 / wpm;
}

char cw_morse_element(int32_t dur_ms, int32_t dit_ms) {
    if (dit_ms <= 0 || dur_ms < 0) return '\0';
    return dur_ms >= dit_ms * 2 ? '-' : '.';
}

// ---------------------------------------------------------------------------
// 解码器
// ---------------------------------------------------------------------------
// 把一个攒够的元素序列翻译成字符。单词间隔不在这里立即输出空格，而是记下来等下一个
// 字符出现时再补 —— 否则"最后一组发完继续静默"会多吐一个尾巴空格。
static void decoder_flush(cw_decoder_t *d) {
    if (d->len == 0) return;
    d->buf[d->len] = '\0';
    char ch = cw_morse_decode(d->buf);
    d->len = 0;
    d->buf[0] = '\0';
    if (!d->emit) return;
    if (d->pending_space) { d->emit(' ', d->user); d->pending_space = 0; }
    d->emit(ch ? ch : '?', d->user);       // 解不出来的码给 '?'，不静默丢
}

// gap = 距上次松开的静默时长：>=3 个点长收字符，>=7 个点长标记单词间隔。
static void decoder_gap(cw_decoder_t *d, int32_t gap) {
    if (gap < 0) return;
    if (gap >= d->dit_ms * 3) decoder_flush(d);
    if (gap >= d->dit_ms * 7) d->pending_space = 1;
}

void cw_decoder_init(cw_decoder_t *d, int32_t dit_ms, void (*emit)(char, void *), void *user) {
    if (!d) return;
    d->emit = emit;
    d->user = user;
    d->dit_ms = dit_ms > 0 ? dit_ms : cw_morse_dit_ms(18);
    d->len = 0;
    d->buf[0] = '\0';
    d->down_ms = -1;
    d->up_ms = -1;
    d->last_ms = 0;
    d->pending_space = 0;
}

void cw_decoder_feed(cw_decoder_t *d, int on, int32_t now_ms) {
    if (!d) return;
    if (now_ms < d->last_ms) now_ms = d->last_ms;      // 时间轴回退就钳住，避免负间隔
    d->last_ms = now_ms;

    if (on) {
        // 静默够久就收前一个字符（7 个点长以上还要记一个单词间隔）
        if (d->up_ms >= 0) decoder_gap(d, now_ms - d->up_ms);
        d->down_ms = now_ms;
        d->up_ms = -1;
        return;
    }

    if (d->down_ms < 0) return;                        // 没有配对的按下，忽略
    char el = cw_morse_element(now_ms - d->down_ms, d->dit_ms);
    d->down_ms = -1;
    d->up_ms = now_ms;
    if (el && d->len + 1 < CW_MORSE_MAX_CODE) {
        d->buf[d->len++] = el;
        d->buf[d->len] = '\0';
    }
}

void cw_decoder_tick(cw_decoder_t *d, int32_t now_ms) {
    if (!d) return;
    if (now_ms < d->last_ms) now_ms = d->last_ms;
    d->last_ms = now_ms;
    if (d->down_ms >= 0 || d->up_ms < 0) return;
    decoder_gap(d, now_ms - d->up_ms);
}

void cw_decoder_reset(cw_decoder_t *d, int32_t now_ms) {
    if (!d) return;
    d->len = 0;
    d->buf[0] = '\0';
    d->down_ms = -1;
    d->up_ms = -1;
    if (now_ms > d->last_ms) d->last_ms = now_ms;
}
