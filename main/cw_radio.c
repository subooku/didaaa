// main/cw_radio.c —— CW 网络电台终端：主屏 / 菜单 / 参数页三级界面 + 电键 + 侧音合成。
//
// 硬件前提（来自 components/bsp/include/bsp_pins.h，不要靠猜）：
//   ESP32-C3 单核 160MHz、无 PSRAM；ST7789P3 240x320 无触控；ES8311 全双工 I2S；
//   三个按键共用一个 ADC 引脚，靠各自的分压电阻区分（UP=0Ω、DOWN=1k、OK=2.2k）。
//
// ★ 按键重排（在 main.c 的 on_key 里做一次）：物理 UP → 逻辑 OK，物理 DOWN → 逻辑
//   UP，物理 OK → 逻辑 DOWN。即新布局是"OK = 原来的 UP，UP = 原来的 DOWN，
//   DOWN = 原来的 OK"，活（电键 + 长按 900ms 进菜单）跟着新名字走：按物理 UP 键
//   做的是 OK 键的事。本文件看到的已经是重排之后的键。
//
// ★ 组合键：三个键共用一路 ADC，物理 UP 是 0Ω 直连 GND，跟任何键同按都还是 0mV ——
//   凡是涉及物理 UP 的组合都无从检测。唯一能区分的是物理【DOWN+OK】（并联约
//   212mV，已在 bsp_pins.h 里单列为第 4 档电压）；重排后它正好是逻辑 UP+DOWN，
//   用作在线/离线开关。
//   其余语义靠「按下/抬起两个边沿 + OK 长按 900ms = 进/出菜单」表达。
#include "cw_arrow_bmp.h"    // 频谱下方的频道位置箭头（7x14 预渲染位图）
#include "cw_radio.h"
#include "cw_morse.h"
#include "cw_net.h"
#include "cw_ota.h"
#include "cw_proto.h"
#include "cw_prov.h"

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_pins.h"       // BSP_BTN_COUNT：按键状态数组要按这个数开，含组合键那一档

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "cw_radio";

#define SCR_W 240
#define SCR_H 320

// ★ 盖板玻璃四角是圆的，贴着角排的内容会被切掉 —— 顶行的 "CW 40M" 和电量就是
//   这么缺一块的。左右各留 SAFE 作为安全边距（中间几行本来就在圆角之外，
//   跟着一起对齐只是为了让版面齐整），内容宽度统一用 CONTENT_W。
//   14 这个数按圆角半径 28 估算：y=4 处被切掉的水平宽度约 13.6px，刚好够。
#define SAFE       14
#define CONTENT_W  (SCR_W - 2 * SAFE)
// 盖板玻璃四角的圆角半径。在线/离线那一圈状态框要贴着玻璃走，就得用它做圆角
// （见 build_main 末尾），否则方框的直角会戳在圆角外面显得很突兀。
#define SCR_CORNER_R 28

// 频率边界与服务端保持一致（cw-server/server.js 的 F_MIN / F_MAX）。
#define F_MIN 7000000u
#define F_MAX 7200000u

// OK 键按住到这个时长，不等松手就直接进/出菜单（由 tick 判定，见 tick_cb）。
// 2 秒：主屏上 OK 同时是电键，5 WPM 的划也只有 720ms，2 秒远在最长的划之外，
// 正常发报碰不到；代价是进菜单要按住等两秒，换来的是绝不会发着报就跳进菜单。
#define MENU_HOLD_MS 2000
// UP/DOWN 按住多久后进入连发（此前先响应一次单步，保证手感即时）。
#define REPEAT_DELAY_MS 400
#define REPEAT_PERIOD_MS 110

// 菜单列表的几何：以前 10 项硬塞进一屏（行高 24、行距 26）挤得字都贴边，
// 现在放宽到行高 38、行距 44，一屏只放得下 5 行半，超出的靠列表上下滚动。
// UP/DOWN 选中到边界外时列表跟着挪，选中行始终留一行余量、不贴着边。
#define MENU_ROW_H   38
#define MENU_PITCH   44
#define MENU_LIST_Y  36
#define MENU_VIEW_H  250   // 36 + 250 = 286，底部署名从 292 起
#define MENU_SCROLL_ANIM_MS 180

#define SPEC_BARS 40
// 频谱的横向排布：起点让开左边框 6px，柱宽 3、柱距 5（最后一根收在右边框内侧）。
// 频道位置箭头按同一套坐标算 x，改这里两边会一起跟着变。
#define SPEC_X0   7
#define SPEC_SPAN 226
#define SPEC_BAR_W 3
#define SPEC_X(i) (SPEC_X0 + ((i) * (SPEC_SPAN - SPEC_BAR_W)) / (SPEC_BARS - 1))
// 柱子底边固定在 y=96（h_max=30 从 y=66 往下长），箭头就贴在底边下面朝上指着。
#define SPEC_BASE_Y 96

// 服务端占用表一格 = 2 kHz（server.js 的 BIN）。窗口缩到比它更窄时，柱子就只能成块，
// 所以放大以后真正有分辨率的是 roster 里的精确台站频率（见 draw_ticks）。
#define SPEC_BIN_HZ 2000u

#define ROSTER_MAX 8
#define STATION_ROWS 3
#define TXBUF_MAX 16

// 8 kHz 单声道已经够 700 Hz 的侧音了；chunk 取 64 样点 = 8 ms。
// 原来是 320 样点（40 ms）—— 一记点才 66 ms，40 ms 的粒度等于把节奏重新量化了一遍，
// 连着敲几个点就会糊成一记长划。8 ms 既能跟上手，又不至于把写音频的开销翻上去。
#define AUDIO_HZ    8000
#define AUDIO_CHUNK 64

// ---- 并发播放的诊断量（爆音排查用）----
// 爆炸音的直接原因几乎总是"PCM 供给被掐断"：DMA 里的存货放完了，后面接上来的
// 波形和断掉那一下对不上，喇叭就"啪"一声。所以这里把三条路径的耗时都量出来，
// 每 AUDIO_STAT_MS 报一次，改音效/音量之前先看这组数字（见 refresh_main 下方说明）：
//   · s_pcm_gap_ms_max —— PCM 供给的最大间隔：上一次写完到这一次开始写之间的空档。
//     稳态等于一块的时长（8 ms）；明显超过常备存货（24 ms）就说明 DMA 被放空过。
//   · s_pcm_underrun   —— 存货被放空的次数。**这一项就该恒为 0**：每记一次，
//     DMA 就把缓冲清零接着播 0，再接上来的波形幅度和 0 对不上，就是一声爆响。
//     它是"有没有杂音"的直接证据，比"日志里有没有告警"可信得多。
//   · s_draw_ms_max    —— 一次界面重绘的耗时（LVGL 任务里跑）。
//   · s_save_ms_max    —— 一次 NVS 落盘的耗时；Flash 写入会关 cache，这段时间内
//     所有跑在 Flash 上的代码都停摆，音频任务也喂不上数据。
#define AUDIO_STAT_MS 10000

// 侧音的时长约束：再短的点也要响够 TONE_MIN_MS 才听得见；
// 超过 TONE_MAX_MS 还在响，一定是状态卡了（抬起事件丢了），强制闭音。
// 5 WPM 的划也只有 720 ms，3000 ms 这个上限不会误伤正常发报。
#define TONE_MIN_MS 12
#define TONE_MAX_MS 3000
// 收报的兜底：对方 keydown 之后如果 keyup 帧丢了（UDP 不保证送达），
// 本地会一直响下去。超过这个时间没有新的键控帧就当对方已经松手。
#define RX_TONE_MAX_MS 2000

typedef enum { SCR_MAIN = 0, SCR_MENU, SCR_ADJ } scr_t;

typedef enum { ADJ_TONE = 0, ADJ_WPM, ADJ_VOL, ADJ_STEP, ADJ_WIFI, ADJ_NET, ADJ_CALL, 
               ADJ_BL, ADJ_BL_TO, ADJ_EXIT, ADJ_RESET, ADJ_FW, ADJ_ABOUT,
               ADJ_COUNT } adj_t;

typedef struct {
    uint32_t freq;
    uint32_t step;
    int      tone;      // 侧音音调 300..1200 Hz
    int      vol;       // 0..100
    int      wpm;       // 发报速度
    int      bl;        // 背光亮度 10..100 %（下限 10 是怕调没了找不回来）
    int      bl_to;     // 背光超时秒数，取 BL_TO_OPTS 里的值；0 = 常亮不熄
} settings_t;

// VFO 步进的可选档位，同样按菜单里的循环顺序排。
static const uint32_t STEP_OPTS[] = { 10u, 100u, 1000u, 10000u };
#define STEP_N ((int)(sizeof(STEP_OPTS) / sizeof(STEP_OPTS[0])))

// 背光超时的可选档位，顺序就是菜单里 UP/DOWN 的循环顺序。
// 0 表示常亮（永不熄屏）—— 收报时不希望屏幕灭掉就选它。
static const int BL_TO_OPTS[] = { 10, 30, 60, 0 };
#define BL_TO_N ((int)(sizeof(BL_TO_OPTS) / sizeof(BL_TO_OPTS[0])))

// 把秒数翻成菜单上显示的字样。
static const char *bl_to_text(int sec) {
    if (sec == 0) return "ON";
    static char b[8];
    snprintf(b, sizeof(b), "%d s", sec);
    return b;
}

typedef struct { char call[9]; uint32_t freq; uint16_t uid; } roster_t;

// 这一条是不是我自己？优先比 uid —— 呼号靠不住：服务端撞名时会把它改成
// "GH1BHU2" 这种（uniqueCall 加数字后缀），那时按呼号比对就会把自己当成别人，
// 显示在自己的名单和瀑布图里。uid 是服务端分配的，改名也不会变。
static bool roster_is_me(const roster_t *r) {
    uint16_t me = cw_net_uid();
    if (me && r->uid == me) return true;
    // 老服务端不下发 uid 时的兜底：名单里列的都是服务端下发的虚拟呼号，
    // 所以拿它比对（不是 cw_net_call —— 那可能是用户填的 Ham，名单里没有）。
    const char *c = cw_net_vcall();
    if (!c[0]) c = cw_net_call();
    return c[0] && strncmp(r->call, c, sizeof(r->call)) == 0;
}

// ★ 出厂默认值。恢复出厂设置后走的就是这一组（NVS 清空 → settings_load 什么都读不到
//   → 保留这里的初值）：侧音 700 Hz、速度 18 WPM、音量 50%、步进 100 Hz、
//   亮度 50%（CW_BL_DEFAULT）、背光超时 30 秒（CW_BL_TO_DEFAULT）、离线开机。
//   基站在出厂状态下是 "None"、Wi-Fi 凭据为空 —— 那两项不在这里，见 cw_prov。
static settings_t S = { .freq = 7024200u, .step = 100u, .tone = 700, .vol = 50,
                        .wpm = 18, .bl = CW_BL_DEFAULT,
                        .bl_to = CW_BL_TO_DEFAULT };
static settings_t S_bak;

// 并发播放的诊断量：PCM 供给间隔 / 重绘耗时 / 落盘耗时，每 AUDIO_STAT_MS 上报一次。
// 音频任务写、LVGL 任务读，32 位对齐访问在 C3 上天然原子。
static volatile uint32_t s_pcm_gap_ms_max;      // PCM 最大供给间隔（毫秒）
static volatile uint32_t s_pcm_underrun;        // 存货被放空的次数 —— 每记一次就是一声爆响
static volatile uint32_t s_pcm_write_fail;      // 写音频失败次数（正常应为 0）
static volatile uint32_t s_write_ms_max;        // 单次 write 阻塞的最长时间
static volatile uint32_t s_sleep_ms_max;        // 单次主动让出 CPU 的最长时间
static volatile uint32_t s_blocks;              // 统计周期内写了多少块（稳态应 ≈ 1250 块/10s）
static uint32_t s_draw_ms_max;                  // 一次界面重绘的最大耗时
static uint32_t s_save_ms_max;                  // 一次 NVS 落盘的最大耗时
static uint32_t s_stat_ms;                      // 上次上报诊断量的时刻（毫秒）

// DMA 里常备的 PCM 排队量（毫秒）。见 audio_task 里的说明：这是"抗停顿的余量"，
// 同时也是"按键到出声"的延迟 —— 两者是同一个数，只能折中。
static volatile uint32_t s_backlog_ms;
// 包络还没收干净（env > 0.001）。落盘要连这个一起躲开：逻辑上已经松键，
// 但波形还在往下走的时候硬切到 0，照样是"啪"一声。
static volatile bool     s_audio_audible;

// ===========================================================================
// 设置落盘（NVS）：菜单里改过的每一项，重启后都要照旧
// ===========================================================================
// 为什么频率必须留：每次开机都回到默认的 7.024.200，等于每次上机都得重新找台，
// 上一次通联的位置也丢了。
//
// 写时机分两种：
//   · 菜单里的数值项（侧音/速度/音量/步进/亮度/超时）按 OK 确认时请求一次落盘，
//     一次操作一写，量很小，不值得为它做防抖；
//   · 频率是按住连发的，一次调谐能改几十下，逐下 commit 是白白磨损 flash。
//     改成"改完静置 SETS_IDLE_MS 再落盘"（tick 里兜），并在任何 esp_restart 之前
//     强制补写一次 —— 用户可能刚转完频率就选 REBOOT。
//
// ★ 落盘一律不在调用方当场写：写 Flash 会关 cache，那段时间连音频任务都跑不动，
//   DMA 放空就是一声爆音。所以这里只置标记、通知落盘任务，由它在"安全的播放
//   边界"（没有侧音、也没有收报音的时候）真正下笔。只有 esp_restart 之前那一次
//   是例外 —— 那种时候没有音频需要顾及，必须当场写完。
// ★ 落盘一次实测要几百毫秒（NVS 扇区回收），比 DMA 缓冲能撑的时间还长，
//   所以"等不出声再写"比"写完快点"更重要：静音期间 DMA 放空也听不见。
#define SETS_IDLE_MS 3000       // 手停下来多久算"改完了"（防抖，避免逐下写 flash）
#define SAVE_WAIT_MS 2000       // 等安全播放边界的最长时间，等不到也照写
static const char *const SETS_KEYS[] = { "freq", "step", "tone", "vol", "wpm" };
#define SETS_KEY_N ((int)(sizeof(SETS_KEYS) / sizeof(SETS_KEYS[0])))
static bool    s_sets_dirty;
static int64_t s_sets_dirty_ms;
static TaskHandle_t s_save_task;
// 上次真正写进 NVS 的那一份。跟当前值一字不差就不写 —— 进菜单看一眼再按 OK
// 不该白白擦写一次 flash（"只在有意义的状态变化时保存"）。
static settings_t S_saved;
static bool       S_saved_valid;

static void settings_mark_dirty(void) {
    s_sets_dirty = true;
    s_sets_dirty_ms = esp_timer_get_time() / 1000;
}

// 真正下笔。只允许落盘任务和"重启前的同步补写"调用。
static void settings_write(void) {
    if (S_saved_valid &&
        S_saved.freq == S.freq && S_saved.step == S.step && S_saved.tone == S.tone &&
        S_saved.vol  == S.vol  && S_saved.wpm  == S.wpm  && S_saved.bl   == S.bl &&
        S_saved.bl_to == S.bl_to) {
        s_sets_dirty = false;                 // 没变过，白记一笔脏标记而已
        return;
    }
    uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t vals[SETS_KEY_N] = { S.freq, S.step, (uint32_t)S.tone,
                                  (uint32_t)S.vol, (uint32_t)S.wpm };
    // 五项 u32 + 亮度/超时两档，一次会话一次 commit：分三次写会做三次扇区回收，
    // 实测落盘从几百毫秒降到几十毫秒（见 cw_prov_save_settings 的注释）。
    cw_prov_save_settings(SETS_KEYS, vals, SETS_KEY_N, (uint8_t)S.bl, (uint8_t)S.bl_to);
    uint32_t dt = (uint32_t)(esp_timer_get_time() / 1000) - t0;
    if (dt > s_save_ms_max) s_save_ms_max = dt;
    S_saved = S;
    S_saved_valid = true;
    s_sets_dirty = false;
    ESP_LOGI(TAG, "设置已保存(%u ms): %u.%03u.%03u · 步进 %u · 侧音 %d · 音量 %d · %d WPM",
             (unsigned)dt,
             (unsigned)(S.freq / 1000000u), (unsigned)((S.freq / 1000u) % 1000u),
             (unsigned)(S.freq % 1000u), (unsigned)S.step, S.tone, S.vol, S.wpm);
}

// 请求落盘：只置标记 + 通知，调用方立刻返回。按键回调和 LVGL 锁里只准调这个。
static void settings_request(void) {
    if (!s_sets_dirty) return;
    if (s_save_task) xTaskNotifyGive(s_save_task);
}

// 有未落盘的设置就当场写完。只在 esp_restart / 退出电台前调用。
static void settings_flush(void) { if (s_sets_dirty) settings_write(); }

// 落盘任务本体定义在音频任务之后（要用 ms32() 和 audio_sounding()），见 save_task。
static void save_task(void *arg);

// 读回来时逐项校验：NVS 里可能躺着旧固件写的值、或手工改坏的值。
// 任何一项越界就当没存过，用默认值 —— 带上一个非法频率去连服务器，
// 后果比"回到默认频点"糟糕得多。
static void settings_load(void) {
    uint32_t v = 0;
    if (cw_prov_load_u32("freq", &v) && v >= F_MIN && v <= F_MAX) S.freq = v;
    if (cw_prov_load_u32("step", &v)) {
        for (int i = 0; i < STEP_N; i++) if (STEP_OPTS[i] == v) { S.step = v; break; }
    }
    if (cw_prov_load_u32("tone", &v) && v >= 300 && v <= 1200) S.tone = (int)v;
    if (cw_prov_load_u32("vol",  &v) && v <= 100)              S.vol  = (int)v;
    if (cw_prov_load_u32("wpm",  &v) && v >= 10 && v <= 30)    S.wpm  = (int)v;
}

static scr_t      s_scr = SCR_MAIN;
static int        s_sel;                 // 菜单选中项
// WI-FI SETUP 确认页的焦点：0 = 左键"确认"，1 = 右键"取消"。
// 默认必须落在取消上 —— 否则在菜单里手滑一下就把机器重启进配网了。
static int        s_wifi_focus = 1;
// BASE STATION 那一页的焦点：0 = 左键"更改"，1 = 右键"确认"。
// 默认落在确认上：进来多半只是看一眼地址，不需要动它。
static int        s_srv_focus = 1;
// FACTORY RESET 确认页的焦点：0 = 左键"复位"，1 = 右键"取消"。
// 跟 WI-FI SETUP 一样默认落在取消上 —— 这一下会把 Wi-Fi、呼号、所有设置全清掉，
// 手滑一下代价太大。
static int        s_reset_focus = 1;

// CALLSIGN 页的焦点：0 = 左键"更新"，1 = 右键"取消"。默认落在取消上 ——
// 选中更新要整机重启进配网，跟 WI-FI SETUP 一样不该手滑就中。
static int        s_call_focus = 1;
// FIRMWARE（OTA）页的焦点：0 = 左键"升级"，1 = 右键"取消"，默认落在取消上。
// 进页就自动查一次服务器版本，查到新版才摆出这两颗按钮。
static int        s_fw_focus = 1;
static bool       s_fw_query_sent;    // 本轮是否已发起查询（避免每 100ms 重发）
static bool       s_fw_upgrading;     // busy 的是"下载"而非"查版本"（决定进度条要不要画）
static uint32_t   s_fw_done_ms;       // 下载完成后开始计时，2 秒后重启
static bool       s_running;

// 运行时状态
// volatile：音频任务、按键路径和落盘任务三边都要看它（落盘靠它挑安全的下笔时机）。
static volatile bool s_tx_on;
static int64_t   s_tx_down_ms;
// 侧音的"计划熄音时刻"：0 = 键还按着，非 0 = 已经松手、到这个点就闭音。
//
// ★ 为什么声音要另记一套时刻，而不是跟着 s_tx_on 走：
//   按键事件要经过「esp_timer 回调 → 输入队列 → 输入任务 → 电台队列 → 抢 LVGL 锁 →
//   派发」五道手，抢锁那一下最多要等 200 ms。用"派发到哪一步就响到哪一步"，
//   一等就把 66 ms 的点拖成 200 ms 的划，连着敲几个点更是直接糊成一记长划 ——
//   这就是"按快了声音连成一条"的根因。
//   现在起落时刻在按键回调里就落下来（见 cw_radio_key_edge），音频任务照墙上时钟
//   开关音，派发链路再怎么堵也只影响屏幕上的字符和发到网上的帧（那两路本来就带
//   时间戳），声音不受影响。
// 时刻统一取毫秒的低 32 位：C3 是 32 位核，64 位读写不是原子的，音频任务和按键回调
// 一个读一个写会读到撕裂的值。32 位对齐访问天然原子，回绕用带符号差处理。
// 0 当作"键还按着"的哨兵 —— 开机那一刻不在发报，撞不上。
static volatile uint32_t s_tx_off_ms;
static volatile bool s_rx_on;
// 最近一次收到键控帧的时刻，用于收报长音的兜底（见 tick_cb）。
static volatile uint32_t s_rx_key_ms;

static inline uint32_t ms32(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static volatile int s_rx_pitch;
static volatile int s_rx_s;
static char      s_txbuf[TXBUF_MAX + 1];
static char      s_rxbuf[TXBUF_MAX + 1];
static roster_t  s_roster[ROSTER_MAX];
static int       s_roster_n;
static uint8_t   s_bins[100];
static int       s_bins_n;
// 背光超时。s_bl_dim 与 S.bl 分开记：熄灭期间 S.bl 不变，唤醒时原样恢复。
static int64_t   s_bl_last_ms;
static bool      s_bl_dim;
// 熄屏期间按下的那一次，抬起也要一起吞掉（s_wake_only）：点亮归点亮，
// 不该顺带执行"进菜单 / 改参数 / 发报"这些动作。
static bool      s_wake_only;
// 长按 OK 已经自动进（出）过菜单，松手那一下不再执行短按动作。
static bool      s_ok_hold_done;

// 记一次"有活动"。可在任何上下文调用（只写一个 int64，不碰 LVGL、不碰 LEDC）。
static void bl_note(void) { s_bl_last_ms = esp_timer_get_time() / 1000; }

// 有信号 = 能听见别人在发报（s_rx_on 是"这一刻在键控"，s_rx_pitch 是收听音调；
// 字符间隔里 on 会短暂停，但音调还在，两者取或才不会一停就熄）。
static bool rx_active(void) { return s_rx_on || s_rx_pitch > 0; }

// 熄灭/唤醒都在这里收口，由 100ms 的 tick 驱动，避免网络任务直接操作 PWM。
static void bl_tick(int64_t now) {
    // 0 = 常亮：这一档永不熄灭，只有亮度调节和手动重启能改变它。
    // 有人在呼叫时也当作"常亮"：屏幕灭着却正好来个 CQ，整个 QSO 就漏掉了，
    // 那时宁可多耗一点电。信号一走，计时接着往下算。
    bool expired = S.bl_to > 0 && !rx_active() &&
                   (now - s_bl_last_ms) > (int64_t)S.bl_to * 1000;
    if (s_bl_dim && !expired) {
        s_bl_dim = false;
        bsp_display_backlight((uint8_t)S.bl);
    } else if (!s_bl_dim && expired) {
        s_bl_dim = true;
        bsp_display_backlight(0);
    }
}

// 按键唤醒：立刻点亮，不等下一次 tick —— 100ms 的延迟在"按了没反应"的观感上很明显。
static void bl_wake(void) {
    bl_note();
    if (s_bl_dim) { s_bl_dim = false; bsp_display_backlight((uint8_t)S.bl); }
}

static cw_net_state_t s_net_state = CW_NET_IDLE;
static int64_t   s_unix_ms;
static char      s_chat[64];

// 按键连发
static int      s_repeat_btn = -1;
static int64_t  s_repeat_start;
static int64_t  s_press_ms[BSP_BTN_COUNT];
// 每个键是否处于"按下且已派发"状态。抬起必须配对一次按下才作数：
// 按下事件一旦丢失（队满被挤、抢 LVGL 锁超时），s_press_ms 还是 0，
// 拿它算时长会得到"开机至今"，一下就被误判成长按 900ms 而跳进菜单。
static bool     s_down[BSP_BTN_COUNT];

// 在线/离线。由 UP+DOWN 两键同按（物理上是【下+确定】，重排后即逻辑 UP+DOWN）
// 切换：离线时不发也不收，并让服务端把我从别人的名单里摘掉。
// 屏幕最下边一条整宽色带表示当前状态（绿=在线，灰=离线）。
// 开机默认在线，不存 NVS —— 重启就是重新上机的意思。
// 出厂默认离线：开机不进别人的名单、也不往外发键控，按【UP+DOWN】才上线
// （屏幕外圈随之变绿）。在线/离线只是本次上电里的临时状态，不落盘 —— 每次开机
// 都从离线开始，"上电先听着"比"上电就挂在网上"更符合用机习惯。
static bool     s_online = false;

// 组合键状态机。一路 ADC 只能报一个键，两键同按时报的是第 4 档电压（COMBO），
// 于是事件序列是"单键按下 → COMBO 按下 → COMBO 抬起 → 单键抬起"这种夹心结构；
// 中间补发的那两个单键事件必须吃掉，否则会变成一次发报或一次跳频。
static bool     s_cmb;                       // 组合已成立（还没全松开）
static bool     s_cmb_down[BSP_BTN_COUNT];   // 组合涉及的三个索引各自是否按住
static int64_t  s_cmb_ms;                    // 最近一次组合相关事件，用于卡死兜底
// 撤销现场：上一次"单键按下"之前的状态。组合成立时要把第一颗键已经做过的动作退
// 回去 —— 否则每次切在线/离线都会顺手发一个点、或者把频率顶偏一格。
static settings_t s_pre_S;
static int        s_pre_sel;
static bool       s_pre_tx;
static int64_t    s_pre_ms;


// ★ 按键回调跑在 button 组件的 esp_timer 任务里，那里既不能碰 LVGL 也不能阻塞
//   （esp_timer 是所有定时回调的公共载体，拖住它整个系统会卡）。所以回调只把事件
//   丢进队列，由 s_key_task 在拿到 LVGL 锁之后做真正的处理。
typedef struct {
    uint8_t  btn;
    uint8_t  ev;
    int64_t  t_ms;        // 边沿发生的真实时刻：在回调里就取好，队列延迟不影响点划判定
} key_ev_t;
static QueueHandle_t s_keyq;
static TaskHandle_t  s_key_task;
static volatile bool s_key_run;

// ---- LVGL 对象 ----
static lv_obj_t *s_main, *s_menu, *s_adj;
static lv_obj_t *s_lbl_call;
static lv_obj_t *s_lbl_freq, *s_lbl_pitch, *s_bar_s, *s_lbl_rx, *s_lbl_tx, *s_lbl_net, *s_lbl_bat;
static lv_obj_t *s_ticks[ROSTER_MAX];    // 台站竖线：roster 的精确频率，最多 8 条
static lv_obj_t *s_lbl_span;             // 当前频谱窗口宽度（跟着 VFO 步进变）
static lv_obj_t *s_bars[SPEC_BARS];
// 频谱下方的频道位置箭头：朝上指着自己在 40m 波段里的位置。
// 以前是把"自己所在那一格"的柱子染成琥珀，没信号时那根柱子只有 1px 高，
// 看上去就是个黄色小点，既不起眼也说不清方向。
static lv_obj_t *s_marker;
static const lv_image_dsc_t s_arrow_dsc = {
    .header = { LV_IMAGE_HEADER_MAGIC, LV_COLOR_FORMAT_RGB565, 0,
                CW_ARROW_W, CW_ARROW_H, CW_ARROW_W * 2, 0 },
    .data_size = CW_ARROW_BYTES,
    .data = cw_arrow_bmp,
};
static lv_obj_t *s_lbl_st[STATION_ROWS];
// 屏幕最外圈那一圈边框就是在线/离线指示（绿/灰），见 refresh_online。
static lv_obj_t *s_bar_online;
// 顶行 Wi-Fi 图标的三根竖条（矮→高）。没有信号塔字形，用矩形拼。
static lv_obj_t *s_wifi_bars[3];
static lv_obj_t *s_rows[ADJ_COUNT];
// 菜单行的滚动容器：一屏放不下 10 行，行挂在这个容器里，靠改 scroll_y 上下滚。
static lv_obj_t *s_menu_list;
// 自己记一份"目标滚动位置"。lv_obj_get_scroll_y 读的是动画中间值，
// 连续按 UP/DOWN 时会读到半路上的数，据此算下一步就会抖。
static int s_menu_scroll;
static lv_obj_t *s_adj_title, *s_adj_value, *s_adj_bar, *s_adj_note;
// Wi-Fi 确认页的左右两个按钮：[0] 确认（左） [1] 取消（右）。平时隐藏。
static lv_obj_t *s_adj_btn[2], *s_adj_btn_lbl[2];
// 列表式选项页（STEP / BL TIMEOUT）：把全部档位摊开成几行，选中行填成实心琥珀。
// 上限取 4：STEP 和 BL TIMEOUT 都正好 4 档，再多屏幕也放不下。平时隐藏。
#define ADJ_LIST_MAX 4
static lv_obj_t *s_adj_list[ADJ_LIST_MAX], *s_adj_list_lbl[ADJ_LIST_MAX];
// ---- ABOUT ME 页 ----
// 全英文：Montserrat 里没有中文字形，以前那版是预渲染的位图，改一行字要重新生成
// 头文件（233 KB 塞在 flash 里）。现在用文字标签，改起来只是改字符串。
// 两段：DEVICE（设备信息）+ AUTHOR（作者信息），每段一个琥珀小标题 + 若干"键/值"行。
// 键左对齐、值右对齐，用两个标签而不是拼一行字符串 —— 比例字体下拼空格对不齐。
#define ABOUT_ROWS 7            // 3 行设备信息 + 4 行作者信息（最后一行占两行高）
#define ABOUT_KEY_W 76          // "Device ID" 是 9 个字符，76px 足够
#define ABOUT_VAL_X (ABOUT_KEY_W + 4)
#define ABOUT_VAL_W (CONTENT_W - ABOUT_VAL_X)
static lv_obj_t *s_about;                        // 容器（透明，只用来整块显隐）
static lv_obj_t *s_about_hdr[2];                 // DEVICE / AUTHOR 两个小标题
static lv_obj_t *s_about_key[ABOUT_ROWS];
static lv_obj_t *s_about_val[ABOUT_ROWS];

// ---- WI-FI SETUP 页的网络实况：IP / MAC / 连接状态 ----
// 进配网要重启，代价不小；先把"现在连的是什么"摆出来，用户才知道要不要重配。
#define NETINFO_ROWS 3
// 60 是给 CALLSIGN 页的 "VIRTUAL" 留的（7 个字符 × 约 8px）。
// WI-FI SETUP 页的 "IP" / "MAC" / "Link" 跟着一起挪，取值列仍有 148px，够放 MAC。
#define NETINFO_KEY_W 60
static lv_obj_t *s_netinfo_key[NETINFO_ROWS];
static lv_obj_t *s_netinfo_val[NETINFO_ROWS];

static lv_timer_t *s_tick, *s_repeat, *s_dec_tick;

static cw_decoder_t s_dec;      // 收报解码器（解网络上收到的码）
static cw_decoder_t s_txdec;    // 发报解码器（本地回显，解自己手敲的码）
static int          s_dec_wpm;
static TaskHandle_t s_audio_task;
static volatile bool s_audio_run;

// ===========================================================================
// 工具
// ===========================================================================
static void fmt_freq(char *out, size_t n, uint32_t f) {
    snprintf(out, n, "%u.%03u.%03u", (unsigned)(f / 1000000u),
             (unsigned)((f / 1000u) % 1000u), (unsigned)(f % 1000u));
}

// 频率的 MHz 短写法（4 位小数 = 100 Hz 分辨率）：7.024.200 → "7.0242"。
// 台站名单一行要同时塞下呼号、频率、收听音调、S 值，用短写法省出来的宽度刚好够。
// 取百赫兹那一位再对 10000 取模，直接拼 4 位小数：7024200/100 = 70242 → %10000 = 0242。
static void fmt_freq_mhz(char *out, size_t n, uint32_t f) {
    snprintf(out, n, "%u.%04u", (unsigned)(f / 1000000u), (unsigned)((f / 100u) % 10000u));
}

// 零拍(zero beat)：接收机内部有 700 Hz 的 CW 偏移，所以两台**发射同频**时，
// 双方听到的音调正好等于自己的侧音 —— 这就是"零拍"。
//   听到的音调 = 700 + (我的频率 − 对方频率)
// 40m(7 MHz) 用 LSB：对方在你上方（频率更高）时音调偏低；你调高，他的音调就走低。
// 偏离超过 ±450 Hz 掉出 CW 通带（250–1150），跟真机一样失联。
// 必须与服务端 server.js 的 CW_OFFSET / AUD_MIN / AUD_MAX 保持一致，
// 否则离线名单和在线名单会算出两套数。
#define CW_OFFSET_HZ   700
#define AUD_MIN_HZ     250
#define AUD_MAX_HZ     1150
static int audible_pitch(uint32_t me, uint32_t other) {
    long p = CW_OFFSET_HZ + ((long)me - (long)other);
    if (p < AUD_MIN_HZ || p > AUD_MAX_HZ) return 0;
    return (int)p;
}

// ★ 同频判定：两台设备的频率差不超过 CW_CHANNEL_TOL_HZ 才算"在同一个频道上"。
//   与 cw_net.c 的 CW_CHANNEL_TOL_HZ、服务端 server.js 的 CHANNEL_TOL 三处必须一致，
//   否则会出现"服务端转发了、设备端却把它丢了"这种半边生效的怪事。
//   真机上这就是"对方的信号落在我接收机通带里"：差出这一格就是两个频道，
//   跟隔壁频道有人在说话一个道理 —— 听不见，也不该出现在收报区。
#define CW_CHAN_TOL_HZ 100
static bool same_channel(uint32_t a, uint32_t b) {
    long d = (long)a - (long)b;
    if (d < 0) d = -d;
    return d <= CW_CHAN_TOL_HZ;
}
static int s_of_pitch(int pitch) {
    int s = 9 - (int)((abs(pitch - CW_OFFSET_HZ) + 45) / 90);
    return s < 0 ? 0 : (s > 9 ? 9 : s);
}

static void buf_push(char *buf, size_t cap, char ch) {
    size_t n = strlen(buf);
    if (n + 1 >= cap) {
        memmove(buf, buf + 1, n - 1);
        n = cap - 2;
        buf[n] = '\0';
    }
    buf[strlen(buf)] = ch;
}

// 电键的三个动作。时刻一律用按键回调打的时间戳，不是"此刻"。
// 可从按键回调（esp_timer 任务）或派发任务调用：只写几个变量，不碰 LVGL。
//
// ★ 同一颗键的按下会到这里两次：快车道（按键回调）一次、派发（过了队列和 LVGL 锁）
//   一次。派发那次只用它"补漏"，绝不能把快车道已经排好的节奏抹掉 —— 否则声东击西，
//   声音又退回跟着派发节奏走的老毛病。判据：只有"确实更晚的一次按下"才允许重排；
//   与上一记按下时刻相同或更早的，以及落在还没到点的熄音之前的，都是在补漏。
static void tx_down(int64_t t_ms) {
    if (s_tx_on && s_tx_off_ms != 0 && t_ms <= (int64_t)(int32_t)s_tx_off_ms) return;
    if (s_tx_on && t_ms <= s_tx_down_ms) return;
    s_tx_on = true;
    s_tx_off_ms = 0;
    s_tx_down_ms = t_ms;
}
// 松手：不是立刻闭音，而是把熄音时刻排进去。派发迟到了也没关系 ——
// 时刻是当时记下的，音频任务照着时钟走，不会把点拖成划。
static void tx_up(int64_t t_ms) {
    if (!s_tx_on || s_tx_off_ms != 0) return;       // 排过熄音就别再改晚
    // 手抖出来的极短按下也要有一声响，否则听着像漏了码。
    int64_t off = (t_ms - s_tx_down_ms < TONE_MIN_MS) ? s_tx_down_ms + TONE_MIN_MS : t_ms;
    s_tx_off_ms = (uint32_t)off;
}
// 强制闭音（这一下不算发报：进菜单、切在线离线、撤销组合键）。
static void tx_stop(void) { s_tx_on = false; s_tx_off_ms = 0; }

// 音频任务眼中的"电键正在响"：键还按着，或已松手但还没到熄音时刻。
// 到点就在这里收尾，别让 s_tx_on 一直挂到下一次派发。
static bool tx_sounding(uint32_t now, int *hz_out) {
    if (!s_tx_on) return false;
    if (s_tx_off_ms == 0 || (int32_t)(now - s_tx_off_ms) < 0) { *hz_out = S.tone; return true; }
    s_tx_on = false;
    s_tx_off_ms = 0;
    return false;
}

// ===========================================================================
// 音频：侧音合成。发报时发本地音调，收报时发服务端算好的收听音调
// （零拍时它正好等于侧音，耳朵听上去是同一个音）。
// ===========================================================================
// ★ 这里一旦供不上 PCM，喇叭就会"啪"一声 —— 这就是用户说的爆炸音。
//
// 【为什么必须一直写，连静音也写 0】
//   I2S 的 DMA 是个环，存货放完之后 GDMA 会把缓冲清零接着播（auto_clear_after_cb），
//   也就是播出一段 0。等我们再把新数据送进去，接上去的幅度是"包络算到哪儿算哪儿"，
//   多半不是 0 —— 从 0 直接跳到半幅正弦，就是那一声爆响。
//   所以**断流本身就是爆音，跟当时响不响无关**：静音期断流，等到下次起振时
//   接上的那一下同样是硬切。上一版"静音就停喂"正好留着这个口子：停喂期间
//   一旦有人写 Flash（关 cache），存货被吃完，下一次起振就是硬的。
//
// 【一直写会把按键手感拖慢吗】
//   会 —— 但拖慢的不是"一直写"，是"写太快"。写入速率恰好等于播放速率时，
//   DMA 里常备的存货量是恒定的，不会越积越多。（上一版确实写太快了：
//   一块 40 ms 的量 + 20 ms 的 delay，净积压 20 ms/轮，几轮就把 180 ms 的环灌满，
//   按键下去要等满环才出声 —— 那是节奏的问题，不是"持续供给"本身的问题。）
//   所以这里用一条**绝对时间线**做节流：第 n 块应在 T(n) = t0 + n × 8 ms 播出，
//   想让存货常备 backlog 毫秒，就在 T(n) − backlog 那一刻写。写晚了会自动补上，
//   没到点就 vTaskDelay —— 让出 CPU，不忙等。
//
//   · 平时 backlog = 24 ms：起振只晚这么多，又够扛几次调度抖动。
//   · 落盘之前临时顶到 140 ms（audio_prefill）：写 Flash 会关 cache，那几百毫秒里
//     音频任务连代码都取不到，只能靠存货撑过去 —— 这就是文档说的
//     "根据实测停顿和内部 RAM 预算准备排队的 PCM"。140 < DMA 环的 180 ms，不会写爆。
//
// ③ 写音频这条路上不做任何阻塞的事：不写 Flash、不打日志、不抢 LVGL 锁。
#define AUDIO_BACKLOG_MS      16      // 平时常备的 PCM 排队量（毫秒）= 按键到出声的延迟
#define AUDIO_SAVE_BACKLOG_MS 140     // 落盘前临时顶上去的量，必须 < DMA 缓冲 180 ms
#define AUDIO_CATCHUP_MS      120     // 掉队超过这么多就重新对齐，别一次性补上整环

static void audio_prefill(bool on) {
    s_backlog_ms = on ? (uint32_t)AUDIO_SAVE_BACKLOG_MS : (uint32_t)AUDIO_BACKLOG_MS;
}

// ★ 正弦表。原来每块调 64 次 sinf()，而 C3 没有 FPU，sinf 是纯软件算的：
//   一块 8 ms 的量要花 7 ms 才生成完，音频任务等于吃掉了近九成 CPU ——
//   时间线节流被它拖得形同虚设（wait 永远接近 0），DMA 一路被灌到满环 180 ms，
//   按键下去要等满环才出声；LVGL 只剩一点残羹，一次重绘要 90 ms。
//   查表之后一块的生成开销从 7 ms 降到几十微秒，节流才真正有意义。
#define TONE_TBL_N 256
// 内部 DRAM：写 Flash 时 cache 会关掉，表放在 Flash 常量区那会儿读不到。
static DRAM_ATTR int16_t s_sin[TONE_TBL_N];
static void tone_table_init(void) {
    static bool ready;
    if (ready) return;
    for (int i = 0; i < TONE_TBL_N; i++)
        s_sin[i] = (int16_t)(sinf(2.0f * (float)M_PI * (float)i / (float)TONE_TBL_N) * 32767.0f);
    ready = true;
}

static void audio_task(void *arg) {
    (void)arg;
    if (bsp_audio_set_format(AUDIO_HZ, 16, 1) != ESP_OK) {
        ESP_LOGE(TAG, "音频格式设置失败，侧音不可用");
        s_audio_run = false;
        s_audio_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    bsp_audio_set_volume((uint8_t)S.vol);
    tone_table_init();

    // 内部 DRAM：Flash 写的时候 cache 会关掉，放在 Flash 常量区的东西那会儿读不到。
    static DRAM_ATTR int16_t pcm[AUDIO_CHUNK];
    // 相位是 Q32 定点（0..2^32 对应一个周期），包络是 Q16（0..65536）。
    // 全整数：没有 FPU 的核上，浮点是这个任务最大的开销来源。
    uint32_t ph = 0;
    int32_t  env = 0;

    uint32_t cur_bl = s_backlog_ms;
    uint32_t prev_end = ms32();
    // 时间线：第 n 块应在 due 这一刻被写进去。第一块的 due 已经在过去，
    // 于是开头会连着写几块把存货填到 backlog，之后就按 8 ms 一块同速走。
    int64_t due = (int64_t)prev_end - (int64_t)cur_bl;

    while (s_audio_run) {
        uint32_t now = ms32();

        // 排队量的目标变了（落盘前要顶上去 / 落盘后要收回来）：把时间线整体挪过去。
        // 变大 = due 前移 = 立刻连写几块补上；变小 = due 后移 = 多等一会儿让播放追上。
        uint32_t want = s_backlog_ms;
        if (want != cur_bl) { due -= (int32_t)((int64_t)want - (int64_t)cur_bl); cur_bl = want; }

        int32_t wait = (int32_t)(due - (int64_t)now);
        if (wait > 0) {
            if ((uint32_t)wait > s_sleep_ms_max) s_sleep_ms_max = (uint32_t)wait;
            vTaskDelay(pdMS_TO_TICKS(wait));        // 让出 CPU，不忙等
        } else if ((int32_t)((int64_t)now - due) > AUDIO_CATCHUP_MS) {
            // 掉队太多（多半是刚被关 cache 掐过，或者被高优先级任务压了一阵）：
            // 重新对齐，别把攒下的几百毫秒一次性灌进 DMA —— 那等于把起振延迟
            // 拉到满环 180 ms。宁可丢掉这几块，也要让声音跟手。
            due = (int64_t)now - (int64_t)cur_bl;
        }

        int hz = 0;
        if (!tx_sounding(ms32(), &hz) && s_rx_on) hz = s_rx_pitch;
        uint32_t dph = hz > 0 ? (uint32_t)(((uint64_t)hz << 32) / (uint64_t)AUDIO_HZ) : 0u;
        int32_t  amp_max = (int32_t)(11000.0f * (S.vol / 100.0f));
        int32_t  tgt = hz > 0 ? 65536 : 0;

        for (int i = 0; i < AUDIO_CHUNK; i++) {
            ph += dph;
            env += (tgt - env) >> 6;                // 约 8 ms 起落，消除咔哒声
            int32_t amp = (env * amp_max) >> 16;
            pcm[i] = (int16_t)(((int32_t)s_sin[(ph >> 24) & (TONE_TBL_N - 1)] * amp) >> 15);
        }
        s_audio_audible = (env > 64);               // 约千分之一以下算收干净了

        uint32_t tw = ms32();
        if (bsp_audio_write(pcm, sizeof(pcm)) != ESP_OK) {
            // 写失败只在"设备没打开/休眠"时发生。这时不能空转，让出 CPU 等一会儿，
            // 顺手记一笔，出现即说明音频链路出问题了。
            s_pcm_write_fail++;
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        uint32_t wdt = ms32() - tw;                 // 这一次 write 阻塞了多久
        if (wdt > s_write_ms_max) s_write_ms_max = wdt;

        // 供给间隔 = 两次写完之间的时间。稳态就是一块的 8 ms；
        // 多出来的那一段就是"该写却没写"的停顿，超过常备存货就意味着 DMA 被放空过
        // —— 那就是一声爆响，单独记一笔。
        uint32_t t = ms32();
        uint32_t period = t - prev_end;
        if (period > s_pcm_gap_ms_max) s_pcm_gap_ms_max = period;
        if (period > cur_bl + (uint32_t)(AUDIO_CHUNK * 1000 / AUDIO_HZ)) s_pcm_underrun++;
        s_blocks++;
        prev_end = t;
        due += (int64_t)(AUDIO_CHUNK * 1000 / AUDIO_HZ);   // 这一块占 8 ms
    }
    s_audio_task = NULL;
    vTaskDelete(NULL);
}

// 正在出声吗？落盘（NVS）要躲开这个时段：写 Flash 会关 cache，
// 音频任务那几毫秒喂不上数据，正在响的音就会断一下。
// 除了"键还按着 / 还在收报"，还要看包络 —— 逻辑上松了键，波形还在往下走的时候
// 被硬切到 0，听感上同样是"啪"一下。
static bool audio_sounding(void) { return s_tx_on || s_rx_on || s_audio_audible; }

// 立刻停掉音频任务（OTA 前用）。升级过程要连续写几秒 Flash，那期间 cache 反复
// 被关掉，喂 PCM 的任务必然断供 —— 与其让它爆一路杂音，不如先静音。
// 调用方之后会重启，所以它不需要再被拉起来。
static void audio_stop_now(void) {
    s_audio_run = false;
    for (int i = 0; i < 50 && s_audio_task; i++) vTaskDelay(pdMS_TO_TICKS(20));
}

// 音量：改 S.vol 的地方调 vol_set()，真正的寄存器写入由 vol_flush() 挑时机做。
// ★ 为什么不当场写：esp_codec_dev_set_out_vol 要往 ES8311 写寄存器，正在出声时改
//   增益，codec 内部是从旧增益直接跳到新增益的，喇叭上就是"啪"一声 —— 按 OK 确认
//   音量时那一下杂音多半就是它。软件侧的 scale 用的是 S.vol，当场就生效，听感不受
//   影响；硬件增益晚一两拍补上，没人听得出来。
static int  s_vol_hw = -1;
static bool s_vol_pending;
static void vol_set(void) { s_vol_pending = true; }
static void vol_flush(void) {
    if (!s_vol_pending) return;
    if (audio_sounding()) return;            // 正在响：等下一拍再说
    bsp_audio_set_volume((uint8_t)S.vol);
    s_vol_hw = (int)S.vol;
    s_vol_pending = false;
}

// 落盘任务：优先级 3，低于音频(6)、按键(5)和 LVGL(4) —— 写 Flash 时它自己会被
// 关掉的 cache 拖慢，但绝不能反过来把喂 PCM 的那条路堵住。
static void save_task(void *arg) {
    (void)arg;
    for (;;) {
        // 阻塞等通知：任务必须让出 CPU，绝不轮询。
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s_sets_dirty) continue;
        // 等一个安全的播放边界：正在出声（发报侧音 / 收报音）就先等一等，
        // 等不到也不无限拖 —— 对方长鸣十秒的情况下设置也得存得下来。
        uint32_t t0 = ms32();
        while (audio_sounding() && (uint32_t)(ms32() - t0) < SAVE_WAIT_MS) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (!s_sets_dirty) continue;
        // ★ 下笔之前先把 PCM 存货顶到 140 ms。写 Flash 会关 cache，那几百毫秒里
        //   音频任务连代码都取不到，DMA 只能靠存货撑 —— 存货吃完就是断流，
        //   断流之后接上的第一块是硬切，就是那一声爆炸音。
        //   代价是这一瞬间起振会晚 140 ms，但换来的是"写盘不再出声"。
        audio_prefill(true);
        vTaskDelay(pdMS_TO_TICKS(AUDIO_SAVE_BACKLOG_MS + 20));   // 等它真的填满
        settings_write();
        audio_prefill(false);
    }
}

// ===========================================================================
// 界面构建
// ===========================================================================
static lv_obj_t *make_label(lv_obj_t *parent, int x, int y, int w, const char *text,
                            const lv_font_t *font, uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y);
    if (w > 0) lv_obj_set_width(l, w);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

// 屏幕最外圈那一圈边框就是在线/离线指示：绿=在线（可发可收），灰=离线。
// 用边框而不是色块，是为了把中间那一大片留给内容 —— 240x320 的屏上，
// 一整圈 6px 的边已经足够醒目，又不用跟频谱、名单抢地方。
static void refresh_online(void) {
    if (!s_bar_online) return;
    lv_obj_set_style_border_color(s_bar_online,
                                  lv_color_hex(s_online ? 0x2ECC40 : 0x4A5563), 0);
}

// 顶行的 Wi-Fi 图标：连上了三根全亮，只有 Wi-Fi 在等注册亮两根，没连只留一根暗条。
static void refresh_wifi(void) {
    int lv = s_net_state == CW_NET_LINK ? 3 :
             s_net_state == CW_NET_WIFI ? 2 : 0;
    for (int i = 0; i < 3; i++) {
        if (!s_wifi_bars[i]) continue;
        if (i >= lv) { lv_obj_add_flag(s_wifi_bars[i], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_remove_flag(s_wifi_bars[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(s_wifi_bars[i],
                                  lv_color_hex(lv == 3 ? 0xC8D6E5 : 0x3A4450), 0);
    }
}

static void build_main(void) {
    s_main = lv_obj_create(NULL);
    lv_obj_set_size(s_main, SCR_W, SCR_H);
    lv_obj_set_style_bg_color(s_main, lv_color_hex(0x0B1016), 0);
    lv_obj_set_style_bg_opa(s_main, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_main, 0, 0);
    lv_obj_set_style_pad_all(s_main, 0, 0);

    // 顶行离圆角最近，左右都按 SAFE 收：左边从 6 挪到 14，右边让电量在 226 收尾。
    // 左上角放本台呼号（"CW 40M" 那种模式/波段标识让位给它）：
    // 通联时最需要一眼看到的是"我是谁"，波段本来就从频率上看得出来。
    // 顶行整体下移到 y=8：最外圈那 6px 是在线状态框（见 build_main 末尾），
    // 它压在所有内容之上，贴着 y=4 排的字会被它啃掉一截。
    s_lbl_call = make_label(s_main, SAFE, 8, 120, "", &lv_font_montserrat_14, 0x7A8CA0);
    s_lbl_bat = make_label(s_main, SCR_W - SAFE - 56, 8, 56, "", &lv_font_montserrat_14, 0x7A8CA0);
    lv_obj_set_style_text_align(s_lbl_bat, LV_TEXT_ALIGN_RIGHT, 0);

    // 顶行 Wi-Fi 图标：三根递增的竖条，有网就亮三根，没网只留最矮那根并压暗。
    // Montserrat 里没有信号塔那种字形，用几根矩形拼最省事，扫一眼就够。
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = lv_obj_create(s_main);
        int h = 4 + i * 3;                       // 4 / 7 / 10，底边都落在 y=18
        lv_obj_set_pos(b, 147 + i * 5, 18 - h);
        lv_obj_set_size(b, 3, h);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_radius(b, 0, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        s_wifi_bars[i] = b;
    }
    refresh_wifi();

    s_lbl_freq = make_label(s_main, SAFE, 22, CONTENT_W, "7.024.200",
                            &lv_font_montserrat_20, 0xFFB300);

    // 频谱窗口宽度：放大以后 40 根柱子可能只覆盖 1 kHz，不给个数字就不知道自己看的多宽。
    // 放在频率那一行的右侧（20px 的频率字约到 x=124，右边是空的）。
    s_lbl_span = make_label(s_main, 150, 28, SCR_W - SAFE - 150, "SPAN 5k",
                            &lv_font_montserrat_14, 0x7A8CA0);
    lv_obj_set_style_text_align(s_lbl_span, LV_TEXT_ALIGN_RIGHT, 0);

    s_lbl_pitch = make_label(s_main, SAFE, 48, 120, "-- Hz  S0", &lv_font_montserrat_14, 0xC8D6E5);
    s_bar_s = lv_bar_create(s_main);
    lv_obj_set_pos(s_bar_s, 140, 48);
    lv_obj_set_size(s_bar_s, SCR_W - SAFE - 140, 10);
    lv_bar_set_range(s_bar_s, 0, 9);
    lv_bar_set_value(s_bar_s, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar_s, lv_color_hex(0x1B2430), 0);
    lv_obj_set_style_bg_opa(s_bar_s, LV_OPA_COVER, 0);

    // 频谱：40 根柱子，服务端每 1.5s 下发一次占用表，本地不跑 FFT（单核跑不起）。
    // 起点让开左边框的 6px、柱距收到 5px，最后一根正好停在右边框内侧。
    for (int i = 0; i < SPEC_BARS; i++) {
        lv_obj_t *b = lv_obj_create(s_main);
        lv_obj_set_pos(b, SPEC_X(i), 66);
        lv_obj_set_size(b, SPEC_BAR_W, 2);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x2E86C1), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_radius(b, 0, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        s_bars[i] = b;
    }

    // 台站竖线：画在柱子之上（后创建的在上层），用 roster 里的精确频率定位。
    // 绿色 = 别人，琥珀箭头 = 我，一眼分得开。默认全隐藏，draw_ticks() 按需点亮。
    for (int i = 0; i < ROSTER_MAX; i++) {
        lv_obj_t *t = lv_obj_create(s_main);
        lv_obj_set_size(t, 2, 30);
        lv_obj_set_pos(t, SPEC_X0, 66);
        lv_obj_set_style_bg_color(t, lv_color_hex(0x7CE38B), 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(t, 0, 0);
        lv_obj_set_style_radius(t, 0, 0);
        lv_obj_set_style_pad_all(t, 0, 0);
        lv_obj_add_flag(t, LV_OBJ_FLAG_HIDDEN);
        s_ticks[i] = t;
    }

    // 频道位置：柱子只表达"这个频点有人"，自己的位置另用箭头标 —— 两者混在一根
    // 柱子上时，没信号的格子里那 1px 的琥珀块根本看不出来。
    s_marker = lv_image_create(s_main);
    lv_image_set_src(s_marker, &s_arrow_dsc);
    lv_obj_set_pos(s_marker, SPEC_X0, SPEC_BASE_Y);

    make_label(s_main, SAFE, 112, 40, "RX", &lv_font_montserrat_14, 0x7A8CA0);
    s_lbl_rx = make_label(s_main, SAFE + 36, 112, CONTENT_W - 36, "", &lv_font_montserrat_14, 0x7CE38B);

    make_label(s_main, SAFE, 134, 40, "TX", &lv_font_montserrat_14, 0x7A8CA0);
    s_lbl_tx = make_label(s_main, SAFE + 36, 134, CONTENT_W - 36, "", &lv_font_montserrat_14, 0xFFB300);

    for (int i = 0; i < STATION_ROWS; i++) {
        s_lbl_st[i] = make_label(s_main, SAFE, 162 + i * 18, CONTENT_W, "", &lv_font_montserrat_14,
                                 i == 0 ? 0xE4F1FF : 0x8FA3B8);
        // ★ 太长就打省略号，绝不折行：呼号宽窄差得多（6 位常见，也有 8 位的），
        //   折出来的第二行会顶到下面那一行上，整块名单看着是乱的。
        lv_label_set_long_mode(s_lbl_st[i], LV_LABEL_LONG_MODE_DOTS);
    }

    s_lbl_net = make_label(s_main, SAFE, 252, CONTENT_W, "Wi-Fi...", &lv_font_montserrat_14, 0x7A8CA0);

    // 在线/离线状态：从"底部一条"改成绕屏幕一整圈 —— 余光扫到屏幕边缘就知道
    // 当前在线还是离线，不用特地去找底部那条。做法是一个铺满全屏、中间全透明、
    // 只有 6px 边框的对象，并且必须压在所有内容之上（否则会被频谱柱和文字盖住）。
    s_bar_online = lv_obj_create(s_main);
    lv_obj_set_pos(s_bar_online, 0, 0);
    lv_obj_set_size(s_bar_online, SCR_W, SCR_H);
    lv_obj_set_style_bg_opa(s_bar_online, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_bar_online, 6, 0);
    // 四角按玻璃的圆角走（SCR_CORNER_R）。直角方框戳在圆角外面，
    // 两个下角看起来像被切了一块，远不如顺着玻璃的圆角描一圈。
    lv_obj_set_style_radius(s_bar_online, SCR_CORNER_R, 0);
    lv_obj_set_style_pad_all(s_bar_online, 0, 0);
    lv_obj_remove_flag(s_bar_online, LV_OBJ_FLAG_CLICKABLE);   // 纯装饰，别挡输入
    lv_obj_move_foreground(s_bar_online);
    refresh_online();
}

static const char *MENU_NAME[ADJ_COUNT] = { "SIDE TONE", "SPEED", "VOLUME", "STEP",
                                            "WI-FI SETUP", "BASE STATION", "CALLSIGN",
                                            "BRIGHTNESS", "BL TIMEOUT",
                                            "REBOOT", "FACTORY RESET", "UPDATE", "ABOUT ME" };

static void build_menu(void) {
    s_menu = lv_obj_create(NULL);
    lv_obj_set_size(s_menu, SCR_W, SCR_H);
    lv_obj_set_style_bg_color(s_menu, lv_color_hex(0x0B1016), 0);
    lv_obj_set_style_bg_opa(s_menu, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_menu, 0, 0);
    lv_obj_set_style_pad_all(s_menu, 0, 0);
    make_label(s_menu, SAFE, 6, 160, "SETTINGS", &lv_font_montserrat_20, 0xFFB300);

    // 10 行 × 44 = 440，一屏（250）装不下，所以行挂进一个可滚动的容器里。
    // LVGL 默认会把子对象裁到父对象范围内，露在容器外的行自然看不见。
    s_menu_list = lv_obj_create(s_menu);
    lv_obj_set_pos(s_menu_list, SAFE, MENU_LIST_Y);
    lv_obj_set_size(s_menu_list, CONTENT_W, MENU_VIEW_H);
    lv_obj_set_style_bg_opa(s_menu_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_menu_list, 0, 0);
    lv_obj_set_style_radius(s_menu_list, 0, 0);
    lv_obj_set_style_pad_all(s_menu_list, 0, 0);
    // 默认滚动动画是几百毫秒，长按连发（每 110ms 一次）会追不上、看着拖影，
    // 这里压到 180ms，滚动基本跟手。
    lv_obj_set_style_anim_time(s_menu_list, MENU_SCROLL_ANIM_MS, 0);
    // 不要滚动条：右侧那条竖杠在 240px 宽的屏上很显眼，而且行数一眼数得过来，
    // 露出半行就已经说明"下面还有"了。
    lv_obj_set_scrollbar_mode(s_menu_list, LV_SCROLLBAR_MODE_OFF);
    s_menu_scroll = 0;

    for (int i = 0; i < ADJ_COUNT; i++) {
        lv_obj_t *box = lv_obj_create(s_menu_list);
        lv_obj_set_pos(box, 0, i * MENU_PITCH);
        lv_obj_set_size(box, CONTENT_W, MENU_ROW_H);
        lv_obj_set_style_bg_color(box, lv_color_hex(0x141C26), 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(box, 0, 0);
        lv_obj_set_style_radius(box, 6, 0);
        lv_obj_set_style_pad_all(box, 0, 0);
        s_rows[i] = lv_label_create(box);
        lv_obj_set_style_text_font(s_rows[i], &lv_font_montserrat_16, 0);
        lv_obj_set_pos(s_rows[i], 12, 9);    // 行高 38、字 16：上下各留 11 左右，看着居中
    }
    lv_obj_update_layout(s_menu_list);

    // 底部署名：列表区收在 y=286，292 起这一段是空的。
    lv_obj_t *sign = make_label(s_menu, 0, 292, SCR_W, "Design by ZGF",
                                &lv_font_montserrat_14, 0x4A5A6B);
    lv_obj_set_style_text_align(sign, LV_TEXT_ALIGN_CENTER, 0);
}

static void build_adj(void) {
    s_adj = lv_obj_create(NULL);
    lv_obj_set_size(s_adj, SCR_W, SCR_H);
    lv_obj_set_style_bg_color(s_adj, lv_color_hex(0x0B1016), 0);
    lv_obj_set_style_bg_opa(s_adj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_adj, 0, 0);
    lv_obj_set_style_pad_all(s_adj, 0, 0);

    s_adj_title = make_label(s_adj, SAFE, 8, CONTENT_W, "", &lv_font_montserrat_14, 0x7A8CA0);
    s_adj_value = make_label(s_adj, SAFE, 30, CONTENT_W, "", &lv_font_montserrat_20, 0xFFB300);

    s_adj_bar = lv_bar_create(s_adj);
    lv_obj_set_pos(s_adj_bar, SAFE, 62);
    lv_obj_set_size(s_adj_bar, CONTENT_W, 12);
    lv_bar_set_range(s_adj_bar, 0, 100);
    lv_obj_set_style_bg_color(s_adj_bar, lv_color_hex(0x1B2430), 0);
    lv_obj_set_style_bg_opa(s_adj_bar, LV_OPA_COVER, 0);

    s_adj_note = make_label(s_adj, SAFE, 84, CONTENT_W, "", &lv_font_montserrat_14, 0x8FA3B8);
    lv_obj_set_style_text_align(s_adj_note, LV_TEXT_ALIGN_LEFT, 0);

    // Wi-Fi 确认页的左右两个按钮。板子上只有 UP/DOWN/OK 三个键、没有左右键，
    // 所以"左右"只是屏幕上的排布：UP/DN 移动焦点，OK 选中。默认隐藏，
    // 只有 ADJ_WIFI 那一页才显示（见 refresh_adj）。
    static const char *BTN_TXT[2] = { "SET", "CANCEL" };
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = lv_obj_create(s_adj);
        lv_obj_set_pos(b, SAFE + i * 110, 96);
        lv_obj_set_size(b, 102, 46);
        lv_obj_set_style_radius(b, 6, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        s_adj_btn[i] = b;
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, BTN_TXT[i]);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_center(l);
        s_adj_btn_lbl[i] = l;
        lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    }

    // 列表行：4 行 × 34 高，从 y=42 起排到 y=190，下面 y=204 留给说明文字。
    // 做成和菜单行同样的观感（暗底圆角 + 选中填琥珀），避免两种页面看着像两套 UI。
    for (int i = 0; i < ADJ_LIST_MAX; i++) {
        lv_obj_t *b = lv_obj_create(s_adj);
        lv_obj_set_pos(b, SAFE, 42 + i * 38);
        lv_obj_set_size(b, CONTENT_W, 34);
        lv_obj_set_style_radius(b, 5, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        s_adj_list[i] = b;
        lv_obj_t *l = lv_label_create(b);
        // 只编译了 14 和 20 两档字号（sdkconfig 里确认过），列表行用 14，
        // 免得为一行字再挂一套 16 号字体进 flash。
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_set_pos(l, 12, 8);
        s_adj_list_lbl[i] = l;
        lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    }

    // ABOUT ME：一段设备信息 + 一段作者信息，全英文文字（以前是中文位图）。
    // 整块装进一个透明容器，显隐只动容器一个对象的 flag。平时隐藏，
    // 只有 ADJ_ABOUT 那页显示；行内容由 refresh_adj 填。
    s_about = lv_obj_create(s_adj);
    lv_obj_set_pos(s_about, SAFE, 30);
    // 高 232：最后一行（Web）的网址是两行排版，比别的行多占 18px。
    lv_obj_set_size(s_about, CONTENT_W, 232);
    lv_obj_set_style_bg_opa(s_about, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_about, 0, 0);
    lv_obj_set_style_pad_all(s_about, 0, 0);
    lv_obj_remove_flag(s_about, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *h = lv_label_create(s_about);
        lv_obj_set_pos(h, 0, i == 0 ? 0 : 92);      // DEVICE / AUTHOR 两个小标题
        lv_obj_set_size(h, CONTENT_W, 18);
        lv_obj_set_style_text_font(h, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(h, lv_color_hex(0xFFB300), 0);
        s_about_hdr[i] = h;
    }
    for (int i = 0; i < ABOUT_ROWS; i++) {
        // 前 3 行是设备信息，后 5 行是作者信息，段内行距 20。
        int y = (i < 3) ? (22 + i * 20) : (114 + (i - 3) * 20);
        lv_obj_t *k = lv_label_create(s_about);
        lv_obj_set_pos(k, 0, y);
        lv_obj_set_size(k, ABOUT_KEY_W, 18);
        lv_obj_set_style_text_font(k, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(k, lv_color_hex(0x8FA3B8), 0);
        s_about_key[i] = k;
        lv_obj_t *v = lv_label_create(s_about);
        lv_obj_set_pos(v, ABOUT_VAL_X, y);
        // Web 那一行的网址有 200px，比取值列（132px）宽，要占两行；其余行都是单行。
        // ★ 高 40：montserrat_14 的 line_height 是 16，两行 32px，
        //   原来给 36 只够单行再宽一点，两行贴着边容易裁掉下沿，这里留足。
        lv_obj_set_size(v, ABOUT_VAL_W, (i == ABOUT_ROWS - 1) ? 40 : 18);
        lv_obj_set_style_text_font(v, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(v, lv_color_hex(0xE4F1FF), 0);
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
        s_about_val[i] = v;
    }
    lv_obj_add_flag(s_about, LV_OBJ_FLAG_HIDDEN);

    // WI-FI SETUP 页的网络实况：IP / MAC / 连接状态。
    // 摆在标题（y=8）和两颗按钮（y=96）之间那块空档里。平时隐藏。
    for (int i = 0; i < NETINFO_ROWS; i++) {
        int y = 34 + i * 20;
        lv_obj_t *k = lv_label_create(s_adj);
        lv_obj_set_pos(k, SAFE, y);
        lv_obj_set_size(k, NETINFO_KEY_W, 18);
        lv_obj_set_style_text_font(k, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(k, lv_color_hex(0x8FA3B8), 0);
        s_netinfo_key[i] = k;
        lv_obj_t *v = lv_label_create(s_adj);
        lv_obj_set_pos(v, SAFE + NETINFO_KEY_W + 4, y);
        lv_obj_set_size(v, CONTENT_W - NETINFO_KEY_W - 4, 18);
        lv_obj_set_style_text_font(v, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(v, lv_color_hex(0xE4F1FF), 0);
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
        s_netinfo_val[i] = v;
        lv_obj_add_flag(k, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(v, LV_OBJ_FLAG_HIDDEN);
    }
}

// ===========================================================================
// 刷新
// ===========================================================================
// 频谱的显示窗口（SPAN）跟着 VFO 步进自动缩放：步进越小，越得放大才看得见自己在挪。
// 原来固定铺满 7.000–7.200（226px / 200kHz），10 Hz 步进按一下只挪 0.01px —— 等于没动。
// 四档都按"按一下 ≈ 2~11 px"配，窗口以自己的频率为中心：放大后看到的是**信号相对你**
// 在动，自己的箭头定在正中，跟真机缩放 span 是同一回事。
static uint32_t spec_span_hz(void) {
    switch (S.step) {
        case 10000u: return 200000u;    // 全段找台
        case 1000u:  return 50000u;
        case 100u:   return 5000u;      // 常用档：按一下约 4.5 px
        default:     return 1000u;      // 10 Hz 档：按一下约 2.3 px
    }
}
static uint32_t spec_lo_hz(uint32_t span) {
    uint32_t half = span / 2;
    if (S.freq <= F_MIN + half) return F_MIN;          // 贴着频段边沿，窗口不再往外挪
    if (S.freq >= F_MAX - half) return F_MAX - span;
    return S.freq - half;
}
// 频率 → 柱区 x。柱子、自己的箭头、台站竖线都走这一把尺子，改一处就都跟着变。
static int spec_x_of(uint32_t f, uint32_t lo, uint32_t span) {
    int64_t pos = (int64_t)f - (int64_t)lo;
    if (pos < 0) pos = 0;
    if (pos > (int64_t)span) pos = span;
    return SPEC_X0 + (int)(pos * (SPEC_SPAN - SPEC_BAR_W) / span);
}

// 自己的位置：不用"自己那一格"的柱号反推，是因为占用表可能还没来（s_bins_n == 0），
// 那时柱号算不出来，而频率是本地本来就有的。
static void draw_marker(void) {
    if (!s_marker) return;
    uint32_t span = spec_span_hz(), lo = spec_lo_hz(span);
    int x = spec_x_of(S.freq, lo, span) + SPEC_BAR_W / 2 - CW_ARROW_W / 2;
    if (x < 2) x = 2;
    if (x > SCR_W - CW_ARROW_W - 2) x = SCR_W - CW_ARROW_W - 2;
    lv_obj_set_pos(s_marker, x, SPEC_BASE_Y);
}

// 台站竖线：占用表一格 2 kHz，窗口缩到 1 kHz 时整屏只落在同一格里，柱子只能是块。
// 这时候真正能看清"对方在我左边多少 Hz"的是 roster 里的精确频率 —— 它没有分辨率上限。
static void draw_ticks(uint32_t lo, uint32_t span) {
    if (!s_ticks[0]) return;
    int k = 0;
    for (int i = 0; i < s_roster_n && k < ROSTER_MAX; i++) {
        if (roster_is_me(&s_roster[i])) continue;
        uint32_t f = s_roster[i].freq;
        if (f < lo || f > lo + span) continue;         // 窗口外的台站不画
        lv_obj_clear_flag(s_ticks[k], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_ticks[k], spec_x_of(f, lo, span), 66);
        k++;
    }
    for (; k < ROSTER_MAX; k++) lv_obj_add_flag(s_ticks[k], LV_OBJ_FLAG_HIDDEN);
}

static void draw_spectrum(void) {
    uint32_t span = spec_span_hz(), lo = spec_lo_hz(span);
    draw_marker();                      // 频率一变就得挪，跟占用表下不下发无关
    draw_ticks(lo, span);               // 台站位置来自 roster，同样跟占用表无关
    if (!s_bins_n || !s_bars[0]) return;
    const int h_max = 30;
    for (int i = 0; i < SPEC_BARS; i++) {
        // 这根柱子覆盖的频率区间落到哪几个 bin，就取其中最忙的那个：
        // 全段时一柱约 2.5 格（取 max 才不漏台），放大后几十柱共用一格（成块，正常）。
        uint32_t f0 = lo + (uint32_t)((uint64_t)span * i / SPEC_BARS);
        uint32_t f1 = lo + (uint32_t)((uint64_t)span * (i + 1) / SPEC_BARS);
        int b0 = (int)((f0 - F_MIN) / SPEC_BIN_HZ);
        int b1 = (int)((f1 - F_MIN) / SPEC_BIN_HZ);
        int v = 0;
        for (int b = b0; b <= b1; b++)
            if (b >= 0 && b < s_bins_n && s_bins[b] > v) v = s_bins[b];
        int h = v <= 0 ? 1 : (v >= 4 ? h_max : 6 + v * 6);
        if (h > h_max) h = h_max;
        lv_obj_set_size(s_bars[i], SPEC_BAR_W, h);
        lv_obj_set_pos(s_bars[i], SPEC_X(i), 66 + (h_max - h));
        lv_obj_set_style_bg_color(s_bars[i], lv_color_hex(v ? 0x2E86C1 : 0x1B2430), 0);
    }
}

// 屏幕上该显示哪个呼号：Ham 优先，没填就用服务器下发的虚拟呼号，都没有返回空串。
// 空串是有意的 —— 顶行那块地方直接留白，比写个 NOCALL 或拿 MAC 凑的假号诚实。
static const char *top_call(void) {
    const char *c = cw_net_ham();
    if (!c[0]) c = cw_net_vcall();
    return c;
}
// 状态行那种"必须有内容"的场合：两个都没有就摆两个短横，否则会显示成 "online | "。
static const char *top_call_dash(void) {
    const char *c = top_call();
    return c[0] ? c : "--";
}

static void refresh_main(void) {
    char buf[96];                                  // 状态行要塞下 呼号/频率/收听音调/S 值，留足余量
    fmt_freq(buf, sizeof(buf), S.freq);
    lv_label_set_text(s_lbl_freq, buf);

    if (s_lbl_span) {                              // 窗口宽度：SPAN 200k/50k/5k/1k
        uint32_t sp = spec_span_hz();
        snprintf(buf, sizeof(buf), "SPAN %uk", (unsigned)(sp / 1000u));
        lv_label_set_text(s_lbl_span, buf);
    }

    // 顶行显示"我是谁"：Ham（用户自己填的）优先，没填才用服务器下发的虚拟呼号，
    // 两个都没有就留空 —— 不拿 MAC 凑一个假号出来，宁可这一行是空的。
    // 每轮重取：认证成功前后它会从空变成虚拟呼号，写死一次就不跟手了。
    if (s_lbl_call) lv_label_set_text(s_lbl_call, top_call());
    refresh_online();

    if (s_rx_on || s_rx_pitch > 0) snprintf(buf, sizeof(buf), "%d Hz  S%d", s_rx_pitch, s_rx_s);
    else snprintf(buf, sizeof(buf), "-- Hz  S0");
    lv_label_set_text(s_lbl_pitch, buf);
    lv_bar_set_value(s_bar_s, s_rx_on ? s_rx_s : 0, LV_ANIM_OFF);

    lv_label_set_text(s_lbl_rx, s_rxbuf);
    // TX 行显示"已解出的字符 + 正在敲的点划"。后者让用户看清自己敲到哪儿了，
    // 也是判断"为什么解出来是 X"的唯一线索 —— 老版本只显示最终字符，等于把
    // 判定过程完全藏起来，出问题无从排查。
    if (s_txdec.len) {
        char pend[TXBUF_MAX + CW_MORSE_MAX_CODE + 1];
        snprintf(pend, sizeof(pend), "%s%s", s_txbuf, s_txdec.buf);
        lv_label_set_text(s_lbl_tx, pend);
    } else {
        lv_label_set_text(s_lbl_tx, s_txbuf);
    }

    // 可闻台站：按 S 值排序取前三
    // ★ 自己那一行不算：名单是服务端把频道里所有台站（含本台）一起下发来的，
    //   零拍之后自己跟自己同频，音调是 700，会稳稳进这张表；频率刚改过而服务端
    //   那份还没刷新时（离线期间转会、或 presence 还没发上去），自己就会以一个
    //   假音调冒出来，看着像"收到了自己"。认自己优先按 uid（呼号可能被服务端
    //   改名），见 roster_is_me()。
    int best[STATION_ROWS] = { -1, -1, -1 };
    for (int i = 0; i < s_roster_n; i++) {
        if (roster_is_me(&s_roster[i])) continue;
        // ★ 只有同频的信号才进收报区。以前这里按"零拍通带 ±450 Hz"筛，
        //   差出半千赫的台站也照样列出来 —— 那不是电台该有的样子：
        //   差一格就是隔壁频道，屏幕上不该有它。
        if (!same_channel(S.freq, s_roster[i].freq)) continue;
        int p = audible_pitch(S.freq, s_roster[i].freq);
        if (!p) continue;
        int s = s_of_pitch(p);
        for (int r = 0; r < STATION_ROWS; r++) {
            if (best[r] < 0) { best[r] = i; break; }
            if (s > s_of_pitch(audible_pitch(S.freq, s_roster[best[r]].freq))) {
                for (int k = STATION_ROWS - 1; k > r; k--) best[k] = best[k - 1];
                best[r] = i;
                break;
            }
        }
    }
    for (int r = 0; r < STATION_ROWS; r++) {
        if (best[r] < 0) { lv_label_set_text(s_lbl_st[r], r == 0 ? "no station on this freq" : ""); continue; }
        int p = audible_pitch(S.freq, s_roster[best[r]].freq);
        char line[64], fbuf[16];
        // ★ 呼号按 6 位宽对齐（多数呼号就是 6 位），频率用 MHz 短式（7.0242）。
        //   原来按 8 位宽补空格 + 完整频率（7.024.200），一行要 212px 而标签只有 212px，
        //   稍微长一点就折成两行 —— 这就是"信号检测有时显示两行"的来源。
        //   现在典型一行 183px，留了近 30px 余量；真遇上 8 位长呼号也只是打省略号。
        fmt_freq_mhz(fbuf, sizeof(fbuf), s_roster[best[r]].freq);
        snprintf(line, sizeof(line), "%-6s %s %4dHz S%d",
                 s_roster[best[r]].call, fbuf, p, s_of_pitch(p));
        lv_label_set_text(s_lbl_st[r], line);
    }

    const char *ns = s_net_state == CW_NET_LINK ? "ONLINE" :
                     s_net_state == CW_NET_WIFI ? "Wi-Fi only" :
                     s_net_state == CW_NET_ERROR ? "NET FAIL" : "idle";
    if (s_chat[0]) snprintf(buf, sizeof(buf), "%s | %s", ns, s_chat);
    else snprintf(buf, sizeof(buf), "%s | %s", ns, top_call_dash());
    lv_label_set_text(s_lbl_net, buf);
    refresh_wifi();                 // 顶行 Wi-Fi 图标跟着网络状态走
}

static void refresh_battery(void) {
    int soc = bsp_battery_soc();
    if (soc < 0) { lv_label_set_text(s_lbl_bat, ""); return; }
    char b[12];
    snprintf(b, sizeof(b), "%d%%", soc);
    lv_label_set_text(s_lbl_bat, b);
}

static void menu_scroll_to_sel(bool instant);

static void refresh_menu(void) {
    for (int i = 0; i < ADJ_COUNT; i++) {
        char line[40];
        switch (i) {
        case ADJ_TONE: snprintf(line, sizeof(line), "%s   %d Hz", MENU_NAME[i], S.tone); break;
        case ADJ_WPM:  snprintf(line, sizeof(line), "%s   %d WPM", MENU_NAME[i], S.wpm); break;
        case ADJ_VOL:  snprintf(line, sizeof(line), "%s   %d%%", MENU_NAME[i], S.vol); break;
        case ADJ_STEP: {
            const char *u = S.step >= 1000 ? " kHz" : " Hz";
            int v = S.step >= 1000 ? (int)(S.step / 1000) : (int)S.step;
            snprintf(line, sizeof(line), "%s   %d%s", MENU_NAME[i], v, u);
            break;
        }
        case ADJ_BL:
            snprintf(line, sizeof(line), "%s   %d%%", MENU_NAME[i], S.bl);
            break;
        case ADJ_BL_TO:
            snprintf(line, sizeof(line), "%s   %s", MENU_NAME[i], bl_to_text(S.bl_to));
            break;
        default: snprintf(line, sizeof(line), "%s", MENU_NAME[i]); break;
        }
        lv_label_set_text(s_rows[i], line);
        lv_obj_set_style_bg_color(lv_obj_get_parent(s_rows[i]),
                                  lv_color_hex(i == s_sel ? 0x243447 : 0x141C26), 0);
        lv_obj_set_style_text_color(s_rows[i],
                                    lv_color_hex(i == s_sel ? 0xFFB300 : 0xC8D6E5), 0);
    }
    menu_scroll_to_sel(false);
}

// 让选中行落在可视区里：上方留一行余量，实在排不下就贴边。
// instant=true 用于"首尾回绕"和刚进菜单——那时跳一大段，动画反而晃眼。
static void menu_scroll_to_sel(bool instant) {
    if (!s_menu_list) return;
    int top  = s_sel * MENU_PITCH;                                  // 行在内容里的顶边
    int want = s_menu_scroll;
    int up   = top + MENU_ROW_H + MENU_PITCH - MENU_VIEW_H;         // 让它下面也留一行
    int down = top - MENU_PITCH;                                    // 让它上面留一行
    if (want < up)   want = up;
    if (want > down) want = down;
    // 内容总高减去一屏就是能滚到的最远处，自己先夹住，
    // 免得记的目标值比 LVGL 实际能滚到的还大、后面几次判断都跟着偏。
    int maxs = ADJ_COUNT * MENU_PITCH - (MENU_PITCH - MENU_ROW_H) - MENU_VIEW_H;
    if (maxs < 0)    maxs = 0;
    if (want > maxs) want = maxs;
    if (want < 0)    want = 0;
    if (want == s_menu_scroll) return;
    if (!instant && (want - s_menu_scroll > MENU_VIEW_H || s_menu_scroll - want > MENU_VIEW_H))
        instant = true;                                             // 跨度超过一屏：直接跳
    s_menu_scroll = want;
    lv_obj_scroll_to_y(s_menu_list, want, instant ? LV_ANIM_OFF : LV_ANIM_ON);
}

// 把一枚举项摊成列表：items 是全部档位的字样，cur 是当前选中行的下标。
// 顺手把数值与进度条藏掉、说明文字挪到列表下方 —— 这几件事在每个列表分支里
// 都一样，抽出来省得两处写歪。
static void adj_list_show(const char *const *items, int n, int cur) {
    for (int i = 0; i < ADJ_LIST_MAX; i++) {
        if (!s_adj_list[i]) continue;
        if (i >= n) { lv_obj_add_flag(s_adj_list[i], LV_OBJ_FLAG_HIDDEN); continue; }
        bool on = (i == cur);
        lv_obj_remove_flag(s_adj_list[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(s_adj_list[i], lv_color_hex(on ? 0xFFB300 : 0x141C26), 0);
        lv_obj_set_style_bg_opa(s_adj_list[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(s_adj_list[i], lv_color_hex(on ? 0xFFB300 : 0x2A3644), 0);
        lv_obj_set_style_text_color(s_adj_list_lbl[i],
                                    lv_color_hex(on ? 0x0B1016 : 0x8FA3B8), 0);
        lv_label_set_text(s_adj_list_lbl[i], items[i]);
    }
    lv_obj_add_flag(s_adj_value, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_adj_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_adj_note, SAFE, 204);
}

// 两个"左右按钮"页共用这一段：摆出两颗按钮，焦点那颗填成实心琥珀。
// 板子上只有 UP/DOWN/OK 三个键、没有左右键，所以"左右"只是屏幕上的排布：
// UP/DN 移动焦点，OK 选中。
static void adj_btn_show(const char *l0, const char *l1, int focus) {
    const char *txt[2] = { l0, l1 };
    for (int i = 0; i < 2; i++) {
        if (!s_adj_btn[i] || !s_adj_btn_lbl[i]) continue;
        bool on = (i == focus);
        lv_obj_remove_flag(s_adj_btn[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(s_adj_btn[i], lv_color_hex(on ? 0xFFB300 : 0x141C26), 0);
        lv_obj_set_style_bg_opa(s_adj_btn[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(s_adj_btn[i], lv_color_hex(on ? 0xFFB300 : 0x2A3644), 0);
        lv_obj_set_style_text_color(s_adj_btn_lbl[i],
                                    lv_color_hex(on ? 0x0B1016 : 0x8FA3B8), 0);
        lv_label_set_text(s_adj_btn_lbl[i], txt[i]);
    }
}

static void refresh_adj(void) {
    char v[48];                 // 服务器地址最长 40（CW_PROV_SRV_MAX），24 装不下
    int pct = 0;
    // 默认是"数值 + 进度条"那套；Wi-Fi 确认页换成两个按钮，STEP / BL TIMEOUT
    // 换成列表。每轮都先复位，切到别的菜单项时才不会残留上一页的状态。
    v[0] = '\0';
    for (int i = 0; i < 2; i++) {
        if (!s_adj_btn[i]) continue;
        lv_obj_add_flag(s_adj_btn[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < ADJ_LIST_MAX; i++) {
        if (!s_adj_list[i]) continue;
        lv_obj_add_flag(s_adj_list[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (s_about) lv_obj_add_flag(s_about, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < NETINFO_ROWS; i++) {
        if (s_netinfo_key[i]) lv_obj_add_flag(s_netinfo_key[i], LV_OBJ_FLAG_HIDDEN);
        if (s_netinfo_val[i]) lv_obj_add_flag(s_netinfo_val[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_remove_flag(s_adj_value, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_adj_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_align(s_adj_value, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_align(s_adj_title, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(s_adj_title, SAFE, 8);
    lv_obj_set_style_text_align(s_adj_note, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(s_adj_note, SAFE, 84);
    lv_obj_set_style_text_color(s_adj_value, lv_color_hex(0xFFB300), 0);
    switch (s_sel) {
    case ADJ_TONE:
        snprintf(v, sizeof(v), "%d Hz", S.tone);
        pct = (S.tone - 300) * 100 / 900;
        lv_label_set_text(s_adj_note, "Sidetone pitch. 700 Hz is the usual choice.");
        break;
    case ADJ_WPM: {
        snprintf(v, sizeof(v), "%d WPM", S.wpm);
        pct = (S.wpm - 10) * 100 / 20;
        int dit = (int)cw_morse_dit_ms(S.wpm);
        static char note[48];
        snprintf(note, sizeof(note), "dit %d ms   dash %d ms", dit, dit * 3);
        lv_label_set_text(s_adj_note, note);
        break;
    }
    case ADJ_VOL:
        snprintf(v, sizeof(v), "%d%%", S.vol);
        pct = S.vol;
        lv_label_set_text(s_adj_note, "Output volume.");
        break;
    case ADJ_STEP: {
        // 列表式：四档全摊开，UP/DN 直接挪高亮那一行，OK 保存。
        static const char *const items[STEP_N] = { "10 Hz", "100 Hz", "1 kHz", "10 kHz" };
        int idx = 0;
        for (; idx < STEP_N; idx++) if (STEP_OPTS[idx] == S.step) break;
        if (idx >= STEP_N) idx = 1;                 // 存了个不在表里的值就落在 100 Hz
        adj_list_show(items, STEP_N, idx);
        lv_label_set_text(s_adj_note, "VFO step per press. Span follows.");
        break;
    }
    case ADJ_BL:
        snprintf(v, sizeof(v), "%d%%", S.bl);
        pct = S.bl;
        lv_label_set_text(s_adj_note, "Screen brightness.");
        break;
    case ADJ_BL_TO: {
        static const char *const items[BL_TO_N] = { "10 s", "30 s", "60 s", "ON" };
        int idx = 0;
        for (; idx < BL_TO_N; idx++) if (BL_TO_OPTS[idx] == S.bl_to) break;
        if (idx >= BL_TO_N) idx = 1;
        adj_list_show(items, BL_TO_N, idx);
        lv_label_set_text(s_adj_note, "Idle time before the screen dims.\nON keeps it lit (uses more battery).");
        break;
    }
    case ADJ_NET:
        // 基站页：上面那行大字是当前服务器地址，下面左"更改" / 右"确认"。
        // 改地址要重启进配网，跟 WI-FI SETUP 一样不该"选中即执行"，所以做成按钮页。
        adj_btn_show("SET", "OK", s_srv_focus);
        lv_obj_add_flag(s_adj_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_align(s_adj_value, LV_TEXT_ALIGN_CENTER, 0);
        snprintf(v, sizeof(v), "%s", cw_prov_server_ip());
        lv_label_set_text(s_adj_note,
                          s_srv_focus == 0
                              ? "Restart into the setup page."
                              : "Keep this address and go back.");
        lv_obj_set_style_text_align(s_adj_note, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(s_adj_note, SAFE, 60);   // 数值占了 30..54，说明紧跟其后
        break;
    case ADJ_WIFI: {
        // 确认页：进配网要整机重启，代价不小，不该像改音量那样"选中即执行"。
        // 这里把数值与进度条藏掉，摆出左右两个按钮，焦点那颗填成实心琥珀。
        // 上面那三行是"现在连的是什么"，先让人看清再决定要不要重配。
        adj_btn_show("SET", "OK", s_wifi_focus);
        lv_obj_add_flag(s_adj_value, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_adj_bar, LV_OBJ_FLAG_HIDDEN);
        static const char *const nk[NETINFO_ROWS] = { "IP", "MAC", "Link" };
        char nv[NETINFO_ROWS][24];
        snprintf(nv[0], sizeof(nv[0]), "%s", cw_net_ip_str());
        snprintf(nv[1], sizeof(nv[1]), "%s", cw_net_mac_str());
        snprintf(nv[2], sizeof(nv[2]), "%s",
                 (s_net_state == CW_NET_WIFI || s_net_state == CW_NET_LINK)
                     ? "Connected" : "Not linked");
        for (int i = 0; i < NETINFO_ROWS; i++) {
            if (s_netinfo_key[i]) {
                lv_label_set_text(s_netinfo_key[i], nk[i]);
                lv_obj_remove_flag(s_netinfo_key[i], LV_OBJ_FLAG_HIDDEN);
            }
            if (s_netinfo_val[i]) {
                lv_label_set_text(s_netinfo_val[i], nv[i]);
                lv_obj_remove_flag(s_netinfo_val[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        lv_label_set_text(s_adj_note,
                          s_wifi_focus == 0
                              ? "Save and restart into the setup page."
                              : "Keep the current network and go back.");
        lv_obj_set_style_text_align(s_adj_note, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(s_adj_note, SAFE, 168);
        break;
    }
    case ADJ_CALL: {
        // 呼号页：上面两行是"现在是什么"（Ham = 用户自定义 / Virtual = 服务器下发），
        // 下面左"更新" / 右"取消"。
        // ★ 只有 Ham 能更新：虚拟呼号是服务器按 Global UID 绑定的，本机改不了 ——
        //   改了也没用，下次认证还是拿回原来那个号。所以配网页上根本不给它输入框。
        adj_btn_show("SET", "OK", s_call_focus);
        lv_obj_add_flag(s_adj_value, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_adj_bar, LV_OBJ_FLAG_HIDDEN);
        static const char *const ck[NETINFO_ROWS] = { "HAM", "VIRTUAL", "Auth" };
        char cv[NETINFO_ROWS][24];
        // 没配 / 没下发就摆两个短横：不拿 MAC 凑的假号糊弄，也不写 (none) 占地方。
        snprintf(cv[0], sizeof(cv[0]), "%s", cw_net_ham()[0]   ? cw_net_ham()   : "--");
        snprintf(cv[1], sizeof(cv[1]), "%s", cw_net_vcall()[0] ? cw_net_vcall() : "--");
        snprintf(cv[2], sizeof(cv[2]), "%s", cw_net_authed() ? "OK" : "pending");
        for (int i = 0; i < NETINFO_ROWS; i++) {
            if (s_netinfo_key[i]) {
                lv_label_set_text(s_netinfo_key[i], ck[i]);
                lv_obj_remove_flag(s_netinfo_key[i], LV_OBJ_FLAG_HIDDEN);
            }
            if (s_netinfo_val[i]) {
                lv_label_set_text(s_netinfo_val[i], cv[i]);
                lv_obj_remove_flag(s_netinfo_val[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        lv_label_set_text(s_adj_note,
                          s_call_focus == 0
                              ? "Restart into the setup page\nto change your HAM call.\nVIRTUAL is issued by\nthe server and locked."
                              : "Keep both and go back.");
        lv_obj_set_style_text_align(s_adj_note, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(s_adj_note, SAFE, 168);
        break;
    }
    case ADJ_ABOUT: {
        // 两段：DEVICE（唯一 ID / 固件版本 / MAC）+ AUTHOR（联系方式）。
        // 没有可调参数：数值/进度条藏掉，OK 短按即回菜单。
        if (s_about) lv_obj_remove_flag(s_about, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_adj_value, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_adj_bar, LV_OBJ_FLAG_HIDDEN);
        // ★ 不再单列 Global UID：它就是设备 ID，两个名字一个东西，
        //   摆两行除了让人怀疑"我到底有几个 ID"之外没有别的用处。
        static const char *const ak[ABOUT_ROWS] = { "Device ID", "Firmware", "MAC",
                                                    "Creator", "QQ", "QQ Group", "Web" };
        char av[ABOUT_ROWS][40];
        // 设备 ID = MAC 的完整 48 位（12 位十六进制），服务端就是拿它绑定虚拟呼号的。
        snprintf(av[0], sizeof(av[0]), "%s", cw_net_device_id_hex());
        snprintf(av[1], sizeof(av[1]), "%s", CW_FW_VERSION);
        snprintf(av[2], sizeof(av[2]), "%s", cw_net_mac_str());
        snprintf(av[3], sizeof(av[3]), "ZGF");
        snprintf(av[4], sizeof(av[4]), "1422361371");
        snprintf(av[5], sizeof(av[5]), "1098596164");
        // 完整仓库地址。★ 整串在 montserrat_14 下有 200px，取值列只有 132px，
        //   一行必然放不下；而它中间没有空格，交给 LVGL 折行会按字符硬断，
        //   断在哪儿全看宽度，看上去就是"网址被截掉了一截"。
        //   所以这里写死换行点：域名一行（88px）、仓库名一行（112px），
        //   两行都在 132px 以内，第一行末尾留个 / 表明下一行还是同一串。
        snprintf(av[6], sizeof(av[6]), "github.com/\nsubooku/didaaa");
        for (int i = 0; i < ABOUT_ROWS; i++) {
            if (s_about_key[i]) lv_label_set_text(s_about_key[i], ak[i]);
            if (s_about_val[i]) lv_label_set_text(s_about_val[i], av[i]);
        }
        if (s_about_hdr[0]) lv_label_set_text(s_about_hdr[0], "DEVICE");
        if (s_about_hdr[1]) lv_label_set_text(s_about_hdr[1], "AUTHOR");
        lv_obj_set_style_text_align(s_adj_note, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(s_adj_note, SAFE, 264);   // 内容到 y=258，提示挪到它下面
        lv_label_set_text(s_adj_note, "Press OK to go back.");
        break;
    }
    case ADJ_FW: {
        // 固件升级页。四种形态：查询中 / 有新版待确认 / 下载中 / 出错。
        // 进页那一下由 refresh_adj 自己发起查询（见下面 s_fw_query_sent），
        // 之后的每次刷新只是把最新状态画出来 —— 包括下载进度。
        // 进页自动查一次版本。三个条件缺一不可：本轮没查过、没有任务在跑、
        // 也不是"刚升完等重启"的状态 —— 否则 tick 每 100ms 就会重发一次。
        if (!s_fw_query_sent && !cw_ota_busy() && cw_ota_state() == CW_OTA_IDLE && !s_fw_done_ms) {
            s_fw_query_sent = true;
            s_fw_focus = 1;                 // 焦点照例先落在"取消"上
            cw_ota_query_start();
        }
        cw_ota_state_t st = cw_ota_state();
        bool busy = (st == CW_OTA_BUSY);
        const char *nv = cw_ota_newver();
        bool have_new = !busy && st != CW_OTA_OK && nv[0] && cw_ota_ver_newer(nv, CW_FW_VERSION);

        lv_obj_set_style_text_align(s_adj_value, LV_TEXT_ALIGN_CENTER, 0);
        if (busy && cw_ota_pct() > 0) snprintf(v, sizeof(v), "%d%%", cw_ota_pct());
        else                          snprintf(v, sizeof(v), "v%s", CW_FW_VERSION);
        // 进度条只在真下载时露脸：查版本那几百毫秒里它是 0%，画出来只会闪一下。
        if (busy && st == CW_OTA_BUSY && s_fw_upgrading) {
            lv_obj_remove_flag(s_adj_bar, LV_OBJ_FLAG_HIDDEN);
            pct = cw_ota_pct();
        } else {
            lv_obj_add_flag(s_adj_bar, LV_OBJ_FLAG_HIDDEN);
        }
        if (have_new) {
            adj_btn_show("UPDATE", "CANCEL", s_fw_focus);
            lv_obj_add_flag(s_adj_bar, LV_OBJ_FLAG_HIDDEN);
        } else if (!busy && st != CW_OTA_OK && !(st == CW_OTA_ERR && s_fw_upgrading)) {
            // 没有新版（或压根没查到）：照样摆两个按钮 —— 一是给 UP/DOWN 一个看得见
            //   的落点（不然这页上按 UP/DOWN 毫无反应，像按键坏了），二是给"服务器
            //   连不上"留一条重试的路，不用退出去再进来。
            adj_btn_show("RETRY", "BACK", s_fw_focus);
            lv_obj_add_flag(s_adj_bar, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_style_text_align(s_adj_note, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(s_adj_note, SAFE, 152);   // 按钮在 96..142，说明紧跟着
        char note[96];
        if (have_new) {
            snprintf(note, sizeof(note), "Installed v%s\nServer has v%s (%u KB)",
                     CW_FW_VERSION, nv, (unsigned)(cw_ota_newsize() / 1024u));
        } else if (st == CW_OTA_OK) {
            snprintf(note, sizeof(note), "Installed.\nRebooting...");
        } else if (st == CW_OTA_ERR) {
            // 只在真下载失败时才说要重启（音频已停，只能靠重启恢复）；
            // 单纯"查版本失败"留在页面上就行，退出去再进来可以重试。
            if (s_fw_upgrading) snprintf(note, sizeof(note), "FAILED\n%s\nRebooting...", cw_ota_msg());
            else                 snprintf(note, sizeof(note), "%s\nCheck Wi-Fi and try again.", cw_ota_msg());
        } else {
            snprintf(note, sizeof(note), "%s", cw_ota_msg()[0] ? cw_ota_msg() : "Checking...");
        }
        lv_label_set_text(s_adj_note, note);
        break;
    }
    case ADJ_RESET:
        // 二次确认页：左键"复位" / 右键"取消"，默认焦点落在取消上。
        // 这一下要清掉 Wi-Fi 凭据、呼号、基站地址和全部设置并重启，手滑不得 ——
        // 所以跟 WI-FI SETUP 一样做成按钮页，而不是"选中即执行"。
        adj_btn_show("ERASE", "CANCEL", s_reset_focus);
        lv_obj_add_flag(s_adj_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_align(s_adj_value, LV_TEXT_ALIGN_CENTER, 0);
        snprintf(v, sizeof(v), "ERASE ALL?");
        lv_obj_set_style_text_align(s_adj_note, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(s_adj_note, SAFE, 152);   // 按钮在 96..142，说明紧跟着
        lv_label_set_text(s_adj_note,
                          s_reset_focus == 0
                              ? "Wi-Fi, callsign, base station\nand all settings go back to\nthe factory defaults."
                              : "Keep everything and go back.");
        break;
    case ADJ_EXIT:
        snprintf(v, sizeof(v), "OK");
        pct = 100;
        lv_label_set_text(s_adj_note, "Selected in menu: restart the device.");
        break;
    default:
        snprintf(v, sizeof(v), "%s", s_net_state == CW_NET_LINK ? "ONLINE" : "OFFLINE");
        pct = s_net_state == CW_NET_LINK ? 100 : 0;
        lv_label_set_text(s_adj_note, "Network status is read-only.");
        break;
    }
    lv_label_set_text(s_adj_title, MENU_NAME[s_sel]);
    lv_label_set_text(s_adj_value, v);
    lv_bar_set_value(s_adj_bar, pct, LV_ANIM_OFF);
}

// ===========================================================================
// 参数调整
// ===========================================================================
// ★ up = 按的是 UP 键。两类项的语义不一样，别混：
//   列表项（STEP / BL TIMEOUT）有光标，UP 就是"往上挪一格"，即选前一个档位；
//   数值项（音调 / 速度 / 音量 / 亮度）没有光标，UP 表示"调大"。
//   之前统一按"UP = +1"处理，菜单里按 UP 光标反而往下跑，是反直觉的。
static void adj_delta(bool up) {
    switch (s_sel) {
    case ADJ_TONE:
        S.tone += up ? 50 : -50;
        if (S.tone < 300) S.tone = 300;
        if (S.tone > 1200) S.tone = 1200;
        break;
    case ADJ_WPM:
        S.wpm += up ? 2 : -2;
        if (S.wpm < 10) S.wpm = 10;
        if (S.wpm > 30) S.wpm = 30;
        break;
    case ADJ_VOL:
        S.vol += up ? 5 : -5;
        if (S.vol < 0) S.vol = 0;
        if (S.vol > 100) S.vol = 100;
        vol_set();
        break;
    case ADJ_STEP: {
        // 列表：UP 往上挪一格（档位变小），到头绕到另一端的最后一行。
        int i = 0;
        for (; i < STEP_N; i++) if (STEP_OPTS[i] == S.step) break;
        if (i >= STEP_N) i = 1;
        i += up ? -1 : 1;
        if (i < 0) i = STEP_N - 1;
        if (i > STEP_N - 1) i = 0;
        S.step = STEP_OPTS[i];
        break;
    }
    case ADJ_BL:
        S.bl += up ? 10 : -10;
        if (S.bl < 10) S.bl = 10;
        if (S.bl > 100) S.bl = 100;
        // 调亮度要当场看得见变化，不能等按 OK 保存 —— 否则除非盲调。
        bsp_display_backlight((uint8_t)S.bl);
        break;
    case ADJ_BL_TO: {
        // 同样是列表：UP 往上挪一格（10s 那头），绕圈回到最后一行。
        // 若存了个不在表里的值（旧固件），落在默认档上继续调。
        int i = 0;
        for (; i < BL_TO_N; i++) if (BL_TO_OPTS[i] == S.bl_to) break;
        if (i >= BL_TO_N) i = 1;
        i += up ? -1 : 1;
        if (i < 0) i = BL_TO_N - 1;
        if (i > BL_TO_N - 1) i = 0;
        S.bl_to = BL_TO_OPTS[i];
        // 改完立刻按下新档位重算一次，省得从"常亮"切到 10s 时还得多等一轮。
        bl_note();
        break;
    }
    case ADJ_NET:
    case ADJ_WIFI:
    case ADJ_CALL:
    case ADJ_FW:
    case ADJ_RESET:
        // 只有左右两颗按钮，UP 和 DOWN 都是"移到另一颗"。
        // ★ ADJ_FW 必须列在这里：漏了它就掉进 default，FIRMWARE 页上 UP/DOWN
        //   什么都不做 —— 看着就像按键失灵（底下那行 s_fw_focus 也永远走不到）。
        // 若按 dir 定向（UP 固定左、DOWN 固定右），按住连发时会一直往同一颗上赋值、
        // 焦点纹丝不动，手感上像按键失灵。
        if (s_sel == ADJ_NET)        s_srv_focus   ^= 1;
        else if (s_sel == ADJ_WIFI)  s_wifi_focus  ^= 1;
        else if (s_sel == ADJ_CALL)  s_call_focus  ^= 1;
        else if (s_sel == ADJ_FW)    s_fw_focus    ^= 1;
        else                         s_reset_focus ^= 1;
        break;
    default:
        break;
    }
}

static void tune_delta(int dir) {
    uint32_t f = S.freq;
    if (dir < 0) f = (f > F_MIN + S.step) ? f - S.step : F_MIN;
    else f = (f + S.step < F_MAX) ? f + S.step : F_MAX;
    if (f == S.freq) return;
    S.freq = f;
    cw_net_tune(S.freq);
    // 频率是按住连发的，这里只打个"待落盘"标记，等手停下来（tick 里 SETS_IDLE_MS）
    // 再写 NVS；重启前另有强制补写，见 settings_flush。
    settings_mark_dirty();
}

// ===========================================================================
// 解码器回调：把收到的字符追加到收报区
// ===========================================================================
static void rx_emit(char ch, void *user) {
    (void)user;
    if (bsp_lvgl_lock(100)) {
        buf_push(s_rxbuf, sizeof(s_rxbuf), ch);
        if (s_scr == SCR_MAIN && s_lbl_rx) lv_label_set_text(s_lbl_rx, s_rxbuf);
        bsp_lvgl_unlock();
    }
}
static void tx_emit(char ch, void *user) {
    (void)user;
    // 本地回显走的是和收报同一套判定逻辑，日志留一行便于核对"手敲的节奏"与
    // "解出来的字符"是否对得上——点划判定出问题时这是唯一的现场证据。
    ESP_LOGI(TAG, "TX '%c'", ch);
    if (bsp_lvgl_lock(100)) {
        buf_push(s_txbuf, sizeof(s_txbuf), ch);
        if (s_scr == SCR_MAIN && s_lbl_tx) lv_label_set_text(s_lbl_tx, s_txbuf);
        bsp_lvgl_unlock();
    }
}

// ===========================================================================
// 网络回调（网络任务上下文，操作 UI 必须加锁）
// ===========================================================================
void cw_radio_on_net_state(cw_net_state_t state) {
    s_net_state = state;
    if (s_running && bsp_lvgl_lock(100)) {
        if (s_lbl_net) refresh_main();
        bsp_lvgl_unlock();
    }
}

// 发报方的 uid 在名单里吗？在的话顺便核一下频率 —— 不同频的直接丢掉。
// 服务端本来就只给同频设备转发，这一层是防"名单滞后"：presence 有 150ms 节流，
// 我刚转走的那一下，服务端手里的还是旧频率，会多转一两个元素过来。
// 查不到（名单里没有这个 uid）时放行 —— 宁可多收一个元素，也不能把正常通联掐了。
static bool rx_from_ok(uint16_t from_uid) {
    if (!from_uid) return true;                 // 老服务端不下发 uid，无从判断
    for (int i = 0; i < s_roster_n; i++) {
        if (s_roster[i].uid != from_uid) continue;
        return same_channel(S.freq, s_roster[i].freq);
    }
    return true;
}

void cw_radio_on_rx_key(int on, int pitch_hz, int s, int wpm, int flags, uint16_t from_uid) {
    if (!s_online) return;      // 离线：一个码都不收（网络层照收，只是不当回事）
    // ★ 只有同频的信号才进 RX：收报区、解码器、喇叭都得跟着这条走，
    //   否则屏幕上会出现"隔壁频道在说话"的字符，用户还以为收到了。
    if (!rx_from_ok(from_uid)) return;
    // 半双工：自己在发报时闭掉收听，否则自己的侧音会被别人收回去形成回声。
    int32_t now = (int32_t)(esp_timer_get_time() / 1000);
    s_rx_key_ms = now;                  // 收报长音兜底用（见 tick_cb）
    bl_note();                  // 别人在发报也算活动：熄着屏却有人在呼叫就漏了
    s_rx_s = s;

    // 点长以发报方的速度为准；只在速度真的变化时重建解码器，否则会打断正在收的字符。
    int want = wpm > 0 ? wpm : S.wpm;
    if (want != s_dec_wpm) {
        cw_decoder_init(&s_dec, cw_morse_dit_ms(want), rx_emit, NULL);
        s_dec_wpm = want;
    }

    if (on && !s_tx_on) {
        s_rx_pitch = pitch_hz;
        s_rx_on = true;
    } else {
        s_rx_on = false;
    }
    if (!s_running) return;

    if (flags & CW_KEY_FLAG_CANCEL) {
        // 对方长按 OK 进菜单了：他发出的那一段不算数，直接丢掉半个的元素。
        cw_decoder_reset(&s_dec, now);
        return;
    }
    if (!s_tx_on) cw_decoder_feed(&s_dec, on, now);
}

void cw_radio_on_roster(const char *text) {
    // 格式："CALL,freq,uid;CALL,freq,uid;..."（uid 是后加的，老服务端只有前两字段，
    // 那时 uid 记 0，roster_is_me() 会退回按呼号比对。）
    int n = 0;
    const char *p = text;
    while (*p && n < ROSTER_MAX) {
        const char *semi = strchr(p, ';');
        size_t len = semi ? (size_t)(semi - p) : strlen(p);
        const char *comma = memchr(p, ',', len);
        if (comma) {
            size_t cl = (size_t)(comma - p);
            if (cl > 8) cl = 8;
            memcpy(s_roster[n].call, p, cl);
            s_roster[n].call[cl] = '\0';
            s_roster[n].freq = (uint32_t)strtoul(comma + 1, NULL, 10);
            const char *rest = comma + 1;
            const char *comma2 = memchr(rest, ',', len - (size_t)(rest - p));
            s_roster[n].uid = comma2 ? (uint16_t)strtoul(comma2 + 1, NULL, 10) : 0;
            n++;
        }
        if (!semi) break;
        p = semi + 1;
    }
    s_roster_n = n;
    if (s_running && bsp_lvgl_lock(100)) {
        if (s_scr == SCR_MAIN) { refresh_main(); draw_spectrum(); }   // 台站竖线跟着名单走
        bsp_lvgl_unlock();
    }
}

void cw_radio_on_occupy(const uint8_t *bins, int n) {
    if (!bins || n <= 0) return;
    if (n > (int)sizeof(s_bins)) n = (int)sizeof(s_bins);
    memcpy(s_bins, bins, (size_t)n);
    s_bins_n = n;
    if (s_running && bsp_lvgl_lock(100)) {
        if (s_scr == SCR_MAIN) draw_spectrum();
        bsp_lvgl_unlock();
    }
}

void cw_radio_on_time(int64_t unix_ms) { s_unix_ms = unix_ms; }

void cw_radio_on_chat(const char *from, const char *msg) {
    snprintf(s_chat, sizeof(s_chat), "%s: %s", from, msg);
    if (s_running && bsp_lvgl_lock(100)) {
        if (s_scr == SCR_MAIN) refresh_main();
        bsp_lvgl_unlock();
    }
}

// ===========================================================================
// 屏幕切换
// ===========================================================================
// C3 无 PSRAM，LVGL 堆只有几十 KB。三个满屏同时建会撑爆 lv_realloc（失败后
// LV_ASSERT_NULL 直接死循环，表现为 main 任务卡死触发看门狗）。所以只建当前要用的：
// 主屏常驻，菜单/参数页首次进入时才建，之后保留。
static void ensure_scr(scr_t s) {
    if (s == SCR_MENU && !s_menu) build_menu();
    else if (s == SCR_ADJ && !s_adj) build_adj();
}

static void go(scr_t s) {
    s_scr = s;
    ensure_scr(s);
    if (s == SCR_MAIN) { refresh_main(); refresh_battery(); draw_spectrum(); lv_screen_load(s_main); }
    else if (s == SCR_MENU) { refresh_menu(); lv_screen_load(s_menu); }
    else {
        S_bak = S;
        if (s_sel == ADJ_WIFI) s_wifi_focus = 1;   // 每次进确认页焦点都先落在"取消"上
        if (s_sel == ADJ_NET)  s_srv_focus = 1;    // 基站页则先落在"确认"上
        if (s_sel == ADJ_RESET) s_reset_focus = 1; // 复位页同样先落在"取消"上
        if (s_sel == ADJ_CALL) s_call_focus = 1;   // 呼号页同样先落在"取消"上
        if (s_sel == ADJ_FW) {                     // 固件页：重新查一次服务器版本
            s_fw_focus = 1;
            s_fw_query_sent = false;
            s_fw_upgrading = false;
            s_fw_done_ms = 0;
        }
        refresh_adj();
        lv_screen_load(s_adj);
    }
}

// ===========================================================================
// 按键
// ===========================================================================
// 进/出菜单，由长按 OK 触发。
static void menu_toggle(int64_t now) {
    if (s_scr == SCR_MAIN) {
        // ★ 进菜单只在离线时有效。在线时 OK 键就是电键，按多久都只是一记长划，
        //   不能把它抢去当菜单键 —— 要进菜单先按【UP+DOWN】切离线，再长按 OK。
        //   反向（菜单 → 主屏）不受这条限制，否则进得去出不来。
        if (s_online) return;
        if (s_tx_on) {
            // 正在敲的那一下不算发报，通知服务端丢弃这个元素。
            tx_stop();
            cw_net_send_key(0, CW_KEY_FLAG_CANCEL, now);
        }
        cw_decoder_reset(&s_txdec, (int32_t)now);   // 本地攒了一半的字符也丢掉
        // 每次进菜单都从第一项、列表顶部开始：上次停在 REBOOT 那几行时
        // 列表是滚下去的，不归位的话一进来看到的是半截列表。
        s_sel = ADJ_TONE;
        s_menu_scroll = 0;
        if (s_menu_list) lv_obj_scroll_to_y(s_menu_list, 0, LV_ANIM_OFF);
        go(SCR_MENU);
        return;
    }
    if (s_scr == SCR_MENU) { go(SCR_MAIN); return; }
    // 参数页：组合键 = 放弃改动
    S = S_bak;
    if (s_sel == ADJ_VOL) vol_set();
    if (s_sel == ADJ_BL) bsp_display_backlight((uint8_t)S.bl);   // 调亮度时实时改过，得还原
    // 超时只是个数值，S=S_bak 已经还原；但调的过程中屏幕可能已经按新档位熄了，
    // 这里按还原后的档位重新点亮并计时（bl_wake 兼记活动时间）。
    bl_wake();
    go(SCR_MENU);
}

static void on_ok_release(int64_t dur, int64_t now) {
    if (s_scr == SCR_MAIN) {
        if (dur >= MENU_HOLD_MS) { tx_stop(); menu_toggle(now); return; }
        tx_up(now);                         // 只是把熄音时刻排进去，声音照时刻走
        cw_net_send_key(0, 0, now);         // 离线时这一句自己会拦掉，不发
        // ★ 交给常驻的发报解码器：它会在静默满 3 个点长时才收字符，
        //   所以 "点 划" 能攒成 A，而不是各自变成 E 和 T。
        //   离线练习也要喂：屏幕上的 TX 行照样出字，只是没发出去。
        cw_decoder_feed(&s_txdec, 0, (int32_t)now);
        return;
    }
    if (s_scr == SCR_MENU) {
        if (dur >= MENU_HOLD_MS) { menu_toggle(now); return; }
        // 全局的"长按 OK 退出 demo"已被 main.c 屏蔽（它的阈值会和本页的
        // MENU_HOLD_MS 长按打架），所以这里必须自己留一个离开 CW 的口子。
        // 重启前把当前频率补写进 NVS：菜单里常是"转到一个频点 → 发现要重启 → REBOOT"，
        // 防抖那 2 秒还没到，不补写这一下上次的位置就丢了。
        if (s_sel == ADJ_EXIT) { settings_flush(); esp_restart(); }   // 不返回
        if (s_sel == ADJ_NET) { go(SCR_ADJ); return; }  // 基站页：看地址 / 改地址
        go(SCR_ADJ);                                    // 含 ADJ_WIFI：先过确认页
        return;
    }
    // 参数页：短按=保存，长按=放弃
    if (dur >= MENU_HOLD_MS) { menu_toggle(now); return; }
    if (s_sel == ADJ_NET) {
        if (s_srv_focus == 0) {             // 左键"更改"：重启进配网，只改服务器地址
            settings_flush();               // 频率别丢在这道重启上
            cw_prov_request_server();
            esp_restart();                  // 不返回
        }
        go(SCR_MENU);                       // 右键"确认"：地址就这样，回菜单
        return;
    }
    if (s_sel == ADJ_WIFI) {
        if (s_wifi_focus == 0) {            // 焦点在左键"确认"：置标志后重启进配网
            settings_flush();               // 同上：进配网前先把频率存下来
            cw_prov_request();
            esp_restart();                  // 不返回
        }
        go(SCR_MENU);                       // 焦点在右键"取消"：就这么回去
        return;
    }
    if (s_sel == ADJ_CALL) {
        if (s_call_focus == 0) {        // 左键"更新"：重启进配网，页面上只有 Ham 呼号
            settings_flush();           // 频率别丢在这道重启上
            cw_prov_request_call();
            esp_restart();              // 不返回
        }
        go(SCR_MENU);                   // 右键"取消"：两个呼号都不动
        return;
    }
    if (s_sel == ADJ_FW) {
        // 左键的含义跟着页面走：有新版时是"升级"，没有（或没查到）时是"重试"。
        // 判断条件和 refresh_adj 里画按钮的那个一致，免得屏幕上写着 RETRY、
        // 按下去却开始下载。
        bool have_new = !cw_ota_busy() && cw_ota_state() == CW_OTA_IDLE &&
                        cw_ota_newver()[0] &&
                        cw_ota_ver_newer(cw_ota_newver(), CW_FW_VERSION);
        if (s_fw_focus == 0 && !have_new) {     // 左键"重试"
            cw_ota_reset();             // 失败后状态停在 ERR，不清掉查询起不来
            s_fw_query_sent = false;    // 让 refresh_adj 重新发一次查询
            s_fw_focus = 1;             // 焦点回到"取消"上，别手滑连按两次
            refresh_adj();
            return;                     // 留在这一页等新结果
        }
        if (s_fw_focus == 0) {          // 左键"升级"：先把设置落盘，再静音下载
            settings_flush();           // 频率别丢在这道重启上
            // ★ 必须先停音频：写 Flash 会关 cache，那段时间内音频任务喂不上 PCM，
            //   DMA 放空就是一声爆响（和菜单里落盘时躲开播放期是同一个道理）。
            //   反正升级完要重启，不用再把它拉起来。
            audio_stop_now();
            s_fw_upgrading = true;
            cw_ota_upgrade_start();
            return;                     // 留在这一页看进度
        }
        go(SCR_MENU);                   // 右键"取消"：什么都不动
        return;
    }
    if (s_sel == ADJ_RESET) {
        if (s_reset_focus == 0) {       // 左键"复位"：清 NVS 后重启
            // 先放掉正在敲的那一下（离线时 cw_net_send_key 自己会拦掉，无害），
            // 免得服务端那边留着一个 keydown，别人听到的是长音。
            tx_stop();
            cw_net_send_key(0, CW_KEY_FLAG_CANCEL, now);
            // 重启后 NVS 空空：settings_load 一项都读不到，全部回到出厂默认
            // （离线 / 700 Hz / 18 WPM / 50% / 100 Hz / 亮度 50% / 超时 30 s），
            // 基站是 None、Wi-Fi 凭据为空 —— 于是开机先进配网（长按 OK 可跳过）。
            cw_prov_factory_reset();
            esp_restart();              // 不返回
        }
        go(SCR_MENU);                   // 右键"取消"：什么都不动
        return;
    }
    if (s_sel == ADJ_ABOUT) {           // 没有可保存的状态：看完直接回菜单
        go(SCR_MENU);
        return;
    }
    if (s_sel == ADJ_WPM) {
        cw_net_set_wpm(S.wpm);
        // 速度变了，点划判定基准必须跟着变；只重建发报解码器，收报的另有来源速度。
        cw_decoder_init(&s_txdec, cw_morse_dit_ms(S.wpm), tx_emit, NULL);
    }
    if (s_sel == ADJ_VOL) vol_set();
    // 短按 OK = 确认：整份设置（含亮度、超时）一起落盘，下次开机就是这个值。
    // 只有 NET / WIFI / ABOUT 在上面已经 return 了，走到这儿的都是可调项。
    // ★ 只发请求：按键派发是在 LVGL 锁里跑的，那一会儿绝不能写 Flash
    //   （关 cache 会掐断 PCM 供给 → 正在响的收报音爆一下）。
    //   参数页里 UP/DOWN 改的是 S 本身、不打脏标记，这里先补上；
    //   到底写不写由 settings_write 拿 S 和上次那份比对后决定（没动过就不写）。
    settings_mark_dirty();
    settings_request();
    go(SCR_MENU);
}

// 撤销"组合键第一颗键已经做过的那个动作"。
// 两键同按必然有先后：先落下的那颗（电键起振 / 频率跳一格 / 菜单光标挪一位）在组合
// 成立时已经生效了。不退回去的话，每切一次在线离线都会顺手发一个点、或把频率顶偏。
// 只在 500ms 内成立 —— 超过这个间隔说明是两次独立操作，不该互相抵消。
static void undo_last_key(int64_t now) {
    if (now - s_pre_ms > 500) return;
    if (s_tx_on && !s_pre_tx) {                 // 电键已经起振：立刻松键并丢掉半个元素
        tx_stop();
        cw_net_send_key(0, CW_KEY_FLAG_CANCEL, now);
        cw_decoder_reset(&s_txdec, (int32_t)now);
    }
    settings_t old = S;
    S = s_pre_S;
    if (S.vol != old.vol) vol_set();
    if (S.bl  != old.bl)  bsp_display_backlight((uint8_t)S.bl);
    if (S.wpm != old.wpm) {
        cw_decoder_init(&s_dec, cw_morse_dit_ms(S.wpm), rx_emit, NULL);
        cw_decoder_init(&s_txdec, cw_morse_dit_ms(S.wpm), tx_emit, NULL);
        s_dec_wpm = S.wpm;
    }
    if (S.freq != old.freq) { cw_net_tune(S.freq); draw_spectrum(); }
    if (s_sel != s_pre_sel) s_sel = s_pre_sel;
    if (s_scr == SCR_MAIN)      refresh_main();
    else if (s_scr == SCR_MENU) refresh_menu();
    else                        refresh_adj();
}

// 组合键成立：在线 <-> 离线。
static void toggle_online(int64_t now) {
    // 正在发报时切离线，得先把键控放掉再下线 —— cw_net_send_key 在离线后是空操作，
    // 顺序反了服务端那边会一直留着一个 keydown，别人听到的是长音。
    if (s_online && s_tx_on) {
        tx_stop();
        cw_net_send_key(0, CW_KEY_FLAG_CANCEL, now);
        cw_decoder_reset(&s_txdec, (int32_t)now);
    }
    s_online = !s_online;
    cw_net_set_online(s_online);
    if (!s_online) { s_rx_on = false; s_rx_pitch = 0; }   // 离线：立刻闭掉收听
    refresh_online();
    if (s_scr == SCR_MAIN) refresh_main();
}

// 组合键的事件过滤。物理上是【下+确定】（唯一能区分的并联档），经过 main.c 的重排
// 后对应逻辑 UP + DOWN —— 也就是说，现在的 UP+DOWN 两键同按就能切在线/离线。
// 返回 true = 这个事件已经被吃掉，不再往下分发。
//
// 为什么要这一层：一路 ADC 只能报一个键，两键同按时上报的是第 4 档电压（COMBO），
// 于是事件序列是"单键按下 → COMBO 按下 → COMBO 抬起 → 单键抬起"的夹心结构；
// 中间那两个单键事件若不处理，松开组合键就会变成一次发报或一次跳频。
static bool combo_filter(bsp_btn_t btn, bsp_btn_ev_t ev, int64_t now) {
    if (btn != BSP_BTN_COMBO && btn != BSP_BTN_UP && btn != BSP_BTN_DOWN) return false;
    if (ev != BSP_BTN_PRESS && ev != BSP_BTN_RELEASE) return false;
    s_cmb_ms = now;

    if (ev == BSP_BTN_PRESS) {
        s_cmb_down[btn] = true;
        if (btn != BSP_BTN_COMBO) {
            if (!s_cmb) return false;          // 组合还没成立，这就是一次普通单键按下
            s_press_ms[btn] = 0;               // 组合进行中：单键不生效，也不许判长按
            s_down[btn] = false;
            s_repeat_btn = -1;
            return true;
        }
        s_press_ms[btn] = 0;
        s_down[btn] = false;
        if (s_cmb) return true;                // 同一次组合只触发一次
        s_cmb = true;
        // 组合档与单按 DOWN 只差几十 mV，把实测电压打出来，方便日后核对窗口是否被
        // 电阻公差/ADC 漂移顶穿（正常应在 212mV 附近）。
        ESP_LOGI(TAG, "组合键按下，ADC %d mV（期望 ~212；单按 DOWN 是 ~300）",
                 bsp_button_read_mv());
        // 组合里的键一律不许参与"长按进菜单"和连发：按住电键凑组合会被误判成长按。
        s_press_ms[BSP_BTN_UP] = s_press_ms[BSP_BTN_DOWN] = 0;
        s_down[BSP_BTN_UP] = s_down[BSP_BTN_DOWN] = false;
        s_repeat_btn = -1;
        undo_last_key(now);
        toggle_online(now);
        return true;
    }

    // 抬起
    s_cmb_down[btn] = false;
    const bool was = s_cmb;
    if (!s_cmb_down[BSP_BTN_COMBO] && !s_cmb_down[BSP_BTN_UP] && !s_cmb_down[BSP_BTN_DOWN]) {
        s_cmb = false;                          // 三档全松开，这次组合结束
    }
    if (btn == BSP_BTN_COMBO) { s_press_ms[btn] = 0; s_down[btn] = false; return true; }
    if (was) {                                  // 组合期间补发的单键抬起，一并吞掉
        s_press_ms[btn] = 0;
        s_down[btn] = false;
        s_repeat_btn = -1;
        return true;
    }
    return false;                               // 组合压根没成立过，照常处理
}

// OK 按下：起振并立刻发 keydown，不加任何延迟。
// 单独抽出来，是因为抬起时若发现按下事件没派发到，要按"一次极短的按下+抬起"补上，
// 不能白丢一个点 —— 但补出来的那次绝不能算长按。
static void ok_press(int64_t now) {
    if (s_scr != SCR_MAIN) return;
    // 离线也照样起振：那是"练习"——侧音和本地解码都走，只是不发到网上。
    // （在线/离线的区别只在下面那一句 cw_net_send_key，它自己会拦掉离线的帧。）
    tx_down(now);
    cw_net_send_key(1, 0, now);
    cw_decoder_feed(&s_txdec, 1, (int32_t)now);
}

// 真正的按键处理。运行在 s_key_task（已持有 LVGL 锁），可以放心碰 UI 和发网络包。
static void key_dispatch(bsp_btn_t btn, bsp_btn_ev_t ev, int64_t now) {
    if (!s_running) return;

    // 屏幕熄着的时候，第一次按键只管点亮：不进菜单、不改参数、不发报。
    // 摸黑按一下本意是"看清屏幕"，若顺手就把设置改了（REBOOT 那项甚至直接重启），
    // 代价太大。点亮之后抬起也要一起吞掉，否则松手会补一次动作。
    if (s_bl_dim) {
        bl_wake();
        s_press_ms[btn] = 0;            // 别让 tick 误判成"OK 一直按着"
        s_repeat_btn = -1;
        s_down[btn] = false;
        if (ev == BSP_BTN_PRESS) s_wake_only = true;
        return;
    }
    if (s_wake_only) {
        if (ev == BSP_BTN_RELEASE) { s_wake_only = false; s_press_ms[btn] = 0; s_down[btn] = false; }
        return;
    }

    // 组合键（在线/离线开关）先过一道：吃掉夹在中间的单键事件，自己也不往下走。
    if (combo_filter(btn, ev, now)) return;

    bl_wake();                  // 任何按键都算活动

    if (ev == BSP_BTN_PRESS) {
        s_press_ms[btn] = now;
        s_down[btn] = true;
        // 记下现场：万一紧接着按下另一颗键凑成组合，要把这次的动作退回去。
        s_pre_S = S; s_pre_sel = s_sel; s_pre_tx = s_tx_on; s_pre_ms = now;
        if (btn == BSP_BTN_OK) {
            ok_press(now);
            return;
        }
        // UP / DOWN：先响应一次，保证单步手感即时
        bool up = (btn == BSP_BTN_UP);
        if (s_scr == SCR_MENU) {
            s_sel += up ? -1 : 1;               // 菜单行自上而下排，UP 就是往上挪一格
            if (s_sel < 0) s_sel = ADJ_COUNT - 1;
            if (s_sel >= ADJ_COUNT) s_sel = 0;
            refresh_menu();
        } else if (s_scr == SCR_ADJ) {
            adj_delta(up);
            refresh_adj();
        } else {
            // 主屏没有光标，UP 按"频率升高"理解 —— 跟旋钮一个方向。
            tune_delta(up ? 1 : -1);
            refresh_main();
            draw_spectrum();
        }
        s_repeat_btn = (int)btn;
        s_repeat_start = now;
        return;
    }

    if (ev == BSP_BTN_RELEASE) {
        s_repeat_btn = -1;
        s_down[btn] = false;
        if (btn == BSP_BTN_OK) {
            int64_t dur;
            if (s_press_ms[btn] > 0) {
                dur = now - s_press_ms[btn];
            } else {
                // 按下那一下没派发到（抢锁超时被丢弃，或队满时被挤掉）。
                // 此时 s_press_ms 是 0，拿它算时长会得到"开机至今"，必然超过
                // MENU_HOLD_MS —— 这就是"只按一下 OK 却跳进菜单"的根因。
                // 真实时长无从得知，按极短的一次处理：先补上按下（保住这个点），
                // 时长记 0，绝不做长按动作。
                ESP_LOGW(TAG, "OK 抬起时没有配对的按下，按短按处理");
                ok_press(now);
                dur = 0;
            }
            s_press_ms[btn] = 0;
            // 长按在 tick 里已经自动处理过了（进/出菜单），松手这一次不再算数；
            // 否则"按住进菜单 + 松手"会立刻又退出菜单。
            if (s_ok_hold_done) { s_ok_hold_done = false; return; }
            on_ok_release(dur, now);
            return;
        }
        s_press_ms[btn] = 0;
        return;
    }
    // CLICK / DOUBLE / LONG 一律忽略：本页所有语义都由 PRESS+RELEASE 的时长定义，
    // 混用两套时基会让"长按进菜单"和"长按返回 demo 菜单"互相打架。
}

// 按键回调：esp_timer 任务上下文，只许做"记时刻 + 入队"这两件必定有界的事。
// 抬起事件插到队首 —— 宁可丢一次按下，也绝不能丢抬起，否则连发标志清不掉，
// 频率会一直自己滚下去（这正是"按 UP/DOWN 会卡住"的直接原因）。
// 电键时序的快车道。由 main.c 的按键回调（esp_timer 任务上下文）同步调用，
// 比 cw_radio_key 更早一步 —— 后者还要过输入队列和 LVGL 锁。
// 这里只写几个时刻变量，不碰 LVGL、不发网络包，可以安全地放在回调里。
// 声音的起落在这里就定死了：派发链路再堵，也只是屏幕上的字符晚一点出，
// 点划的节奏不受影响。其余语义（解码、发报、进菜单）仍然走 cw_radio_key。
void cw_radio_key_edge(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (btn != BSP_BTN_OK) return;
    // 只在主屏发声；菜单/参数页里 OK 是导航键。熄屏那一下也照旧只管点亮。
    if (!s_running || s_scr != SCR_MAIN || s_bl_dim) return;
    int64_t now = esp_timer_get_time() / 1000;
    if (ev == BSP_BTN_PRESS) tx_down(now);
    else if (ev == BSP_BTN_RELEASE) tx_up(now);
}

void cw_radio_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (!s_keyq || !s_running) return;
    key_ev_t e = { .btn = (uint8_t)btn, .ev = (uint8_t)ev, .t_ms = esp_timer_get_time() / 1000 };

    // 按下/抬起一律走队尾，保序 —— 之前把抬起插到队首，快速点划时抬起会跑到按下
    // 前面去，按下时刻还没记上就拿它算时长，于是"只按一下"也能算出个超长的按住。
    // 真到队满时丢最老的一条给新事件腾位置（32 格，实际上到不了），抬起照样进得来。
    if (xQueueSend(s_keyq, &e, 0) != pdTRUE) {
        key_ev_t drop;
        xQueueReceive(s_keyq, &drop, 0);
        xQueueSend(s_keyq, &e, 0);
    }
}

static void key_task(void *arg) {
    (void)arg;
    key_ev_t e;
    while (s_key_run) {
        if (xQueueReceive(s_keyq, &e, pdMS_TO_TICKS(200)) != pdTRUE) continue;
        if (!s_running) continue;
        // 真正在下载固件时：按键一概不响应。下载要连着写几秒 Flash，这期间切页面、
        // 改设置都可能跟写槽抢资源；而且屏幕正显示进度，用户也不该有别的操作。
        // ★ 只锁"下载"，不锁"查版本"：那一趟只是个几 KB 的 HTTP GET，
        //   锁上它等于进页就卡住按键若干秒（服务器连不上时更久），手感就是失灵。
        if (s_fw_upgrading && cw_ota_busy()) continue;
        // 所有 UI 操作都要在锁内；拿不到就丢弃本次动作，绝不在按键路径上硬等。
        if (bsp_lvgl_lock(200)) {
            key_dispatch((bsp_btn_t)e.btn, (bsp_btn_ev_t)e.ev, e.t_ms);
            bsp_lvgl_unlock();
        } else if (e.ev == BSP_BTN_RELEASE) {
            // 动作可以丢，状态不能卡：抬起丢了的话 tick 会以为 OK 一直按着，
            // MENU_HOLD_MS 后照样自动进菜单。这里把按住状态强制清掉兜底。
            s_repeat_btn = -1;
            s_down[e.btn] = false;
            s_press_ms[e.btn] = 0;
            // 电键的抬起尤其不能丢：它的熄音时刻就靠这一下排进去。丢了的话
            // s_tx_on 一直是真，喇叭长鸣不止，服务端那边也留着一个永不结束的
            // keydown —— 别人听到的也是一条长音。这里按当时记下的时刻补上。
            if (e.btn == BSP_BTN_OK && s_scr == SCR_MAIN) {
                tx_up(e.t_ms);
                cw_net_send_key(0, 0, e.t_ms);
                cw_decoder_feed(&s_txdec, 0, (int32_t)e.t_ms);
            }
        }
    }
    s_key_task = NULL;
    vTaskDelete(NULL);
}

// ===========================================================================
// LVGL 定时器：连发 + 周期刷新
// ===========================================================================
static void repeat_cb(lv_timer_t *t) {
    (void)t;
    if (!s_running || s_repeat_btn < 0) return;
    int64_t now = esp_timer_get_time() / 1000;
    if (now - s_repeat_start < REPEAT_DELAY_MS) return;
    bl_note();                  // 连发也算活动，一直滚屏不该把背光滚灭了
    bool up = (s_repeat_btn == (int)BSP_BTN_UP);
    if (s_scr == SCR_MENU) {
        s_sel += up ? -1 : 1;               // 与单步同一方向：UP 往上
        if (s_sel < 0) s_sel = ADJ_COUNT - 1;
        if (s_sel >= ADJ_COUNT) s_sel = 0;
        refresh_menu();
    } else if (s_scr == SCR_ADJ) {
        adj_delta(up);
        refresh_adj();
    } else {
        tune_delta(up ? 1 : -1);
        refresh_main();
        draw_spectrum();
    }
}

// 字符收尾只靠这一个轻量定时器驱动，不跟着 100ms 的 UI 刷新走：
// 30 WPM 的字符间隔只有 120ms，用 100ms 粒度去卡它，收尾时刻会有近一个间隔的抖动。
static void dec_tick_cb(lv_timer_t *t) {
    (void)t;
    if (!s_running) return;
    int32_t now = (int32_t)(esp_timer_get_time() / 1000);
    cw_decoder_tick(&s_dec, now);       // 收报：网络来的码
    cw_decoder_tick(&s_txdec, now);     // 发报：自己手敲的码，够 3 个点长静默就收字符
}

static int s_bat_div;
static void tick_cb(lv_timer_t *t) {
    (void)t;
    if (!s_running) return;
    int64_t now = esp_timer_get_time() / 1000;
    bl_tick(now);                           // 背光超时任何页面都要管，得放在下面那个 return 之前

    // 防抖落盘：改完频率手停下来 SETS_IDLE_MS 就写一次 NVS，不留到下次开机才存。
    // 只发请求，真正的下笔在落盘任务里等安全播放边界做（见 save_task）。
    if (s_sets_dirty && now - s_sets_dirty_ms >= SETS_IDLE_MS) settings_request();
    vol_flush();                // 改过的音量挑个不出声的时刻写进 codec

    // 并发播放的体检报告：供给间隔 / 重绘耗时 / 落盘耗时，每 AUDIO_STAT_MS 一行。
    // 改音效或音量之前先看这组数 —— 日志没告警不代表并发播放不出杂音，
    // 只有"供给间隔远小于 DMA 能撑的时长"才是真的安全。
    if (s_stat_ms == 0) s_stat_ms = (uint32_t)now;
    else if ((uint32_t)now - s_stat_ms >= AUDIO_STAT_MS) {
        ESP_LOGI(TAG, "音频体检: 供给间隔最大 %u ms · 存货放空 %u 次 · %u 块/10s · "
                      "write 阻塞最大 %u ms · 让出最大 %u ms · 重绘最大 %u ms · 落盘最大 %u ms · 写失败 %u",
                 (unsigned)s_pcm_gap_ms_max, (unsigned)s_pcm_underrun, (unsigned)s_blocks,
                 (unsigned)s_write_ms_max, (unsigned)s_sleep_ms_max,
                 (unsigned)s_draw_ms_max,
                 (unsigned)s_save_ms_max, (unsigned)s_pcm_write_fail);
        s_pcm_gap_ms_max = 0;
        s_pcm_underrun = 0;
        s_blocks = 0;
        s_write_ms_max = 0;
        s_sleep_ms_max = 0;
        s_draw_ms_max = 0;
        s_save_ms_max = 0;
        s_stat_ms = (uint32_t)now;
    }

    // 组合键兜底：万一某颗键的抬起事件丢了（队满被挤掉、或抢锁超时），s_cmb 会一直
    // 卡住，三个键从此按不动。3 秒没有组合相关动作就强制复位。
    if (s_cmb && now - s_cmb_ms > 3000) {
        s_cmb = false;
        for (int i = 0; i < BSP_BTN_COUNT; i++) s_cmb_down[i] = false;
    }

    // 长按 OK 到阈值就自动进/出菜单，不用等松手：按住的时候屏幕上有"正在发报"的
    // 提示，等到抬手才切换，用户会以为没反应。s_ok_hold_done 保证只触发一次，
    // 并且让 key_dispatch 吞掉随后的那次抬起。
    // ★ 组合期间不算：按住电键凑组合时不能被当成"长按进菜单"。
    // ★ 在线时不算：那时 OK 是电键，长按就该是一记长划。若照样置 s_ok_hold_done，
    //   松手会被吞掉、keyup 帧发不出去，服务端那边就留下一个永不结束的长音。
    //   只有在"主屏 → 菜单"这个方向上要求离线；菜单/参数页里长按 OK 仍是"返回"，
    //   那条路任何时候都得留着，否则进得去出不来。
    if (!s_ok_hold_done && !s_cmb && (!s_online || s_scr != SCR_MAIN) &&
        s_down[BSP_BTN_OK] && s_press_ms[BSP_BTN_OK] > 0 &&
        now - s_press_ms[BSP_BTN_OK] >= MENU_HOLD_MS) {
        s_ok_hold_done = true;
        menu_toggle(now);
    }

    // —— 长音卡住的兜底 ——
    // ① 自己这边：抬起事件丢了（队满被挤、抢锁超时、ADC 边沿漏检），s_tx_on 就一直
    //    是真，喇叭长鸣。到点强制闭音并补一个 keyup，别让服务端和别人家的喇叭跟着响。
    if (s_tx_on && s_tx_off_ms == 0 && now - s_tx_down_ms > TONE_MAX_MS) {
        ESP_LOGW(TAG, "电键按下已 %lld ms，强制闭音（抬起事件丢了？）",
                 (long long)(now - s_tx_down_ms));
        tx_stop();
        cw_net_send_key(0, CW_KEY_FLAG_CANCEL, now);
        cw_decoder_reset(&s_txdec, (int32_t)now);
    }
    // ② 对方那边：keydown 收到了、keyup 的 UDP 帧丢了，本地也会一直响下去。
    //    正常发报的键控帧是几十一百毫秒一帧，2 秒没有任何动静只能是对面卡了。
    uint32_t now32 = ms32();
    if (s_rx_on && (int32_t)(now32 - s_rx_key_ms) > RX_TONE_MAX_MS) {
        ESP_LOGW(TAG, "收报音持续 %ld ms 没有新键控帧，强制闭音",
                 (long)(int32_t)(now32 - s_rx_key_ms));
        s_rx_on = false;
        s_rx_pitch = 0;
        cw_decoder_reset(&s_dec, (int32_t)now);
    }

    // —— 固件升级：升完（或失败）自动重启 ——
    // ★ 收尾不看当前在哪一页：升级也可能由菜单之外触发（比如聊天里的远程指令），
    //   那时若只有 FIRMWARE 页才重启，镜像已经写进另一个槽却永远不切过去，
    //   设备会一直跑着旧固件 —— 比重启更糟。
    if (s_fw_upgrading) {
        cw_ota_state_t st = cw_ota_state();
        if (!cw_ota_busy() && (st == CW_OTA_OK || st == CW_OTA_ERR)) {
            // 成功 2 秒后重启；失败留 4 秒让人看清原因再重启 —— 反正音频已经停了，
            // 重启才能把侧音拉回来。
            if (!s_fw_done_ms) s_fw_done_ms = now32;
            if (now32 - s_fw_done_ms >= (st == CW_OTA_OK ? 2000u : 4000u)) {
                settings_flush();
                esp_restart();                                  // 不返回
            }
        }
        if (cw_ota_busy()) bl_wake();   // 下载可能比背光超时还久，别让屏幕熄了
    }
    // 进度条与状态文字靠这一下往前走（只有正停在 FIRMWARE 页时才需要）
    if (s_scr == SCR_ADJ && s_sel == ADJ_FW) refresh_adj();

    if (s_scr != SCR_MAIN) return;
    if (++s_bat_div >= 20) { s_bat_div = 0; refresh_battery(); }
    if (s_rx_pitch && !s_rx_on) s_rx_pitch = 0;
    uint32_t d0 = (uint32_t)(esp_timer_get_time() / 1000);
    refresh_main();                     // 每 100ms 都刷，否则正在敲的点划不会显示
    uint32_t dd = (uint32_t)(esp_timer_get_time() / 1000) - d0;
    if (dd > s_draw_ms_max) s_draw_ms_max = dd;
}

// ===========================================================================
// demo 生命周期
// ===========================================================================
void cw_radio_enter(void) {
    s_running = true;
    s_scr = SCR_MAIN;
    s_sel = ADJ_TONE;
    s_txbuf[0] = '\0';
    s_rxbuf[0] = '\0';
    s_chat[0] = '\0';
    s_roster_n = 0;
    s_bins_n = 0;
    tx_stop();
    s_rx_on = false;
    s_rx_key_ms = ms32();
    s_repeat_btn = -1;
    s_rx_pitch = 0;
    s_wifi_focus = 1;
    s_srv_focus = 1;
    s_reset_focus = 1;
    s_call_focus = 1;
    s_wake_only = false;
    s_ok_hold_done = false;
    s_cmb = false;
    s_online = false;                // 开机即离线：要通联先按【UP+DOWN】（外圈变绿）
    for (int i = 0; i < BSP_BTN_COUNT; i++) { s_press_ms[i] = 0; s_down[i] = false; s_cmb_down[i] = false; }

    // 背光亮度与超时都从 NVS 恢复；没存过就用默认值。开机先点亮，超时交给 tick 管。
    uint8_t bl = 0;
    if (cw_prov_load_bl(&bl) && bl >= 10 && bl <= 100) S.bl = (int)bl;
    uint8_t to = 0;
    if (cw_prov_load_blto(&to) && cw_blto_valid((int)to)) S.bl_to = (int)to;
    // 频率、步进、侧音、音量、速度同样从 NVS 取回上次的值（逐项校验，越界就用默认）。
    settings_load();
    s_sets_dirty = false;                   // 刚读回来的就是已落盘的状态，不用回写
    S_saved = S;                            // 同上：把它当作"上次写进去的那份"
    S_saved_valid = true;
    // NVS 分区只有 24KB，条目用满后每次 commit 都要回收扇区，落盘会变慢 ——
    // 开机报一次，落盘耗时异常时好对照。
    cw_nvs_stats_t ns;
    if (cw_prov_nvs_stats(&ns)) {
        ESP_LOGI(TAG, "NVS 占用: %u/%u 条 · 剩余 %u 条 · %u 个命名空间",
                 (unsigned)ns.used, (unsigned)ns.total, (unsigned)ns.free_entries,
                 (unsigned)ns.ns);
    }
    ESP_LOGI(TAG, "背光: %d%% · 超时 %d 秒（0 = 常亮）", S.bl, S.bl_to);
    ESP_LOGI(TAG, "已恢复上次设置: %u.%03u.%03u · 步进 %u · 侧音 %d Hz · 音量 %d%% · %d WPM",
             (unsigned)(S.freq / 1000000u), (unsigned)((S.freq / 1000u) % 1000u),
             (unsigned)(S.freq % 1000u), (unsigned)S.step, S.tone, S.vol, S.wpm);
    s_bl_dim = false;
    bl_note();
    bsp_display_backlight((uint8_t)S.bl);

    cw_decoder_init(&s_dec, cw_morse_dit_ms(S.wpm), rx_emit, NULL);
    cw_decoder_init(&s_txdec, cw_morse_dit_ms(S.wpm), tx_emit, NULL);

    build_main();
    draw_marker();                  // 占用表还没来也要先把箭头摆到当前频率上
    lv_screen_load(s_main);
    refresh_battery();

    // 无 PSRAM，内存余量是这台机器最紧的资源，开机就报一次便于调 LV_MEM_SIZE。
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "内存: 系统剩余 %u KB · LVGL 池 %u KB 已用 %u KB (%u%%, 碎片 %u%%)",
             (unsigned)(esp_get_free_heap_size() / 1024),
             (unsigned)(mon.total_size / 1024),
             (unsigned)((mon.total_size - mon.free_size) / 1024),
             (unsigned)mon.used_pct, (unsigned)mon.frag_pct);

    s_tick = lv_timer_create(tick_cb, 100, NULL);
    s_repeat = lv_timer_create(repeat_cb, REPEAT_PERIOD_MS, NULL);
    s_dec_tick = lv_timer_create(dec_tick_cb, 30, NULL);

    // 电台跑到这儿说明新固件能正常起来 —— 撤掉回滚保护。
    // 放在最后：若中间任何一步卡死（建屏失败、LVGL 堆不够触发看门狗），
    // 就永远走不到这一句，重启时 bootloader 会自动弹回上一槽。
    cw_ota_init();
}

void cw_radio_exit(void) {
    // 退出电台（关机 / 切到别的应用）前把没落盘的设置补上，
    // 尤其是刚转过还没到防抖时间的频率。
    settings_flush();
    s_running = false;
    if (s_tick) { lv_timer_delete(s_tick); s_tick = NULL; }
    if (s_repeat) { lv_timer_delete(s_repeat); s_repeat = NULL; }
    if (s_dec_tick) { lv_timer_delete(s_dec_tick); s_dec_tick = NULL; }
    s_repeat_btn = -1;
    if (s_main) { lv_obj_delete(s_main); s_main = NULL; }
    if (s_menu) { lv_obj_delete(s_menu); s_menu = NULL; }
    if (s_adj) { lv_obj_delete(s_adj); s_adj = NULL; }
    s_lbl_call = NULL;
    s_lbl_rx = s_lbl_tx = s_lbl_net = s_lbl_bat = s_lbl_freq = s_lbl_pitch = NULL;
    s_bar_s = s_adj_bar = NULL;
    s_adj_title = s_adj_value = s_adj_note = NULL;
    s_about = NULL;
    for (int i = 0; i < ADJ_LIST_MAX; i++) s_adj_list[i] = s_adj_list_lbl[i] = NULL;
    for (int i = 0; i < 2; i++) s_adj_btn[i] = s_adj_btn_lbl[i] = NULL;
    for (int i = 0; i < ABOUT_ROWS; i++) s_about_key[i] = s_about_val[i] = NULL;
    for (int i = 0; i < 2; i++) s_about_hdr[i] = NULL;
    for (int i = 0; i < NETINFO_ROWS; i++) s_netinfo_key[i] = s_netinfo_val[i] = NULL;
    for (int i = 0; i < SPEC_BARS; i++) s_bars[i] = NULL;
    for (int i = 0; i < ROSTER_MAX; i++) s_ticks[i] = NULL;
    s_marker = NULL;
    s_lbl_span = NULL;
    for (int i = 0; i < 3; i++) s_wifi_bars[i] = NULL;
    s_bar_online = NULL;
    for (int i = 0; i < STATION_ROWS; i++) s_lbl_st[i] = NULL;
    for (int i = 0; i < ADJ_COUNT; i++) s_rows[i] = NULL;
}

esp_err_t cw_radio_start(void) {
    s_audio_run = true;
    s_backlog_ms = AUDIO_BACKLOG_MS;    // 音频任务一上来就按这个量把存货填起来
    // 优先级 6：高于 LVGL(4) 和按键(5)。喂 PCM 那条路一旦被拖住，DMA 放空就是爆音，
    // 所以它必须能压过刷屏。
    if (xTaskCreate(audio_task, "cw_audio", 3072, NULL, 6, &s_audio_task) != pdPASS) {
        s_audio_run = false;
        ESP_LOGE(TAG, "音频任务创建失败，侧音不可用");
    }
    // 优先级 3：写 Flash 会关 cache，这条路整个系统都会跟着停一下；
    // 放在低位，至少不会去抢喂 PCM 和刷屏的 CPU。
    if (!s_save_task) {
        if (xTaskCreate(save_task, "cw_save", 3072, NULL, 3, &s_save_task) != pdPASS) {
            s_save_task = NULL;
            ESP_LOGE(TAG, "落盘任务创建失败，设置改完只能等重启前补写");
        }
    }
    if (!s_keyq) s_keyq = xQueueCreate(32, sizeof(key_ev_t));
    if (s_keyq && !s_key_task) {
        s_key_run = true;
        // 优先级 5：高于 lvgl 任务，按键能被及时调度；又不会高到饿死别人。
        if (xTaskCreate(key_task, "cw_key", 4096, NULL, 5, &s_key_task) != pdPASS) {
            s_key_run = false;
            ESP_LOGE(TAG, "按键任务创建失败，按键不可用");
        }
    }
    // 没有 Wi-Fi 凭据（首次开机跳过了配网）：别起网络任务去白等 20 秒连接超时，
    // 直接以离线方式起来 —— 能练习发报、能进菜单，菜单里选 WI-FI SETUP 再去配网。
    if (!cw_prov_have_cred()) {
        ESP_LOGW(TAG, "没有 Wi-Fi 凭据：以离线模式启动（菜单 → WI-FI SETUP 再去配网）");
        cw_net_ids_init();             // 呼号还是要有的，顶行不能空着
        s_online = false;
        refresh_online();
        return ESP_OK;
    }
    return cw_net_start(S.freq, S.wpm);
}

esp_err_t cw_radio_stop(void) {
    // 收摊顺序有讲究：先把没落盘的设置同步写完（此刻已经不追求音质了），
    // 再停音频任务，最后停落盘任务。
    settings_flush();
    s_audio_run = false;
    for (int i = 0; i < 40 && s_audio_task; i++) vTaskDelay(pdMS_TO_TICKS(100));
    if (s_save_task) { vTaskDelete(s_save_task); s_save_task = NULL; }
    s_key_run = false;
    for (int i = 0; i < 20 && s_key_task; i++) vTaskDelay(pdMS_TO_TICKS(100));
    cw_net_stop();
    return ESP_OK;
}
