// main/cw_prov.h —— SoftAP + 强制门户配网的对外接口。
//
// 凭据存在 NVS（namespace "cw"），不再靠编译时写死：
//   ssid / pass  家里 Wi-Fi 的用户名与密码（必需）
//   srv          服务器地址（可选；IP 或域名都行，取值规则见 cw_prov_server_addr）
//   srvclr       1 = 基站被清过（恢复出厂），此时不许再拿 Kconfig 的地址兜底
//   call         呼号（可选，空则用 Kconfig 的 CONFIG_CW_CALLSIGN）
//   force        1 = 下次开机直接进配网（菜单里手动触发用）
//   srvmode      1 = 下次开机进配网，但只让改服务器地址
//   callmode     1 = 下次开机进配网，但只让改 Ham 呼号（虚拟呼号由服务器下发，改不了）
//   vcall        服务器下发的虚拟呼号（6 位，V 开头），与 Global UID 永久绑定
//   appass       配网热点的 WPA2 密码，首次开机随机生成后固定
//   fail         连续连接失败次数，达到 2 次自动置 force
//   skip         1 = 上次在配网屏长按 OK 跳过了配网，本次开机别再进配网
#pragma once

#include <stdbool.h>
#include "bsp_button.h"
#include "esp_err.h"

#define CW_PROV_SSID_MAX 33
#define CW_PROV_PASS_MAX 65
// 64 而不是 40：这一栏现在要放域名（station.didaaa.bubblegear.xyz 就有 29 个字符），
// 别人自建服务器的域名可能更长。DNS 名字理论上能到 253，但我们只用它连一台服务器，
// 64 足够，再长就挤爆配网页的输入框和 ABOUT 那一行的排版了。
#define CW_PROV_SRV_MAX  64
#define CW_PROV_CALL_MAX 9
// 虚拟呼号最长 6 位（V + 5 位），留一位给 '\0'。
#define CW_PROV_VCALL_MAX 9

// 配网的三种形态。以前是 bool server_only，现在多了"只改呼号"，用枚举更清楚：
// 三者的差别只在"页面上出现哪几栏"，保存时未出现的字段一律沿用 NVS 里的旧值。
typedef enum {
    CW_PROV_FULL = 0,     // 完整配网：Wi-Fi + 服务器地址 + Ham 呼号
    CW_PROV_SERVER,       // 只改服务器地址（菜单 BASE STATION → CHANGE）
    CW_PROV_CALL,         // 只改 Ham 呼号（菜单 CALLSIGN → UPDATE）
} cw_prov_mode_t;

// 出厂默认背光亮度（NVS 里没存过时用它）。配网屏和电台屏各有一份初始值，
// 都引用这个宏，免得改了一处忘了另一处。
//   bl           背光亮度 10..100%
//   blto         背光超时秒数：10 / 30 / 60，0 = 常亮不熄
#define CW_BL_DEFAULT 50

// 出厂默认背光超时（NVS 里没存过时用它）。可选档位见 cw_radio.c 的 BL_TO_OPTS。
#define CW_BL_TO_DEFAULT 30

// 合法档位判定：0 表示常亮，其余只认 10 / 30 / 60 三档。
// NVS 里躺着别的值（旧固件或手改过）就当没存过，回落到默认值。
static inline bool cw_blto_valid(int s) {
    return s == 0 || s == 10 || s == 30 || s == 60;
}

typedef struct {
    char ssid[CW_PROV_SSID_MAX];
    char pass[CW_PROV_PASS_MAX];
    char srv[CW_PROV_SRV_MAX];
    char call[CW_PROV_CALL_MAX];         // Ham 呼号：用户自定义
    char vcall[CW_PROV_VCALL_MAX];       // 虚拟呼号：服务器下发，本地只做缓存展示
} cw_cred_t;

// 读凭据。返回 false 表示 NVS 里还没有（首次开机）。
bool cw_prov_load(cw_cred_t *out);

// 背光亮度（10..100%）。配网屏上 UP/DOWN 直接调它，改完就落盘，重启后电台界面沿用。
// 读不到（首次开机）返回 false，调用方用默认值。
bool cw_prov_load_bl(uint8_t *bl);
void cw_prov_save_bl(uint8_t bl);

// 背光超时（秒，0 = 常亮）。菜单里 BL TIMEOUT 一项调它，同样落盘，
// 电台屏和配网屏共用同一档。读不到返回 false。
bool cw_prov_load_blto(uint8_t *sec);
void cw_prov_save_blto(uint8_t sec);

// 电台设置的通用的落盘/读取口（NVS namespace "cw"）。菜单里改过的频率、步进、
// 侧音、音量、速度都靠这两个接口持久化，重启后原样恢复。
// 读不到（首次开机 / 旧固件没存过）返回 false，调用方保留默认值。
bool cw_prov_load_u32(const char *key, uint32_t *v);
// 一次 NVS 会话里写多项：keys/vals 是等长数组，n 为项数。
// 分开逐项写会反复 open/commit/close，一次调谐要改好几项时没必要。
void cw_prov_save_u32s(const char *const *keys, const uint32_t *vals, int n);

// 电台设置的"一次写全"：n 项 u32 + 背光亮度/超时两档，全部 set 完只 commit 一次。
// ★ 为什么必须合成一次：NVS 分区只有 24KB（6 个 4KB 扇区），条目写多了就要做扇区
//   回收（搬移 + 擦除），单次 commit 实测能到几百毫秒。落盘那段时间 cache 是关的，
//   音频任务喂不上 PCM，DMA 放空就是一声爆音 —— 三次 commit 等于把危险翻三倍。
void cw_prov_save_settings(const char *const *keys, const uint32_t *vals, int n,
                           uint8_t bl, uint8_t blto);

// NVS 分区占用（诊断用）。条目快用满时每次 commit 都可能触发回收，落盘会变慢。
typedef struct { size_t used, free_entries, total, ns; } cw_nvs_stats_t;
bool cw_prov_nvs_stats(cw_nvs_stats_t *out);

// force 标志：菜单里手动进配网，或连续连不上时自动置位。
bool cw_prov_forced(void);
void cw_prov_request(void);

// srvmode 标志：只改服务器地址。进的还是配网那套（SoftAP + 配置页），但页面上
// 只有服务器地址一栏，Wi-Fi 的 SSID/密码原样保留。菜单 BASE STATION → CHANGE 用它。
bool cw_prov_srv_mode(void);
void cw_prov_request_server(void);
void cw_prov_clear_srv_mode(void);

// callmode 标志：只改 Ham 呼号。虚拟呼号由服务器按 Global UID 下发，
// 本地改不了 —— 页面上就只有 Ham 一栏，VIRTUAL 那行是只读的。
bool cw_prov_call_mode(void);
void cw_prov_request_call(void);
void cw_prov_clear_call_mode(void);

// 虚拟呼号缓存（服务器下发后写进来）。它跟着"呼号永久绑定"走：同一个 Global UID
// 每次认证拿到的都是同一个号，存一份只是为了让离线开机时屏幕上不空着。
void cw_prov_save_vcall(const char *vcall);

// 恢复出厂设置：清空 NVS 里本应用存过的全部内容（Wi-Fi 凭据、服务器地址、呼号、
// 频率/步进/侧音/音量/速度、背光两档、force/srvmode/fail/skip 各种标志）。
// 只留配网热点的固定密码 appass —— 它与硬件绑定（首次开机随机一次之后不变），
// 不该跟着出厂设置一起变。清完由调用方 esp_restart()，重启后一切都回到默认值。
void cw_prov_factory_reset(void);

// 当前生效的服务器地址：NVS 里配过用配的，从没配过用 Kconfig 兜底，都没有就是 "None"。
// 菜单 BASE STATION 一页显示它（"None" = 没配基站，出了厂/复位后就是这个状态）。
const char *cw_prov_server_ip(void);

// 给网络层用的同一份地址，只是"没配"时返回空串 —— 调用方据此跳过 UDP / MQTT，
// 而不是拿 "None" 去 inet_addr（那会得到 255.255.255.255 这种假地址）。
const char *cw_prov_server_addr(void);

// 跳过配网：配网屏上长按 OK 约 1 秒即可什么都不填直接开机，以离线方式进电台
// （顶行 Wi-Fi 图标是暗的，菜单里随时可以再选 WI-FI SETUP 去配网）。
// 跳过会在 NVS 里留一个 skip 标记并重启 —— 直接往下走的话 SoftAP/httpd/DNS 任务
// 还占着内存，而且 STA 起不来（同一块 Wi-Fi 已经被配成 AP 了）。
bool cw_prov_skip_pending(void);
void cw_prov_clear_skip(void);

// NVS 里是否已有可用的 Wi-Fi 凭据（Kconfig 里写死的 SSID 也算）。
// 电台启动时先看它：没有就别白等 20 秒连接超时，直接以离线模式起来。
bool cw_prov_have_cred(void);

// 连上了清失败计数；连不上记一次，达到阈值就置 force 并返回 true（调用方应重启）。
void cw_prov_note_ok(void);
bool cw_prov_note_fail(void);

// 进入配网模式：起 SoftAP（带随机密码）+ DNS 劫持 + HTTP 配置页，
// 屏幕上写明热点名、密码与地址。
// mode 决定页面上出现哪几栏（CW_PROV_SERVER 只留服务器地址、CW_PROV_CALL 只留
// Ham 呼号），没出现的字段一律沿用 NVS 里的旧值。
// 阻塞运行；用户在页面上保存后本函数会自行 esp_restart()，正常不返回。
// 返回非 ESP_OK 表示 SoftAP 起不来（此时调用方应退回原来的流程）。
esp_err_t cw_prov_run(cw_prov_mode_t mode);

// 热点名，形如 "CW-2F4A"（MAC 尾段，避免多台设备同名）。
const char *cw_prov_ap_ssid(void);

// 热点的 WPA2 密码：第一次进配网时随机生成并写入 NVS，之后固定不变（同一台
// 设备永远是同一串，抄一次即可）。想换只能清 NVS 里的 "appass"。
const char *cw_prov_ap_pass(void);

// 配网屏的按键入口。电台还没启动（main 的输入队列此时也没放行），按键归配网屏：
// UP/DOWN 调亮度，任意键唤醒背光。
// 由 main 的按键回调转发，运行在 esp_timer 上下文，所以这里只记增量，不改 LVGL。
void cw_prov_on_key(bsp_btn_t btn, bsp_btn_ev_t ev);
