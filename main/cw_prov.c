// main/cw_prov.c —— 手机配网：设备自己发一个热点，手机连上后浏览器自动弹配置页。
//
// 为什么不用 ESP-IDF 自带的 wifi_provisioning：它带 protocomm + protobuf + mbedtls，
// 体积大、页面还不好自定义。这里只需要"一个表单"，用 esp_http_server 自己写更省。
//
// 强制门户（captive portal）的原理：手机连上热点后会主动探测几个固定 URL
// （Android 是 /generate_204，iOS 是 /hotspot-detect.html，Windows 是 /ncsi.txt），
// 期待得到"已联网"的应答。我们一不做二不休：
//   1) DHCP 把 DNS 下发给手机（esp_netif 的 AP 默认就这么干），
//   2) 起一个 UDP 53 的假 DNS，把所有 A 查询都答成 192.168.4.1，
//   3) 所有没注册过的 GET 路径一律 302 到配置页。
// 于是手机的探测请求必然拿到我们的页面，"需要登录"的提示就自己弹出来了。
#include "cw_prov.h"

#include "bsp_audio.h"
#include "bsp_display.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lvgl.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "cw_prov";

#define NVS_NS      "cw"
#define AP_IP_STR   "192.168.4.1"
#define FAIL_LIMIT  2
#define AP_PASS_LEN 8             // WPA2 要求至少 8 位

static char             s_ap_ssid[16];
static char             s_ap_pass[AP_PASS_LEN + 1];
static SemaphoreHandle_t s_done;
static bool             s_nvs_ready;
// 本次配网的形态：完整 / 只改服务器地址 / 只改 Ham 呼号。
// 决定页面上出现哪几栏 —— 没出现的字段沿用 NVS 里的旧值，绝不写空。
static cw_prov_mode_t   s_mode = CW_PROV_FULL;

// ---------------------------------------------------------------------------
// NVS
// ---------------------------------------------------------------------------
static esp_err_t nvs_ensure(void) {
    if (s_nvs_ready) return ESP_OK;
    esp_err_t e = nvs_flash_init();
    // 分区表被改动过才需要擦除；这里与 cw_net 的约定一致：不因初始化失败擦用户数据。
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重建: %s", esp_err_to_name(e));
        return e;
    }
    if (e != ESP_OK) return e;
    s_nvs_ready = true;
    return ESP_OK;
}

bool cw_prov_load(cw_cred_t *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (nvs_ensure() != ESP_OK) return false;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = sizeof(out->ssid);
    esp_err_t e = nvs_get_str(h, "ssid", out->ssid, &n);
    if (e == ESP_OK) {
        n = sizeof(out->pass); (void)nvs_get_str(h, "pass", out->pass, &n);
        n = sizeof(out->srv);   (void)nvs_get_str(h, "srv",   out->srv,   &n);
        n = sizeof(out->call);  (void)nvs_get_str(h, "call",  out->call,  &n);
        n = sizeof(out->vcall); (void)nvs_get_str(h, "vcall", out->vcall, &n);
    }
    nvs_close(h);
    return e == ESP_OK && out->ssid[0] != '\0';
}

static bool get_u8(const char *key, uint8_t *v) {
    if (nvs_ensure() != ESP_OK) return false;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    esp_err_t e = nvs_get_u8(h, key, v);
    nvs_close(h);
    return e == ESP_OK;
}

static void set_u8(const char *key, uint8_t v) {
    if (nvs_ensure() != ESP_OK) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    (void)nvs_set_u8(h, key, v);
    (void)nvs_commit(h);
    nvs_close(h);
}

bool cw_prov_load_bl(uint8_t *bl) {
    uint8_t b = 0;
    if (!get_u8("bl", &b)) return false;
    if (bl) *bl = b;
    return true;
}

void cw_prov_save_bl(uint8_t bl) { set_u8("bl", bl); }

bool cw_prov_load_blto(uint8_t *sec) {
    uint8_t v = 0;
    if (!get_u8("blto", &v)) return false;
    if (!cw_blto_valid((int)v)) return false;   // 不是合法档位就当没存过
    if (sec) *sec = v;
    return true;
}

void cw_prov_save_blto(uint8_t sec) { set_u8("blto", sec); }

bool cw_prov_load_u32(const char *key, uint32_t *v) {
    if (!key || !v) return false;
    if (nvs_ensure() != ESP_OK) return false;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    esp_err_t e = nvs_get_u32(h, key, v);
    nvs_close(h);
    return e == ESP_OK;
}

void cw_prov_save_u32s(const char *const *keys, const uint32_t *vals, int n) {
    if (!keys || !vals || n <= 0) return;
    if (nvs_ensure() != ESP_OK) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    for (int i = 0; i < n; i++) (void)nvs_set_u32(h, keys[i], vals[i]);
    (void)nvs_commit(h);          // 一次 commit：中途掉电要么全旧要么全新，不会写一半
    nvs_close(h);
}

void cw_prov_save_settings(const char *const *keys, const uint32_t *vals, int n,
                           uint8_t bl, uint8_t blto) {
    if (!keys || !vals || n <= 0) return;
    if (nvs_ensure() != ESP_OK) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    for (int i = 0; i < n; i++) (void)nvs_set_u32(h, keys[i], vals[i]);
    // 亮度/超时原本是 u8 键（配网屏也在用，键名不能改），一并塞进同一次会话。
    (void)nvs_set_u8(h, "bl", bl);
    (void)nvs_set_u8(h, "blto", blto);
    (void)nvs_commit(h);          // 全场只有这一次 commit
    nvs_close(h);
}

bool cw_prov_nvs_stats(cw_nvs_stats_t *out) {
    if (!out) return false;
    nvs_stats_t st;
    if (nvs_get_stats("nvs", &st) != ESP_OK) return false;
    out->used = st.used_entries;
    out->free_entries = st.free_entries;
    out->total = st.total_entries;
    out->ns = st.namespace_count;
    return true;
}

bool cw_prov_forced(void) { uint8_t v = 0; get_u8("force", &v); return v != 0; }
void cw_prov_request(void) { set_u8("force", 1); }
void cw_prov_note_ok(void) { set_u8("fail", 0); }

// srvmode：下次开机进配网，但只让改服务器地址。配完（cred_save）自动清掉。
bool cw_prov_srv_mode(void) { uint8_t v = 0; get_u8("srvmode", &v); return v != 0; }
void cw_prov_request_server(void) { set_u8("srvmode", 1); }
void cw_prov_clear_srv_mode(void) { set_u8("srvmode", 0); }

// callmode：下次开机进配网，只让改 Ham 呼号。虚拟呼号由服务器按 Global UID 下发，
// 本地改不了 —— 页面上根本不给它输入框。
bool cw_prov_call_mode(void) { uint8_t v = 0; get_u8("callmode", &v); return v != 0; }
void cw_prov_request_call(void) { set_u8("callmode", 1); }
void cw_prov_clear_call_mode(void) { set_u8("callmode", 0); }

// 虚拟呼号落盘。它是服务器认证成功后下发的，与 Global UID 永久绑定；
// 存一份只是为了让离线开机时屏幕上不空着（下次连上服务器还会再校验一次）。
void cw_prov_save_vcall(const char *vcall) {
    if (!vcall) return;
    if (nvs_ensure() != ESP_OK) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    (void)nvs_set_str(h, "vcall", vcall);
    (void)nvs_commit(h);
    nvs_close(h);
}

// 跳过配网的标记。写在 NVS 里，是因为跳过后要整机重启（SoftAP/httpd/DNS 得让位），
// 重启之后 app_main 靠这个标记才知道"这次别再拉我进配网了"。
bool cw_prov_skip_pending(void) { uint8_t v = 0; get_u8("skip", &v); return v != 0; }
void cw_prov_clear_skip(void) { set_u8("skip", 0); }

// 有没有可用的 Wi-Fi 凭据：NVS 里存过，或者编译期就把 SSID 填好了（不是占位符）。
bool cw_prov_have_cred(void) {
    cw_cred_t c;
    if (cw_prov_load(&c)) return true;
    return strcmp(CONFIG_CW_WIFI_SSID, "myssid") != 0;
}

// 恢复出厂设置：整个 "cw" 命名空间擦掉（以后新加的键也一并清了，不用在这里记账），
// 再把 appass 写回去，最后补一个空的 "srv" —— 它相当于"用户明确不配基站"的记号，
// 有这个键（哪怕是空串）才不会被 Kconfig 的兜底地址填上（见 srv_key_present）。
void cw_prov_factory_reset(void) {
    if (nvs_ensure() != ESP_OK) return;
    char pass[AP_PASS_LEN + 1] = { 0 };
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    size_t n = sizeof(pass);
    if (nvs_get_str(h, "appass", pass, &n) != ESP_OK) pass[0] = '\0';
    esp_err_t e = nvs_erase_all(h);
    if (e == ESP_OK) {
        if (pass[0]) (void)nvs_set_str(h, "appass", pass);
        // 空串 + srvclr 标记 = "基站被清空了"，别再拿 Kconfig 的地址填回来
        // （取值规则见 cw_prov_server_addr）。
        (void)nvs_set_str(h, "srv", "");
        (void)nvs_set_u8(h, "srvclr", 1);
        e = nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGW(TAG, "恢复出厂设置：NVS 已清空（保留热点密码 %s）%s",
             pass[0] ? pass : "(无)", e == ESP_OK ? "" : " · 写入失败");
}

// "srv" 的取值规则，关键是"空串"有两种来历，得分开对待：
//   · srvclr=1（恢复出厂时写的）→ 基站是被清掉的，srv 里是什么就是什么（空 = None）；
//   · srvclr 没写过            → srv 非空用 srv，空则退回 Kconfig 的编译期地址
//                                （调试时免配网的用法，老机器一直靠它连服务器）。
// 不这么区分的话：要么复位后又被 Kconfig 填回一个地址（BASE STATION 永远显示不出 None），
// 要么老机器那个"配网时留空的 srv"被当成"明确不要基站"（一升级就连不上了）。
static bool srv_cleared(void) { uint8_t v = 0; get_u8("srvclr", &v); return v != 0; }

const char *cw_prov_server_addr(void) {
    static char ip[CW_PROV_SRV_MAX];
    char s[CW_PROV_SRV_MAX] = { 0 };
    if (nvs_ensure() == ESP_OK) {
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
            size_t n = sizeof(s);
            (void)nvs_get_str(h, "srv", s, &n);
            nvs_close(h);
        }
    }
    if (s[0])                 snprintf(ip, sizeof(ip), "%s", s);
    else if (!srv_cleared())  snprintf(ip, sizeof(ip), "%s", CONFIG_CW_SERVER_IP);
    else                      ip[0] = '\0';
    return ip;
}

const char *cw_prov_server_ip(void) {
    const char *ip = cw_prov_server_addr();
    return ip[0] ? ip : "None";
}

bool cw_prov_note_fail(void) {
    uint8_t f = 0;
    get_u8("fail", &f);
    if (f < 200) f++;
    set_u8("fail", f);
    ESP_LOGW(TAG, "Wi-Fi 连接失败计数 %d/%d", f, FAIL_LIMIT);
    if (f >= FAIL_LIMIT) { set_u8("force", 1); return true; }
    return false;
}

static esp_err_t cred_save(const cw_cred_t *c) {
    if (nvs_ensure() != ESP_OK) return ESP_FAIL;
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    (void)nvs_set_str(h, "ssid", c->ssid);
    (void)nvs_set_str(h, "pass", c->pass);
    (void)nvs_set_str(h, "srv", c->srv);
    (void)nvs_set_str(h, "call", c->call);
    // 虚拟呼号不在配网页上出现，原样留着（页面改不到它）。
    if (c->vcall[0]) (void)nvs_set_str(h, "vcall", c->vcall);
    // 填了地址就把"基站已清空"的标记摘掉；留空则留着它 —— 那是"明确不要基站"的意思，
    // 摘掉的话地址栏又会被 Kconfig 的兜底值填上（规则见 cw_prov_server_addr）。
    if (c->srv[0]) (void)nvs_set_u8(h, "srvclr", 0);
    (void)nvs_set_u8(h, "force", 0);      // 配完了就别再自动进配网
    (void)nvs_set_u8(h, "srvmode", 0);    // 只改服务器那次也算配完了
    (void)nvs_set_u8(h, "callmode", 0);   // 只改呼号那次同样算配完了
    (void)nvs_set_u8(h, "fail", 0);
    e = nvs_commit(h);
    nvs_close(h);
    return e;
}

// ---------------------------------------------------------------------------
// 假 DNS：所有 A 查询都答成 AP 自己的地址
// ---------------------------------------------------------------------------
static void dns_task(void *arg) {
    (void)arg;
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) { ESP_LOGE(TAG, "DNS socket 创建失败"); vTaskDelete(NULL); return; }

    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        ESP_LOGE(TAG, "DNS 绑定 53 端口失败");
        close(s);
        vTaskDelete(NULL);
        return;
    }
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[512];
    while (1) {
        struct sockaddr_storage from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
        if (n < 12) continue;
        if (buf[2] & 0x80) continue;                  // 不是查询包
        int qd = (buf[4] << 8) | buf[5];
        if (qd != 1) continue;

        // 跳过 QNAME（一串长度前缀的 label，以 0x00 结尾）与 QTYPE/QCLASS
        int i = 12;
        while (i < n && buf[i] != 0) i += buf[i] + 1;
        if (i + 5 > n) continue;
        int qtype = (buf[i + 1] << 8) | buf[i + 2];
        if (qtype != 1) continue;                     // 只管 A 记录
        int qend = i + 5;

        buf[2] |= 0x80;                               // QR：这是应答
        buf[3] |= 0x80;                               // RA
        buf[6] = 0; buf[7] = 1;                       // ANCOUNT = 1
        buf[8] = buf[9] = buf[10] = buf[11] = 0;      // NSCOUNT / ARCOUNT = 0

        // 0xC00C = 指向偏移 12（问题里的域名）；TTL 60 秒
        static const uint8_t ans[16] = { 0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01,
                                         0x00, 0x00, 0x00, 0x3C, 0x00, 0x04,
                                         192, 168, 4, 1 };
        if (qend + 16 <= (int)sizeof(buf)) {
            memcpy(buf + qend, ans, 16);
            (void)sendto(s, buf, qend + 16, 0, (const struct sockaddr *)&from, flen);
        }
    }
}

// ---------------------------------------------------------------------------
// 配置页
// ---------------------------------------------------------------------------
static const char PAGE_HEAD[] =
    "<!DOCTYPE html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>CW Radio setup</title><style>"
    "body{font-family:-apple-system,system-ui,sans-serif;background:#0B1016;color:#E4F1FF;"
    "margin:0;padding:22px;max-width:420px}"
    "h1{font-size:18px;color:#FFB300;margin:0 0 6px}"
    "p{font-size:13px;color:#8FA3B8;margin:0 0 16px;line-height:1.5}"
    "label{display:block;font-size:12px;color:#8FA3B8;margin:14px 0 5px}"
    "input{width:100%;box-sizing:border-box;padding:11px;font-size:16px;border:1px solid #2A3644;"
    "border-radius:6px;background:#141C26;color:#E4F1FF}"
    "button{margin-top:22px;width:100%;padding:13px;font-size:16px;border:0;border-radius:6px;"
    "background:#FFB300;color:#0B1016;font-weight:600}"
    "</style></head><body><h1>CW Radio setup</h1>"
    "<p>Join this hotspot, then fill in your home Wi-Fi. "
    "The device saves it and restarts.</p><form method=post action=/save>";

// 只改服务器地址那一页的表头。SSID 用 <b> 标出来，明确告诉用户"这一项动不了"。
static const char PAGE_HEAD_SRV_1[] =
    "<!DOCTYPE html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Base station</title><style>"
    "body{font-family:-apple-system,system-ui,sans-serif;background:#0B1016;color:#E4F1FF;"
    "margin:0;padding:22px;max-width:420px}"
    "h1{font-size:18px;color:#FFB300;margin:0 0 6px}"
    "p{font-size:13px;color:#8FA3B8;margin:0 0 16px;line-height:1.5}"
    "b{color:#E4F1FF;font-weight:600}"
    "label{display:block;font-size:12px;color:#8FA3B8;margin:14px 0 5px}"
    "input{width:100%;box-sizing:border-box;padding:11px;font-size:16px;border:1px solid #2A3644;"
    "border-radius:6px;background:#141C26;color:#E4F1FF}"
    "button{margin-top:22px;width:100%;padding:13px;font-size:16px;border:0;border-radius:6px;"
    "background:#FFB300;color:#0B1016;font-weight:600}"
    "</style></head><body><h1>Base station</h1>"
    "<p>Only the server address can be changed.<br>Wi-Fi stays on <b>";

static const char PAGE_HEAD_SRV_2[] = "</b>.</p><form method=post action=/save>";

// 只改 Ham 呼号那一页。虚拟呼号那一行是只读的 —— 它由服务器按 Global UID 下发、
// 永久绑定，本机没有改它的权限。
static const char PAGE_HEAD_CALL_1[] =
    "<!DOCTYPE html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Callsign</title><style>"
    "body{font-family:-apple-system,system-ui,sans-serif;background:#0B1016;color:#E4F1FF;"
    "margin:0;padding:22px;max-width:420px}"
    "h1{font-size:18px;color:#FFB300;margin:0 0 6px}"
    "p{font-size:13px;color:#8FA3B8;margin:0 0 16px;line-height:1.5}"
    "b{color:#E4F1FF;font-weight:600}"
    "label{display:block;font-size:12px;color:#8FA3B8;margin:14px 0 5px}"
    "input{width:100%;box-sizing:border-box;padding:11px;font-size:16px;border:1px solid #2A3644;"
    "border-radius:6px;background:#141C26;color:#E4F1FF}"
    ".ro{width:100%;box-sizing:border-box;padding:11px;font-size:16px;border:1px dashed #2A3644;"
    "border-radius:6px;background:#101820;color:#7A8CA0}"
    "button{margin-top:22px;width:100%;padding:13px;font-size:16px;border:0;border-radius:6px;"
    "background:#FFB300;color:#0B1016;font-weight:600}"
    "</style></head><body><h1>Callsign</h1>"
    "<p>Ham callsign is yours to set.<br>"
    "Virtual callsign is issued by the server and locked to this device.<br>"
    "Current virtual: <b>";

static const char PAGE_HEAD_CALL_2[] = "</b></p><form method=post action=/save>";

static const char PAGE_TAIL[] = "<button type=submit>Save and restart</button></form></body></html>";

static const char OK_PAGE[] =
    "<!DOCTYPE html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Saved</title><style>body{font-family:-apple-system,system-ui,sans-serif;"
    "background:#0B1016;color:#E4F1FF;padding:30px;text-align:center}"
    "h1{font-size:18px;color:#7CE38B}p{font-size:13px;color:#8FA3B8;line-height:1.6}"
    "</style></head><body><h1>Saved</h1><p>The radio is restarting.<br>"
    "It will join your Wi-Fi and show the frequency.</p></body></html>";

static void row(char *out, size_t cap, const char *label, const char *name,
                const char *value, const char *ph, int password) {
    // 已存的值填进 value，没存过就留 placeholder（服务器 IP 与呼号有 Kconfig 兜底）
    char tmp[256];
    snprintf(tmp, sizeof(tmp),
             "<label>%s</label><input name=%s type=%s value='%s' placeholder='%s'%s>",
             label, name, password ? "password" : "text", value, ph,
             password ? "" : " autocomplete=off");
    strncat(out, tmp, cap - strlen(out) - 1);
}

static esp_err_t h_root(httpd_req_t *r) {
    static char page[2200];
    cw_cred_t c;
    if (!cw_prov_load(&c)) memset(&c, 0, sizeof(c));

    if (s_mode == CW_PROV_SERVER) {
        // 只留服务器地址一栏：SSID / 密码 / 呼号连输入框都不给，自然也就改不了。
        snprintf(page, sizeof(page), "%s%s%s", PAGE_HEAD_SRV_1,
                 c.ssid[0] ? c.ssid : "(none)", PAGE_HEAD_SRV_2);
        row(page, sizeof(page), "Server address", "srv", c.srv, CONFIG_CW_SERVER_IP, 0);
    } else if (s_mode == CW_PROV_CALL) {
        // 只留 Ham 呼号一栏；虚拟呼号以只读文本摆出来，不给 input。
        snprintf(page, sizeof(page), "%s%s%s", PAGE_HEAD_CALL_1,
                 c.vcall[0] ? c.vcall : "(not issued yet)", PAGE_HEAD_CALL_2);
        // 占位符写成"例如"而不是 Kconfig 的默认值：Ham 没有默认值，留空就是留空
        // （屏幕上显示 --），摆一个看似已填好的号会让人以为不用管这一栏。
        row(page, sizeof(page), "Ham callsign", "call", c.call, "e.g. BH1ABC (optional)", 0);
    } else {
        snprintf(page, sizeof(page), "%s", PAGE_HEAD);
        row(page, sizeof(page), "Wi-Fi name (SSID)", "ssid", c.ssid, "MyHomeWiFi", 0);
        row(page, sizeof(page), "Wi-Fi password", "pass", "", "leave empty if open", 1);
        row(page, sizeof(page), "Server IP", "srv", c.srv, CONFIG_CW_SERVER_IP, 0);
        row(page, sizeof(page), "Callsign", "call", c.call, "e.g. BH1ABC (optional)", 0);
    }
    strncat(page, PAGE_TAIL, sizeof(page) - strlen(page) - 1);

    httpd_resp_set_type(r, "text/html");
    return httpd_resp_send(r, page, HTTPD_RESP_USE_STRLEN);
}

// 手机的各种联网探测都落到这里：一律 302 到配置页，它就会弹"需要登录"。
static esp_err_t h_redirect(httpd_req_t *r) {
    httpd_resp_set_status(r, "302 Found");
    httpd_resp_set_hdr(r, "Location", "/");
    return httpd_resp_send(r, NULL, 0);
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void urldec(char *s) {
    char *d = s;
    for (const char *p = s; *p;) {
        if (*p == '+') { *d++ = ' '; p++; }
        else if (*p == '%' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) {
            *d++ = (char)((hexval(p[1]) << 4) | hexval(p[2]));
            p += 3;
        } else *d++ = *p++;
    }
    *d = '\0';
}

// 呼号只留大写字母数字，与 cw_net 的兜底逻辑保持一致（不然屏幕上会出现小写和符号）。
static void norm_call(char *call) {
    char up[CW_PROV_CALL_MAX] = { 0 };
    for (size_t i = 0, n = 0; call[i] && n < CW_PROV_CALL_MAX - 1; i++) {
        char ch = call[i];
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) up[n++] = ch;
    }
    memcpy(call, up, sizeof(up));
}

static void field(char *dst, size_t n, const char *body, const char *key) {
    char pat[16];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char *p = strstr(body, pat);
    if (!p) { dst[0] = '\0'; return; }
    p += strlen(pat);
    const char *e = strchr(p, '&');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    if (len >= n) len = n - 1;
    memcpy(dst, p, len);
    dst[len] = '\0';
    urldec(dst);
}

static esp_err_t h_save(httpd_req_t *r) {
    char body[512];
    int got = 0;
    while (got < r->content_len && got < (int)sizeof(body) - 1) {
        int n = httpd_req_recv(r, body + got, sizeof(body) - 1 - got);
        if (n <= 0) break;
        got += n;
    }
    body[got] = '\0';

    cw_cred_t c = { 0 };
    // 两种"只改一栏"的页面共用这一段：先从 NVS 原样读回全部字段，
    // 再只覆盖页面上出现的那一个 —— 页面上没有的字段绝不写空。
    if (s_mode == CW_PROV_SERVER || s_mode == CW_PROV_CALL) {
        cw_cred_t old;
        if (cw_prov_load(&old)) c = old;
        if (s_mode == CW_PROV_SERVER) {
            field(c.srv, sizeof(c.srv), body, "srv");
            if (!c.srv[0]) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Server is empty");
            ESP_LOGI(TAG, "只改服务器地址: %s（Wi-Fi %s 保持不变）", c.srv, c.ssid);
        } else {
            field(c.call, sizeof(c.call), body, "call");
            norm_call(c.call);
            ESP_LOGI(TAG, "只改 Ham 呼号: %s（虚拟呼号 %s 保持不变）",
                     c.call[0] ? c.call : "(空)", c.vcall[0] ? c.vcall : "(尚无)");
        }
    } else {
        field(c.ssid, sizeof(c.ssid), body, "ssid");
        field(c.pass, sizeof(c.pass), body, "pass");
        field(c.srv, sizeof(c.srv), body, "srv");
        field(c.call, sizeof(c.call), body, "call");
        if (!c.ssid[0]) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "SSID is empty");
        norm_call(c.call);
    }

    esp_err_t e = cred_save(&c);
    ESP_LOGI(TAG, "保存凭据 ssid=%s srv=%s call=%s (%s)",
             c.ssid, c.srv[0] ? c.srv : "(默认)", c.call[0] ? c.call : "(默认)",
             esp_err_to_name(e));

    httpd_resp_set_type(r, "text/html");
    httpd_resp_send(r, OK_PAGE, HTTPD_RESP_USE_STRLEN);
    if (s_done) xSemaphoreGive(s_done);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// 配网提示屏：只建这一屏，不碰电台的三屏 / 音频 / 网络，内存留给 httpd。
// ---------------------------------------------------------------------------
// 配网热点的密码：第一次进配网时随机生成 8 位，写进 NVS 之后永远用这一串。
// 字符集剔掉了 0/O/1/I 这几个在 240×320 小屏上容易看错、在手机软键盘上容易输错的字符，
// 正好凑满 32 个：esp_random() 的取值域是 2^32，能被 32 整除，所以取模没有偏差。
static void make_ap_pass(void) {
    static const char SET[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    for (int i = 0; i < AP_PASS_LEN; i++) s_ap_pass[i] = SET[esp_random() % 32];
    s_ap_pass[AP_PASS_LEN] = '\0';
}

// 为什么固定下来：原来每次进配网都重新随机（配完即作废，旁人抄了也连不上），
// 代价是每次换 Wi-Fi 都得照着小屏重敲一遍 8 位。同一台设备永远同一个密码更实用，
// 它只在第一次开机时生成，之后不再变 —— 想换只能清 NVS（"appass"）。
static void ap_pass_ensure(void) {
    make_ap_pass();
    if (nvs_ensure() != ESP_OK) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    size_t n = sizeof(s_ap_pass);
    if (nvs_get_str(h, "appass", s_ap_pass, &n) == ESP_OK && strlen(s_ap_pass) >= AP_PASS_LEN) {
        nvs_close(h);
        return;                                   // 存过：沿用第一次生成的那一串
    }
    make_ap_pass();                               // 首次：生成后落盘
    (void)nvs_set_str(h, "appass", s_ap_pass);
    (void)nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "配网热点密码已生成并固定: %s", s_ap_pass);
}
// ---------------------------------------------------------------------------
// 配网屏的背光：UP/DOWN 调亮度，30 秒无操作熄灭，任意键唤醒。
//
// 亮度与电台界面共用同一个值，在这里改完直接落盘，重启进电台后沿用 —— 配网屏上
// 往往就是第一次调亮度的地方（比如夜里嫌太亮），不该配完网就丢。
// 超时固定 30 秒，两边一致，不提供调节。
//
// 按键回调跑在共享的 esp_timer 任务里，只准写 volatile 变量；真正改 PWM、改标签、
// 写 NVS 都交给 LVGL 的定时器，避免把 esp_timer 拖住。
// ---------------------------------------------------------------------------
#define BL_MIN        10        // 下限留 10%：调到全黑就找不回来了
#define BL_MAX        100
#define BL_STEP       10
// 背光超时跟电台屏共用 NVS 里那一档（"blto"），不再写死；0 = 常亮不熄。
// 开机时读一次存进 s_bl_to_ms，之后 tick 只比这个毫秒数。
#define BL_TO_MS_DEF  (CW_BL_TO_DEFAULT * 1000)
#define BL_REPEAT_MS  260       // 按住不放时的连发间隔

static uint8_t       s_bl = CW_BL_DEFAULT;
static volatile int  s_bl_to_ms = BL_TO_MS_DEF;   // 0 = 常亮
static volatile bool s_bl_dim;
static volatile int64_t s_bl_last_ms;
static lv_obj_t    *s_bl_lbl;
static lv_timer_t  *s_bl_timer;
static char         s_bl_text[40];
static volatile int s_bl_pend;      // 待应用的亮度增量
static volatile int s_bl_hold = -1; // 按住不放的那个键，用于连发
static volatile bool s_prov_active; // 配网屏建好之后，按键才归它
static bool         s_bl_dirty;     // 亮度改过、还没落盘
// 熄屏期间按下的那一次：请求点亮，并且连抬起一起吞掉（只点亮，不顺手改亮度）。
static volatile bool s_bl_wake_req;
static bool         s_bl_wake_swallow;
// 长按 OK 跳过配网：记下按下的时刻，由 bl_tick_cb 到点触发（不用等松手）。
// 阈值取 1000ms —— 比电台里那个 900ms 进菜单的稍长一点，免得手慢一下就误跳过。
#define PROV_SKIP_HOLD_MS 1000
static volatile int64_t s_ok_down_ms;
static volatile bool    s_skip;

static void bl_label_update(void) {
    if (!s_bl_lbl) return;
    snprintf(s_bl_text, sizeof(s_bl_text), "UP/DN  BRIGHTNESS %d%%", (int)s_bl);
    lv_label_set_text(s_bl_lbl, s_bl_text);
}

static void bl_tick_cb(lv_timer_t *t) {
    (void)t;
    int64_t now = esp_timer_get_time() / 1000;

    // 长按 OK 跳过配网：到点就走，不等松手 —— 按住的时候屏幕上该有反应。
    if (s_ok_down_ms > 0 && now - s_ok_down_ms >= PROV_SKIP_HOLD_MS) {
        s_ok_down_ms = 0;
        if (!s_skip) {
            s_skip = true;
            set_u8("skip", 1);
            ESP_LOGW(TAG, "长按 OK：跳过配网，重启后直接进电台（离线）");
            // 屏幕上留一句话再重启，否则用户会以为按键没反应。
            if (s_bl_lbl) lv_label_set_text(s_bl_lbl, "SKIPPED - RESTARTING");
            if (s_done) xSemaphoreGive(s_done);
        }
        return;
    }

    if (s_bl_wake_req) {
        s_bl_wake_req = false;
        s_bl_dim = false;
        s_bl_pend = 0;
        s_bl_hold = -1;
        bsp_display_backlight(s_bl);      // 按原亮度点回去
        s_bl_last_ms = now;
        return;                           // 这一轮别再判熄灭，刚点亮的屏幕不该马上又灭
    }

    if (s_bl_hold >= 0 && now - s_bl_last_ms > BL_REPEAT_MS) {
        s_bl_pend += (s_bl_hold == (int)BSP_BTN_UP ? BL_STEP : -BL_STEP);
        s_bl_last_ms = now;
    }
    if (s_bl_pend) {
        int v = (int)s_bl + s_bl_pend;
        s_bl_pend = 0;
        if (v < BL_MIN) v = BL_MIN;
        if (v > BL_MAX) v = BL_MAX;
        s_bl = (uint8_t)v;
        s_bl_dim = false;
        s_bl_dirty = true;
        bsp_display_backlight(s_bl);      // 实时生效，能看着屏幕调
        bl_label_update();
        s_bl_last_ms = now;
    }
    // 落盘等手离开键：连发时一格一次地写 NVS 纯属浪费。
    if (s_bl_dirty && s_bl_hold < 0) {
        cw_prov_save_bl(s_bl);
        s_bl_dirty = false;
        ESP_LOGI(TAG, "亮度 %d%%（已存，电台界面沿用）", (int)s_bl);
    }

    // 常亮档（0）永远不熄；其余按存下来的秒数熄灭。
    if (s_bl_to_ms > 0 && !s_bl_dim && now - s_bl_last_ms > s_bl_to_ms) {
        s_bl_dim = true;
        bsp_display_backlight(0);
    }
}

void cw_prov_on_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (!s_prov_active) return;
    // 熄屏期间的那次按下：只请求点亮，抬起也一并吞掉，别顺手把亮度改了。
    if (s_bl_wake_swallow) {
        if (ev == BSP_BTN_RELEASE) s_bl_wake_swallow = false;
        return;
    }
    if (s_bl_dim) {
        s_bl_wake_req = true;
        s_bl_last_ms = esp_timer_get_time() / 1000;
        if (ev == BSP_BTN_PRESS) s_bl_wake_swallow = true;
        return;
    }
    if (ev == BSP_BTN_PRESS) {
        if (btn == BSP_BTN_OK) {
            s_ok_down_ms = esp_timer_get_time() / 1000;   // 长按计时交给 tick
        } else if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
            // UP = 调亮，跟电台菜单里 BRIGHTNESS 那一项的方向保持一致。
            s_bl_pend += (btn == BSP_BTN_UP ? BL_STEP : -BL_STEP);
            s_bl_hold = (int)btn;
        }
    } else if (ev == BSP_BTN_RELEASE) {
        if (btn == BSP_BTN_OK) s_ok_down_ms = 0;
        if (s_bl_hold == (int)btn) s_bl_hold = -1;
    } else {
        return;                            // CLICK / DOUBLE / LONG 一概不管
    }
    // 任意按键都算活动。唤醒直接在这里做（只是写一次 LEDC 占空比，很轻），
    // 等下一个 tick 再亮的话，"按了没反应"的观感很明显。
    s_bl_last_ms = esp_timer_get_time() / 1000;
    if (s_bl_dim) { s_bl_dim = false; bsp_display_backlight(s_bl); }
}

static void prov_screen(void) {
    if (!bsp_lvgl_lock(1000)) return;

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, 240, 320);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0B1016), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    lv_obj_t *t = lv_label_create(scr);
    lv_label_set_text(t, s_mode == CW_PROV_SERVER ? "BASE STATION" :
                         s_mode == CW_PROV_CALL   ? "CALLSIGN" : "WI-FI SETUP");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0xFFB300), 0);
    lv_obj_set_pos(t, 8, 26);

    // 步骤 1 的两行（热点名 / 密码）是要照着往手机上敲的，用大字号高亮，
    // 剩下的步骤说明用小字灰字带过。
    lv_obj_t *g1 = lv_label_create(scr);
    lv_label_set_text(g1, "1. Join Wi-Fi");
    lv_obj_set_style_text_font(g1, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(g1, lv_color_hex(0x8FA3B8), 0);
    lv_obj_set_pos(g1, 8, 74);

    lv_obj_t *ssid = lv_label_create(scr);
    lv_label_set_text(ssid, s_ap_ssid);
    lv_obj_set_style_text_font(ssid, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ssid, lv_color_hex(0xE4F1FF), 0);
    lv_obj_set_pos(ssid, 8, 96);

    lv_obj_t *pl = lv_label_create(scr);
    lv_label_set_text(pl, "Password");
    lv_obj_set_style_text_font(pl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(pl, lv_color_hex(0x8FA3B8), 0);
    lv_obj_set_pos(pl, 8, 128);

    lv_obj_t *pw = lv_label_create(scr);
    lv_label_set_text(pw, s_ap_pass);
    lv_obj_set_style_text_font(pw, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(pw, lv_color_hex(0xFFB300), 0);
    lv_obj_set_pos(pw, 8, 146);

    static char lines[3][64];
    snprintf(lines[0], sizeof(lines[0]), "2. Open %s", AP_IP_STR);
    if (s_mode == CW_PROV_SERVER) {
        // 第三行把当前地址也带出来：不然用户不知道该填成什么样，也是个对照。
        snprintf(lines[1], sizeof(lines[1]), "3. New server address");
        snprintf(lines[2], sizeof(lines[2]), "   now %s", cw_prov_server_ip());
    } else if (s_mode == CW_PROV_CALL) {
        // 呼号页：把现在两个呼号都摆出来，用户才知道改的是哪一个。
        cw_cred_t c;
        if (!cw_prov_load(&c)) memset(&c, 0, sizeof(c));
        snprintf(lines[1], sizeof(lines[1]), "3. Ham %s", c.call[0] ? c.call : "(none)");
        snprintf(lines[2], sizeof(lines[2]), "   Virtual %s", c.vcall[0] ? c.vcall : "(none)");
    } else {
        snprintf(lines[1], sizeof(lines[1]), "3. Fill in your home Wi-Fi");
        snprintf(lines[2], sizeof(lines[2]), "   and press save");
    }
    for (int i = 0; i < 3; i++) {
        lv_obj_t *l = lv_label_create(scr);
        lv_label_set_text(l, lines[i]);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0xC8D6E5), 0);
        lv_obj_set_pos(l, 8, 186 + i * 22);
    }

    // 这一行既是提示也是当前值：配网屏上没有别的入口调亮度，得写明 UP/DN 能调。
    s_bl_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_bl_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_bl_lbl, lv_color_hex(0x8FA3B8), 0);
    lv_obj_set_pos(s_bl_lbl, 8, 262);
    bl_label_update();

    // 跳过配网的入口得写在屏幕上：配网屏没有别的提示，不写没人知道能跳过。
    // 手头没有中文字形（Montserrat 没有），这里照旧用英文。
    lv_obj_t *skip = lv_label_create(scr);
    lv_label_set_text(skip, "HOLD OK TO SKIP");
    lv_obj_set_style_text_font(skip, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(skip, lv_color_hex(0x7A8CA0), 0);
    lv_obj_set_pos(skip, 8, 278);


    lv_screen_load(scr);
    s_bl_last_ms = esp_timer_get_time() / 1000;
    s_bl_timer = lv_timer_create(bl_tick_cb, 100, NULL);
    s_prov_active = true;              // 从此刻起按键归配网屏（调亮度 / 唤醒）
    bsp_lvgl_unlock();
}

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------
const char *cw_prov_ap_ssid(void) { return s_ap_ssid; }
const char *cw_prov_ap_pass(void) { return s_ap_pass; }

esp_err_t cw_prov_run(cw_prov_mode_t mode) {
    (void)nvs_ensure();
    s_mode = mode;

    // 配网屏不出声，也没起音频任务：codec 开着却没有 PCM 喂它，喇叭容易哼出
    // 高频底噪。这里直接走完整的 ES8311 suspend 序列把它关掉，顺便省电。
    // 配完网是整机重启，重启后 bsp_audio_init() 会重新配好，不受影响。
    (void)bsp_audio_sleep();

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "CW-%02X%02X", mac[4], mac[5]);
    ap_pass_ensure();

    esp_netif_init();
    esp_event_loop_create_default();          // 没建的话 esp_netif_create_default_wifi_ap 会 abort
    if (!esp_netif_create_default_wifi_ap()) return ESP_FAIL;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t e = esp_wifi_init(&cfg);
    if (e != ESP_OK) return e;

    wifi_config_t wc = { 0 };
    memcpy(wc.ap.ssid, s_ap_ssid, strlen(s_ap_ssid));
    wc.ap.ssid_len = (uint8_t)strlen(s_ap_ssid);
    wc.ap.max_connection = 4;
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;      // 配网热点也要密码，屏幕上随机显示
    memcpy(wc.ap.password, s_ap_pass, strlen(s_ap_pass));
    wc.ap.channel = 1;
    e = esp_wifi_set_mode(WIFI_MODE_AP);
    if (e == ESP_OK) e = esp_wifi_set_config(WIFI_IF_AP, &wc);
    if (e == ESP_OK) e = esp_wifi_start();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "SoftAP 启动失败: %s", esp_err_to_name(e));
        return e;
    }
    ESP_LOGI(TAG, "配网热点已启动: %s  密码 %s  网关 %s（%s）",
             s_ap_ssid, s_ap_pass, AP_IP_STR,
             s_mode == CW_PROV_SERVER ? "只改服务器地址" :
             s_mode == CW_PROV_CALL   ? "只改 Ham 呼号" : "完整配网");

    // 回读一次鉴权模式：万一密码被判为无效（比如短于 8 位），set_config 会静默
    // 降级成开放热点，屏幕上却照样显示密码 —— 那样既不保密又让人白敲半天。
    wifi_config_t chk = { 0 };
    if (esp_wifi_get_config(WIFI_IF_AP, &chk) == ESP_OK) {
        ESP_LOGI(TAG, "鉴权模式回读: %d (3=WPA2-PSK，0 就是开放热点，得查)", (int)chk.ap.authmode);
    }

    s_done = xSemaphoreCreateBinary();
    if (xTaskCreate(dns_task, "cw_dns", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "DNS 任务创建失败，强制门户可能不弹，直接访问 %s 仍可用", AP_IP_STR);
    }

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.uri_match_fn = httpd_uri_match_wildcard;   // 让 "*" 兜住所有探测路径
    httpd_handle_t hd = NULL;
    if (httpd_start(&hd, &hc) == ESP_OK) {
        // 注册顺序即匹配顺序：先精确后通配。
        httpd_uri_t u_root = { .uri = "/", .method = HTTP_GET, .handler = h_root };
        httpd_uri_t u_save = { .uri = "/save", .method = HTTP_POST, .handler = h_save };
        httpd_uri_t u_any  = { .uri = "*", .method = HTTP_GET, .handler = h_redirect };
        (void)httpd_register_uri_handler(hd, &u_root);
        (void)httpd_register_uri_handler(hd, &u_save);
        (void)httpd_register_uri_handler(hd, &u_any);
    } else {
        ESP_LOGE(TAG, "HTTP 服务器启动失败");
    }

    // 亮度用上次存的值：配网屏上往往就是第一次调亮度的地方，不该配完网就丢。
    uint8_t b = 0;
    if (cw_prov_load_bl(&b) && b >= BL_MIN && b <= BL_MAX) s_bl = b;
    // 超时同样沿用菜单里选的那一档；没存过就用默认 30 秒。0 = 常亮不熄。
    uint8_t tv = 0;
    s_bl_to_ms = cw_prov_load_blto(&tv) ? (int)tv * 1000 : BL_TO_MS_DEF;
    ESP_LOGI(TAG, "背光: %d%% · 超时 %d 秒（0 = 常亮）", (int)s_bl, s_bl_to_ms / 1000);
    bsp_display_backlight(s_bl);

    prov_screen();

    // 等用户在页面上保存，或者长按 OK 跳过。拿到信号量后再缓一拍，
    // 让"保存成功"的响应（或跳过时屏幕上那句话）发完/显示完再重启。
    if (s_done) xSemaphoreTake(s_done, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(1200));
    if (s_skip) {
        // ★ 跳过必须把 force / srvmode 一起清掉。这两个标记的含义是"下次开机拉我进
        //   配网"，跳过的含义是"这次别配了"，两者留在 NVS 里会互相打架：每次重启
        //   都被拉回配网屏，跳过一次只能换来一次正常开机，下次照旧 —— 表现为
        //   "怎么又进配网了"。用户想配网随时能在菜单里选 WI-FI SETUP / BASE STATION。
        set_u8("force", 0);
        set_u8("srvmode", 0);
        set_u8("callmode", 0);
        ESP_LOGW(TAG, "已跳过配网，重启进入电台（离线）");
    } else {
        ESP_LOGI(TAG, "凭据已保存，重启进入 STA 模式");
    }
    esp_restart();
    return ESP_OK;
}
