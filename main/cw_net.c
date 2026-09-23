// main/cw_net.c —— Wi-Fi STA + UDP 键控通道 + MQTT 信令通道。
//
// 两条通路的分工（这是本项目的核心传输决策，别轻易合并）：
//   UDP  16B 二进制帧：键控 keydown/keyup。可丢不可迟 —— 丢了只是少一个点划，
//        迟到了会打乱节奏比例，且重传到达时播放时刻已过，纯属制造"追赶播放"。
//   MQTT over TCP：presence（含 LWT 遗嘱）、台站名单、频道占用表、对时、短报文。
//        这些都是"可迟不可丢"的信令，MQTT 的 retain 与 LWT 正好白拿。
#include "cw_net.h"
#include "cw_proto.h"
#include "cw_radio.h"
#include "cw_prov.h"      // 凭据（Wi-Fi / 服务器地址 / 呼号）存在 NVS 里，由配网页写入

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "nvs_flash.h"
// WebSocket：地址栏填 wss:// 时，键控与 MQTT 都搬到它上面（为了过 Cloudflare）。
#include "esp_websocket_client.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"   // getaddrinfo：服务器那一栏现在可以填域名，得靠它解析
#include "lwip/ip_addr.h"
#include "lwip/dns.h"     // dns_get_server()：体检时看 lwIP 内部真正在用哪个 DNS
#include "lwip/api.h"     // netconn_gethostbyname_addrtype()：比 getaddrinfo 多给一个原始错误码

#include <string.h>
#include <stdlib.h>

static const char *TAG = "cw_net";

#define CW_WIFI_CONNECTED_BIT BIT0
#define CW_WIFI_FAIL_BIT      BIT1
#define CW_HEARTBEAT_MS       5000
#define CW_HELLO_RETRY_MS     3000
// 认证帧的重发间隔。UDP 会丢，且这是开机后唯一能拿到虚拟呼号的途径，
// 收不到 ack 就得一直重发 —— 否则设备永远停在"未认证"，连通联的门都进不去。
#define CW_AUTH_RETRY_MS      3000
// presence 发布的最小间隔。原来是 1500，结果连发调频时只有头一格上报，
// 中间全被吞掉、末值要等下一次心跳（5 s）—— 手感上就是"松手了频率才过去"。
// 现在改成 150：连发 110 ms 一格，大约每格都能报一次，最多隔一格。
#define CW_PRESENCE_MIN_MS    150
// 运行期巡检的容忍窗口：链路断了这么久还不回来，就当这一轮废了、整轮重来。
// 60 s 是留给 Cloudflare 的 —— 一次完整的 TLS 握手实测就要 7~30 s，给短了会在网络
// 只抖了一下的时候反复推倒重来，反而更糟。只用于 WebSocket 模式（UDP 那一路没有
// 可靠的可达性信号：socket 创建出来就永远"就绪"，看不出服务器还在不在）。
#define CW_LINK_LOST_MS       60000
// 注册成功后仍然定期补发 hello。不是为了保活（心跳在做），而是给服务端一个把呼号
// 改回来的机会：撞名时它会把我的名字改成 GH1BHU2 这种，等撞的那个走了也改不回来 ——
// 名字是在 hello 里认的，不发 hello 就永远纠正不了。20 s 一次，一帧而已。
#define CW_REHELLO_MS         20000

// ★ 同频判定："频道"不是一个点，而是一格。两台设备的频率差不超过这个数就算同频，
//   互相收得到（真机上就是"都落在对方接收机通带里"）。100 Hz 与服务端 BUCKET 一致，
//   改这一处要同步改 cw-server/server.js 的 CHANNEL_TOL。
//   以前是 ±450 Hz 的"零拍通带"，失谐半千赫还能听见 —— 那不是电台，是对讲机。
#define CW_CHANNEL_TOL_HZ     100

// WebSocket 模式下键控通道的路径（服务端按 upgrade 的 path 分派：/cw 设备、/mqtt 信令、
// /ws 网页）。二进制帧与 UDP 逐字节相同，所以协议解析那套代码一行都不用改。
#define CW_WS_PATH           "/cw"
// 一条 CW 帧最大 320 字节（频谱帧 10+3×80），缓冲留到 512 富余。
#define CW_WS_ASM_MAX        512
// 接收队列：WS 的回调跑在组件自己的任务里，不能直接在那儿调 LVGL 相关的处理，
// 所以攒成帧队列、由 net_task 取走（跟 UDP 那条路的处理时序保持一致）。
#define CW_RXQ_FRAMES        6
#define CW_RXQ_LEN           320

static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t *s_sta_netif;
static bool s_wifi_started;
static bool s_nvs_ready;

static int      s_sock = -1;
static uint16_t s_uid;
static uint16_t s_seq;
static uint32_t s_freq = 7024200;
static int      s_wpm = 18;
// ---- 三种呼号 ----
// Ham      ：用户在配网页里填的真实呼号。没填就是空串 —— 空是有意义的，屏幕上显示 "--"。
// Virtual  ：服务器按 Global UID 下发的 6 位号，永久绑定，本机改不了。没拿到 = 空串。
// s_call   ："拿去跟服务端打交道的那个" = Virtual 优先，没有才用 Ham，都没有是空串。
//            显示顺序相反（Ham 优先），见 cw_radio.c 的 top_call()。
// ★ 没有第三档兜底：以前会用 MAC 尾段凑一个 Nxxxx，看着像有个呼号，其实是假的 ——
//   别人名单里会出现一个从没被分配过的名字。现在一律留空，由界面自己决定显示 "--" 还是不显示。
static char     s_call[CW_CALL_LEN + 1];              // 见 apply_call：Ham → Virtual → 空
static char     s_ham[CW_CALL_LEN + 1];               // 用户自定义真实呼号，空 = 没配过
static char     s_vcall[CW_CALL_LEN + 1];             // 服务器下发虚拟呼号，空 = 还没拿到
static bool     s_authed;                             // Global UID 认证通过了吗
// 服务器地址与 MQTT 地址：配网页里填的优先，没填就用 Kconfig 编译值兜底。
// ★ 这一栏可以是 IP（192.168.1.10）也可以是域名（cw_station.bubblegear.xyz）——
//   域名让"换服务器只要改一个字符串"成为可能，也让 OTA 后还能换机房而不用重烧。
//   64 字节跟着 CW_PROV_SRV_MAX，别再写小：长域名会被 snprintf 悄悄截断，
//   截出来的字符串还是个合法域名，只是解析失败，很难从现象反推。
static char     s_srv_host[64];
static char     s_mqtt_uri[80];
// ---- 传输模式：由地址栏的前缀决定 ----
//   cw_station.bubblegear.xyz  → 明文 UDP 键控（老路子）
//   ws://host                  → WebSocket 明文（本机调试用）
//   wss://host                 → WebSocket + TLS，三条路全走 443（过 Cloudflare 用这个）
// 解析出"主机名 / 端口 / 走不走 WS / 要不要 TLS"这四样，三条通路都按它来。
static bool     s_ws_en;                 // 键控走 WebSocket 吗
static bool     s_ws_tls;                // 是 wss:// 吗（决定要不要校验证书）
static char     s_ws_host[64];           // 剥掉前缀后的主机名
static int      s_ws_port;               // 443 / 80
static esp_websocket_client_handle_t s_ws;
static volatile bool s_ws_up;            // 连接建立了吗（回调里置位，net_task 里读）
// WS 收到的帧先由回调拼好，再放进这个环形队列，等 net_task 来取。
static uint8_t  s_rxq[CW_RXQ_FRAMES][CW_RXQ_LEN];
static int      s_rxq_len[CW_RXQ_FRAMES];
static volatile int s_rxq_head;          // 生产者：WS 回调任务
static volatile int s_rxq_tail;          // 消费者：net_task
// 组帧用的临时缓冲（一个 payload 可能被拆成多次 EVENT_DATA 给过来）。
static uint8_t  s_asm[CW_WS_ASM_MAX];
static int      s_asm_len;
// 队列读写跨两个任务（WS 回调任务写、net_task 读），得有把锁。
// ★ 用 portENTER_CRITICAL 而不是 taskENTER_CRITICAL：RISC-V 端口上后者展开成
//   portENTER_CRITICAL(x) 却没给参数，直接编译不过。
static portMUX_TYPE s_rxq_mux = portMUX_INITIALIZER_UNLOCKED;
static char     s_call_cfg[CW_CALL_LEN + 1];
static volatile bool s_run;
// 在线/离线：由 UI 层（组合键）切换。离线时 presence 保持一条 retain 的 "0"，
// 服务器据此把我从别人的名单里摘掉；键控、频率上报也一并停掉。
// UDP 心跳照发 —— 它只用来保住 uid 不被服务端回收，不含键控。
// 出厂默认是离线：开机只连 Wi-Fi、只注册，不进别人的名单、也不发键控，
// 要通联得先按【UP+DOWN】上线（屏幕外圈变绿）。以前默认在线，一开机就把自己
// 挂到公网名单上，不合"上电先听着"的习惯。
static bool     s_online = false;
static TaskHandle_t s_task;
static esp_mqtt_client_handle_t s_mqtt;
static struct sockaddr_in s_srv;
static int64_t s_last_presence;
// 下次可以发认证帧的时刻。收到 uid=0 的 authack 时把它清零，逼下一轮立刻重发。
static int64_t s_reauth_ms;
// 调频被节流挡掉的末值：由 net_task 补发一次，保证最终落在手离开时的频率上。
static volatile bool    s_tune_dirty;
static volatile int64_t s_tune_last;

// ---------------------------------------------------------------------------
// 配置：优先用 Kconfig 默认值；呼号不足 3 位时用芯片 MAC 尾段兜底，避免重名。
// ---------------------------------------------------------------------------
// 通联用的呼号（进协议、进 MQTT 主题的那个）：虚拟呼号优先，没有才拿 Ham 顶着，
// 都没有就是空串。★ 注意它和屏幕上显示的顺序是反的 —— 界面是 Ham 优先（用户更想
// 看见自己的号），协议这边必须虚拟优先：presence 的主题是 cw/v1/sta/<呼号>/presence，
// hello 帧也带呼号，服务端两处都拿它跟 st.call（它自己下发的虚拟号）比对，
// 报 Ham 上去对不上，在线状态和调频就全丢了。
// ★ 也不拿 MAC 凑数：凑出来的号没经过任何分配，拿着它进频道等于冒名。
static void apply_call(void) {
    if (s_vcall[0])    snprintf(s_call, sizeof(s_call), "%s", s_vcall);
    else if (s_ham[0]) snprintf(s_call, sizeof(s_call), "%s", s_ham);
    else               s_call[0] = '\0';
}

static void build_callsign(void) {
    // Ham 只有一个来源：配网页里填的那一栏。没填过就留空 ——
    // 不用 Kconfig 的编译默认值，也不用 MAC 凑，屏幕上老老实实显示 "--"。
    const char *cfg = s_call_cfg;
    size_t n = 0;
    for (size_t i = 0; cfg[i] && n < CW_CALL_LEN; i++) {
        char c = cfg[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) s_ham[n++] = c;
    }
    s_ham[n] = '\0';
    if (n < 3) s_ham[0] = '\0';         // 太短（占位符之类）就当没配过
    apply_call();
}

const char *cw_net_call(void)   { return s_call; }
const char *cw_net_ham(void)    { return s_ham; }
const char *cw_net_vcall(void)  { return s_vcall; }

// hello 里报的必须是虚拟呼号：服务端拿它和名单里的 st.call 对，对不上就当没认证过，
// 立刻回一个 uid=0 逼重新认证 —— 20 s 一轮的补发会变成 20 s 一次的重认证循环。
// s_call 可能还停在 Ham 上（用户填了呼号时），所以这里不能直接用它。
static const char *hello_call(void) { return s_vcall[0] ? s_vcall : s_call; }
uint16_t    cw_net_uid(void)    { return s_uid; }
bool        cw_net_authed(void) { return s_authed; }

// ---------------------------------------------------------------------------
// 设备身份：出厂 MAC → 设备 ID + MAC 串 + 本机 IP
// ---------------------------------------------------------------------------
// ★ 设备 ID 就是 MAC 的 48 位，与 Global UID 是同一个东西，只是两种写法：
//     Global UID = 线上发的那 8 字节（前 6 字节是 MAC，后 2 字节保留填 0），
//     Device ID  = 屏幕上给人看的 12 位十六进制（4C11AE2F8DC8）。
//   服务端只认前 6 字节 —— 它把身份键统一截到 48 位，两种写法落到同一条记录上。
static char     s_mac_str[18] = "--:--:--:--:--:--";
static uint64_t s_dev_id;                        // MAC 的 48 位，高 16 位恒为 0
static char     s_dev_id_hex[13];                // "4C11AE2F8DC8"
// Global UID：8 字节 = MAC 的 48 位 + 2 字节保留（填 0）。
// 服务端拿它当数据库主键，一板一号，呼号跟它永久绑定。
static uint8_t  s_guid[CW_GUID_LEN];
static char     s_guid_hex[CW_GUID_LEN * 2 + 1];

// 幂等：算一次存下来，两页要显示时直接取，不必每次都读 eFuse。
static void build_device_info(void) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_mac_str, sizeof(s_mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    s_dev_id = ((uint64_t)mac[0] << 40) | ((uint64_t)mac[1] << 32) |
               ((uint64_t)mac[2] << 24) | ((uint64_t)mac[3] << 16) |
               ((uint64_t)mac[4] << 8)  | ((uint64_t)mac[5]);
    // 12 位十六进制。一次写 3 个字符（snprintf 自带结尾 '\0'，下一次从后往前盖）。
    for (int i = 0; i < 6; i++)
        snprintf(s_dev_id_hex + i * 2, 3, "%02X", mac[i]);
    memset(s_guid, 0, sizeof(s_guid));
    memcpy(s_guid, mac, 6);
    for (int i = 0; i < CW_GUID_LEN; i++)
        snprintf(s_guid_hex + i * 2, 3, "%02X", s_guid[i]);
}

uint64_t    cw_net_device_id(void)     { return s_dev_id; }
const char *cw_net_device_id_hex(void) { return s_dev_id_hex; }
const char *cw_net_mac_str(void)       { return s_mac_str; }
const uint8_t *cw_net_guid(void)       { return s_guid; }
const char *cw_net_guid_hex(void)      { return s_guid_hex; }

const char *cw_net_ip_str(void) {
    static char ip[16];
    if (!s_sta_netif) { snprintf(ip, sizeof(ip), "--"); return ip; }
    esp_netif_ip_info_t info;
    // addr == 0 是"接口起来了但还没拿到地址"，跟 esp_netif 调用失败一样都显示为 --
    if (esp_netif_get_ip_info(s_sta_netif, &info) != ESP_OK || info.ip.addr == 0) {
        snprintf(ip, sizeof(ip), "--");
        return ip;
    }
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
    return ip;
}

// ---------------------------------------------------------------------------
// Wi-Fi
// ---------------------------------------------------------------------------
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_run) esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, CW_WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_start(void) {
    if (!s_nvs_ready) {
        esp_err_t e = nvs_flash_init();
        if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            // 分区表被改动过才需要擦除；这里与仓库约定一致：不因初始化失败擦用户数据。
            ESP_LOGW(TAG, "NVS 需要重建: %s", esp_err_to_name(e));
            return e;
        }
        if (e != ESP_OK) return e;
        s_nvs_ready = true;
    }
    if (!s_wifi_event_group) s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) return ESP_ERR_NO_MEM;

    // esp_netif / 默认事件循环必须先建好：esp_netif_create_default_wifi_sta() 内部会
    // 调 esp_event_handler_register()，事件循环没建时会返回 ESP_ERR_INVALID_STATE，
    // 而它用了 ESP_ERROR_CHECK —— 直接 abort，不是返回错误码。
    esp_err_t netif_err = esp_netif_init();
    if (netif_err != ESP_OK && netif_err != ESP_ERR_INVALID_STATE) return netif_err;

    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) return loop_err;

    if (!s_sta_netif) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (!s_sta_netif) return ESP_ERR_NO_MEM;
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t e = esp_wifi_init(&cfg);
    if (e != ESP_OK) return e;

    e = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    if (e != ESP_OK) return e;
    e = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);
    if (e != ESP_OK) return e;

    wifi_config_t wc = { 0 };
    // 配网页存过的凭据优先；没配过就退回编译期写死的 Kconfig 值（首次烧录的默认值）。
    cw_cred_t cred;
    if (cw_prov_load(&cred)) {
        strncpy((char *)wc.sta.ssid, cred.ssid, sizeof(wc.sta.ssid) - 1);
        strncpy((char *)wc.sta.password, cred.pass, sizeof(wc.sta.password) - 1);
    } else {
        strncpy((char *)wc.sta.ssid, CONFIG_CW_WIFI_SSID, sizeof(wc.sta.ssid) - 1);
        strncpy((char *)wc.sta.password, CONFIG_CW_WIFI_PASSWORD, sizeof(wc.sta.password) - 1);
    }
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_LOGI(TAG, "连接 Wi-Fi: %s", (char *)wc.sta.ssid);

    e = esp_wifi_set_mode(WIFI_MODE_STA);
    if (e != ESP_OK) return e;
    e = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (e != ESP_OK) return e;
    e = esp_wifi_start();
    if (e == ESP_OK) s_wifi_started = true;
    return e;
}

static esp_err_t wifi_wait(uint32_t timeout_ms) {
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           CW_WIFI_CONNECTED_BIT | CW_WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (bits & CW_WIFI_CONNECTED_BIT) return ESP_OK;
    return bits & CW_WIFI_FAIL_BIT ? ESP_FAIL : ESP_ERR_TIMEOUT;
}

// ---------------------------------------------------------------------------
// MQTT
// ---------------------------------------------------------------------------
static void publish_presence(int online) {
    if (!s_mqtt) return;
    char topic[64], payload[64];
    snprintf(topic, sizeof(topic), "cw/v1/sta/%s/presence", s_call);
    if (online) snprintf(payload, sizeof(payload), "%s,%u,%d", s_call, (unsigned)s_freq, s_wpm);
    else snprintf(payload, sizeof(payload), "0");
    int qos = 1, retain = 1;
    esp_mqtt_client_publish(s_mqtt, topic, payload, 0, qos, retain);
    s_last_presence = esp_timer_get_time() / 1000;
}

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base;
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)data;
    switch (ev->event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT 已连接");
        esp_mqtt_client_subscribe(s_mqtt, "cw/v1/roster", 0);
        esp_mqtt_client_subscribe(s_mqtt, "cw/v1/occupy", 0);
        esp_mqtt_client_subscribe(s_mqtt, "cw/v1/time", 0);
        esp_mqtt_client_subscribe(s_mqtt, "cw/v1/chat", 0);
        // 重连时按当前在线状态报：离线期间 MQTT 重连不能把我"自动上线"。
        publish_presence(s_online ? 1 : 0);
        cw_radio_on_net_state(s_uid ? CW_NET_LINK : CW_NET_WIFI);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT 断开");
        cw_radio_on_net_state(CW_NET_WIFI);
        break;
    case MQTT_EVENT_DATA: {
        if (!ev->topic) break;
        size_t tlen = (size_t)ev->topic_len;
        if (tlen >= 12 && strncmp(ev->topic, "cw/v1/roster", 12) == 0) {
            // 拷成以 '\0' 结尾的字符串再交给 UI 层解析
            char *copy = malloc(ev->data_len + 1);
            if (copy) {
                memcpy(copy, ev->data, ev->data_len);
                copy[ev->data_len] = '\0';
                cw_radio_on_roster(copy);
                free(copy);
            }
        } else if (tlen >= 12 && strncmp(ev->topic, "cw/v1/occupy", 12) == 0) {
            // MQTT 上运的是跟 UDP 同一套 occupy 帧（含帧头），所以走同一个专用入口，
            // 不能直接把 payload 当"格子数组"用 —— 它现在是稀疏三元组流。
            cw_frame_t o;
            if (cw_proto_parse_occupy((const uint8_t *)ev->data, (size_t)ev->data_len, &o) && o.n > 0)
                cw_radio_on_occupy(o.bins, o.n);
        } else if (tlen >= 10 && strncmp(ev->topic, "cw/v1/time", 10) == 0) {
            char buf[24] = { 0 };
            size_t n = ev->data_len < sizeof(buf) - 1 ? (size_t)ev->data_len : sizeof(buf) - 1;
            memcpy(buf, ev->data, n);
            cw_radio_on_time(strtoll(buf, NULL, 10));
        } else if (tlen >= 10 && strncmp(ev->topic, "cw/v1/chat", 10) == 0) {
            char buf[80] = { 0 };
            size_t n = ev->data_len < sizeof(buf) - 1 ? (size_t)ev->data_len : sizeof(buf) - 1;
            memcpy(buf, ev->data, n);
            char *sep = strchr(buf, ':');
            if (sep) { *sep = '\0'; cw_radio_on_chat(buf, sep + 1); }
        }
        break;
    }
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// ★ 地址栏怎么解释：带 ws:// / wss:// 前缀就把三条通路整个搬到 WebSocket 上。
//   这么设计是为了让"直连"和"过 Cloudflare"两种部署共用一份固件 —— 换模式只要改地址，
//   不用重新烧。改错也不会变砖：填回裸域名就退回 UDP。
// ---------------------------------------------------------------------------
static void parse_server_scheme(void) {
    s_ws_en = false;
    s_ws_tls = false;
    s_ws_port = CONFIG_CW_UDP_PORT;
    snprintf(s_ws_host, sizeof(s_ws_host), "%s", s_srv_host);

    if (strncmp(s_srv_host, "wss://", 6) == 0) {
        s_ws_en = true; s_ws_tls = true;
        snprintf(s_ws_host, sizeof(s_ws_host), "%s", s_srv_host + 6);
        s_ws_port = 443;
    } else if (strncmp(s_srv_host, "ws://", 5) == 0) {
        s_ws_en = true;
        snprintf(s_ws_host, sizeof(s_ws_host), "%s", s_srv_host + 5);
        s_ws_port = 80;
    }
    // 顺手去掉可能带上的尾巴（有人会填 wss://host:443 或 wss://host/cw 这种）
    char *p = strchr(s_ws_host, '/');
    if (p) *p = '\0';
}

bool cw_net_use_ws(void)   { return s_ws_en; }
const char *cw_net_srv_host(void) { return s_ws_en ? s_ws_host : s_srv_host; }
int  cw_net_fw_port(void)  { return s_ws_en ? s_ws_port : (int)CW_FW_PORT; }
// ★ CONFIG_CW_TLS 关着的时候 sdkconfig.h 里根本没有这个宏，只能走预处理器判断 ——
//   写成 (bool)CONFIG_CW_TLS 会在关着的时候编不过（未定义的标识符）。
bool cw_net_fw_tls(void) {
    if (s_ws_en) return s_ws_tls;
#if CONFIG_CW_TLS
    return true;
#else
    return false;
#endif
}

// MQTT 地址也由那一个 host 派生：scheme 与端口跟着传输模式走（常量见 cw_net.h）。
// s_mqtt_uri 现在只用来打日志（见 mqtt_start），不再喂给 esp-mqtt —— 原因见下面那段。
static void build_mqtt_uri(void) {
    if (s_ws_en) {
        snprintf(s_mqtt_uri, sizeof(s_mqtt_uri), "%s://%s:%d/mqtt",
                 s_ws_tls ? "wss" : "ws", s_ws_host, s_ws_port);
    } else {
        snprintf(s_mqtt_uri, sizeof(s_mqtt_uri), "%s://%s:%d",
                 CW_MQTT_SCHEME, s_srv_host, CW_MQTT_PORT);
    }
}

static void mqtt_start(void) {
    // ★ MQTT 要建第二条 TLS 连接（WebSocket 那条已经占了一份），内存不够时
    //   mbedtls_ssl_setup 会返回 -0x7F00(ALLOC_FAILED)，日志上只显示
    //   "Error transport connect"。把水位打出来，省得下次再从头查一遍。
    ESP_LOGI(TAG, "建 MQTT 前：剩余堆 %u 字节 · 历史最低 %u 字节",
             esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
    char lwt_topic[64];
    snprintf(lwt_topic, sizeof(lwt_topic), "cw/v1/sta/%s/presence", s_call);

    // ★ 这里填 hostname + port，而不是拼好的 URI —— 踩过的坑：
    //   域名里带下划线（cw_station.bubblegear.xyz）时，esp-mqtt 的 URI 解析器直接报
    //   "Error parse uri (1)" 并让 esp_mqtt_client_init 返回 NULL，整个信令通道就没了。
    //   而 UDP 那一路走 getaddrinfo 是容忍下划线的，于是出现"认证通过了但信令通道始终
//   起不来"的怪象 —— 状态行的 STATION 一直是黄的。
    //   直接给 hostname 就绕开了 URI 解析这一步，底下照样是 getaddrinfo。
    //   （下划线主机名本身不合规：RFC 1123 只允许字母、数字和连字符，建议域名改用连字符。）
    esp_mqtt_client_config_t cfg = {
        .broker.address.hostname = cw_net_srv_host(),
        .broker.address.port     = s_ws_en ? (uint32_t)s_ws_port : (uint32_t)CW_MQTT_PORT,
        .credentials.client_id = s_call,
        // 掉电/断网时由 broker 代发：别人的名单里立刻看不到我。
        .session.last_will.topic = lwt_topic,
        .session.last_will.msg = "0",
        .session.last_will.msg_len = 1,
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
        .session.keepalive = 30,
        .network.disable_auto_reconnect = false,
        // ★ 默认 10 s 撑不过 Cloudflare 的 TLS 握手（走 /cw 那条 WS 实测要十几秒），
        //   结果就是每 11 s 一次 "Error transport connect" 却看不出原因。放到 25 s。
        .network.timeout_ms = 25000,
    };
    // ★ 校验证书这一步不能图省事跳过：不校验的 TLS 只加密、不认服务器是谁，
    //   中间人拿一张自签证书就能冒充 broker。那比明文更糟 —— 明文至少一眼看得出来。
    if (s_ws_en) {
        // 走 Cloudflare 时 1883 那个裸 TCP 口根本不通（CF 不代理），只能把 MQTT 塞进
        // WebSocket。刚好也绕开了 URI 解析：hostname 是分开给的。
        cfg.broker.address.transport = s_ws_tls ? MQTT_TRANSPORT_OVER_WSS : MQTT_TRANSPORT_OVER_WS;
        cfg.broker.address.path      = "/mqtt";
        if (s_ws_tls) cfg.broker.verification.certificate = CW_ROOT_CA_PEM;
    } else {
#if CONFIG_CW_TLS
        cfg.broker.address.transport = MQTT_TRANSPORT_OVER_SSL;
        cfg.broker.verification.certificate = CW_ROOT_CA_PEM;
#else
        cfg.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
#endif
    }
    s_mqtt = esp_mqtt_client_init(&cfg);
    if (!s_mqtt) { ESP_LOGE(TAG, "MQTT 客户端创建失败（uri=%s）", s_mqtt_uri); return; }
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt);
}

// ---------------------------------------------------------------------------
// UDP
// ---------------------------------------------------------------------------
// ★ 把配置里那一栏服务器地址（IP 或域名）变成 UDP 能用的 sockaddr。
//   以前是 inet_addr()：它只认点分十进制，填域名会返回 INADDR_NONE，
//   也就是把 255.255.255.255 当成目标地址 —— 帧全发到广播上，且看不出哪里出错。
//   所以这里分两步：inet_addr 先跑（配 IP 的人还是大多数，这样就完全不碰 DNS），
//   失败才上 getaddrinfo。
static bool resolve_server(uint16_t port) {
    uint32_t ip = inet_addr(s_srv_host);
    if (ip != INADDR_NONE) {                 // 纯 IPv4：不用 DNS
        s_srv.sin_addr.s_addr = ip;
        return true;
    }
    // AF_INET：服务端现在是 udp4 的 socket，先不支持 IPv6。
    // ai_socktype 必须给 —— lwIP 的 getaddrinfo 用 hints 过滤结果，留空可能拿到
    // 匹配不上的类型。
    const struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM };
    struct addrinfo *res = NULL;
    int e = getaddrinfo(s_srv_host, NULL, &hints, &res);
    if (e != 0 || !res) {
        ESP_LOGW(TAG, "DNS 解析 %s 失败（err=%d）", s_srv_host, e);
        return false;
    }
    struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
    s_srv.sin_addr.s_addr = sa->sin_addr.s_addr;
    ESP_LOGI(TAG, "%s -> " IPSTR, s_srv_host, IP2STR((const ip4_addr_t *)&sa->sin_addr));
    freeaddrinfo(res);
    return true;
}

// ---------------------------------------------------------------------------
// ★ DNS 体检（1.1.15 加的诊断：定位 WS 模式下 esp-tls 报 getaddrinfo 202 的根因）
//   esp-tls 内部是写死的 AF_UNSPEC + SOCK_STREAM，而 UDP 这一路用的是 AF_INET +
//   SOCK_DGRAM。两边在同一台设备上结果不一样时，光看"连不上"是分不出来的 ——
//   这里把三种组合各跑一遍，顺便把 DHCP 下发的 DNS 服务器打出来。
// ---------------------------------------------------------------------------
static void raw_dns_query(const char *name, const char *srv);   // 定义在下面

static void dns_probe(const char *host) {
    char dns0[16] = "192.168.31.1";        // 兜底值，下面读到真的会被覆盖
    esp_netif_t *netif = s_sta_netif;
    if (netif) {
        for (int i = 0; i < 2; i++) {
            esp_netif_dns_info_t dns = { 0 };
            esp_netif_dns_type_t type = (i == 0) ? ESP_NETIF_DNS_MAIN : ESP_NETIF_DNS_BACKUP;
            if (esp_netif_get_dns_info(netif, type, &dns) != ESP_OK) continue;
            if (dns.ip.type != ESP_IPADDR_TYPE_V4) { ESP_LOGI(TAG, "DNS%d = 非 IPv4", i); continue; }
            ESP_LOGI(TAG, "DNS%d(netif) = " IPSTR, i, IP2STR(&dns.ip.u_addr.ip4));
            if (i == 0) snprintf(dns0, sizeof(dns0), IPSTR, IP2STR(&dns.ip.u_addr.ip4));
        }
    }
    // lwIP 内部真正在用的那几个（netif 里读到不代表 lwIP 拿到了，两边要对齐看）
    for (int i = 0; i < 2; i++) {
        const ip_addr_t *s = dns_getserver((u8_t)i);
        if (s && !ip_addr_isany(s)) ESP_LOGI(TAG, "DNS%d(lwip)  = " IPSTR, i, IP2STR(ip_2_ip4(s)));
        else                        ESP_LOGI(TAG, "DNS%d(lwip)  = 未设置", i);
    }
    // 对照域名：目标域名解不开、别的能解开 —— 那是服务器/上游的锅；
    // 全都解不开 —— 那是 lwIP 的 DNS 模块压根没发出查询。
    static const char *probes[4] = { NULL, "cloudflare.com", "www.baidu.com", "104.21.10.166" };
    probes[0] = host;
    for (int i = 0; i < 4; i++) {
        ip_addr_t addr;
        memset(&addr, 0, sizeof(addr));
        int64_t t0 = esp_timer_get_time();
        err_t e = netconn_gethostbyname_addrtype(probes[i], &addr, NETCONN_DNS_IPV4);
        int ms = (int)((esp_timer_get_time() - t0) / 1000);
        if (e == ERR_OK) {
            ESP_LOGI(TAG, "DNS体检 %-28s -> " IPSTR " (%d ms)", probes[i],
                     IP2STR(ip_2_ip4(&addr)), ms);
        } else {
            // err 码比 getaddrinfo 的 202 有信息量：ERR_ARG(-15)=DNS 未初始化、
            // ERR_MEM(-1)=表满/内存、ERR_TIMEOUT(-3)=发了没回、ERR_INPROGRESS(-5)=不该出现
            ESP_LOGW(TAG, "DNS体检 %-28s -> 失败 err=%d (%d ms)", probes[i], (int)e, ms);
        }
    }
    // 手工发一包看网络上到底回了什么：路由器 DNS / 阿里 / 谷歌 各问一次目标域名，
    // 再拿 cloudflare.com 做对照（它解得出，说明链路本身是通的）。
    raw_dns_query(host, dns0);
    raw_dns_query(host, "223.5.5.5");
    raw_dns_query(host, "8.8.8.8");
    raw_dns_query("cloudflare.com", "223.5.5.5");
}

// ---------------------------------------------------------------------------
// ★ 原始 DNS 查询（1.1.15 诊断）：自己手工拼一个 A 查询包发出去，绕开 lwIP 的
//   DNS 客户端直接看网络上回来的到底是什么。想知道的是：
//     - 有没有应答（没应答 = 被拦在半路）
//     - rcode（3=NXDOMAIN 域名不存在、2=SERVFAIL 上游出错）
//     - 应答多大、几条 A（大了/多了有可能是 lwIP 解析不了）
// ---------------------------------------------------------------------------
static void raw_dns_query(const char *name, const char *srv) {
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    if (inet_pton(AF_INET, srv, &a.sin_addr) != 1) { close(s); return; }
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t q[300];
    int n = 0;
    q[n++] = 0xAB; q[n++] = 0xCD;            // 事务 ID（随便给，只认这一份应答）
    q[n++] = 0x01; q[n++] = 0x00;            // 标准查询 + 期望递归
    q[n++] = 0; q[n++] = 1;                  // QDCOUNT = 1（头一共 12 字节，别数错）
    for (int i = 0; i < 6; i++) q[n++] = 0;  // ANCOUNT / NSCOUNT / ARCOUNT = 0
    for (const char *p = name; p && *p; ) {
        const char *dot = strchr(p, '.');
        int len = dot ? (int)(dot - p) : (int)strlen(p);
        q[n++] = (uint8_t)len;
        memcpy(q + n, p, (size_t)len);
        n += len;
        p = dot ? dot + 1 : NULL;
    }
    q[n++] = 0;                              // 名字结束
    q[n++] = 0; q[n++] = 1;                  // QTYPE  = A
    q[n++] = 0; q[n++] = 1;                  // QCLASS = IN

    if (sendto(s, q, (size_t)n, 0, (struct sockaddr *)&a, sizeof(a)) < 0) {
        ESP_LOGW(TAG, "RAW DNS %-28s @%-14s 发送失败", name, srv);
        close(s); return;
    }
    uint8_t r[1500];
    int rn = recvfrom(s, r, sizeof(r), 0, NULL, NULL);
    close(s);
    if (rn < 12) {
        ESP_LOGW(TAG, "RAW DNS %-28s @%-14s 无应答(%d)", name, srv, rn);
        return;
    }
    int rcode = r[3] & 0x0F;                 // 0=正常 2=SERVFAIL 3=NXDOMAIN(域名不存在)
    int ancount = (r[6] << 8) | r[7];
    ESP_LOGI(TAG, "RAW DNS %-28s @%-14s %d字节 rcode=%d an=%d",
             name, srv, rn, rcode, ancount);
}

// ---------------------------------------------------------------------------
// ★ DNS 自救：路由器下发的 DNS 不一定靠谱（小厂家/运营商劫持、转发超时都见过），
//   它一抽风，esp-tls 就只会报一句 getaddrinfo 202，连不上又看不出为什么。
//   所以在建连接之前先自己解一次：解不开就换公共 DNS 再解 —— 换完 lwIP 全局生效，
//   后面 esp-tls / esp-mqtt 用的是同一个解析器，跟着一起好。
// ---------------------------------------------------------------------------
static bool probe_resolve(const char *host, uint32_t *out_ip) {
    const struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM };
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return false;
    *out_ip = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    return true;
}

static bool dns_ensure(const char *host) {
    uint32_t ip = 0;
    // ★ 先试三次再换 DNS：刚拿到 DHCP 的那几秒，路由器自己还在热 DNS 缓存，
    //   第一次查询失败很常见 —— 只试一次就判它不行，会白白切到公共 DNS 上
    //   （公共 DNS 在国内往往更慢，反而更连不上）。
    for (int i = 0; i < 3; i++) {
        if (probe_resolve(host, &ip)) {
            ESP_LOGD(TAG, "DNS 解析 %s -> " IPSTR "（第 %d 次，路由器下发的 DNS）", host,
                     IP2STR((const ip4_addr_t *)&ip), i + 1);
            return true;
        }
        if (i < 2) vTaskDelay(pdMS_TO_TICKS(800));
    }
    if (!s_sta_netif) return false;
    ESP_LOGW(TAG, "路由器 DNS 解不开 %s，改用公共 DNS 重试", host);
    // 阿里、腾讯、谷歌各一个：前两个在国内延迟低，最后一个兜底。
    static const char *pub[] = { "223.5.5.5", "119.29.29.29", "8.8.8.8" };
    for (int i = 0; i < 3; i++) {
        esp_netif_dns_info_t d = { 0 };
        d.ip.type = ESP_IPADDR_TYPE_V4;
        if (esp_netif_str_to_ip4(pub[i], &d.ip.u_addr.ip4) != ESP_OK) continue;
        if (esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &d) != ESP_OK) continue;
        vTaskDelay(pdMS_TO_TICKS(300));
        for (int k = 0; k < 2; k++) {          // 换完也要给两次机会：第一次常常扑空
            if (probe_resolve(host, &ip)) {
                ESP_LOGI(TAG, "改用 DNS %s 后解析成功 -> " IPSTR, pub[i],
                         IP2STR((const ip4_addr_t *)&ip));
                return true;
            }
            if (k == 0) vTaskDelay(pdMS_TO_TICKS(700));
        }
        ESP_LOGW(TAG, "DNS %s 也解不开 %s", pub[i], host);
    }
    ESP_LOGE(TAG, "所有 DNS 都解不开 %s", host);
    return false;
}

// ---------------------------------------------------------------------------
// WebSocket 那一半（只在地址栏填了 ws:// wss:// 时才用得上）
// ---------------------------------------------------------------------------
// 收到的帧先攒在这儿：回调跑在组件自己的任务里，处理函数会去动 LVGL 的东西，
// 不能跨任务直接调。攒成帧交给 net_task，跟 UDP 那条路的处理时序就一致了。
static void ws_rx_push(const uint8_t *d, int n) {
    if (n <= 0 || n > CW_RXQ_LEN) return;
    portENTER_CRITICAL(&s_rxq_mux);
    int next = (s_rxq_head + 1) % CW_RXQ_FRAMES;
    if (next == s_rxq_tail) { portEXIT_CRITICAL(&s_rxq_mux); return; }  // 满了就丢：键控帧丢得起
    memcpy(s_rxq[s_rxq_head], d, (size_t)n);
    s_rxq_len[s_rxq_head] = n;
    s_rxq_head = next;
    portEXIT_CRITICAL(&s_rxq_mux);
}

static int ws_rx_pop(uint8_t *out) {
    portENTER_CRITICAL(&s_rxq_mux);
    if (s_rxq_tail == s_rxq_head) { portEXIT_CRITICAL(&s_rxq_mux); return 0; }
    int n = s_rxq_len[s_rxq_tail];
    memcpy(out, s_rxq[s_rxq_tail], (size_t)n);
    s_rxq_tail = (s_rxq_tail + 1) % CW_RXQ_FRAMES;
    portEXIT_CRITICAL(&s_rxq_mux);
    return n;
}

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base;
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        s_ws_up = true;
        ESP_LOGI(TAG, "WS 已连接 %s:%d%s", s_ws_host, s_ws_port, CW_WS_PATH);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        s_ws_up = false;
        ESP_LOGW(TAG, "WS 断开（组件会自动重连）");
        break;
    case WEBSOCKET_EVENT_DATA: {
        if (!ev || !ev->data_ptr || ev->data_len <= 0) break;
        if (ev->op_code != 0x02) break;          // 0x02 = 二进制帧；文本帧不是我们的协议
        // 大 payload 会被拆成多次事件给过来：按 offset 拼，凑满一帧再入队。
        if (ev->payload_offset == 0) s_asm_len = 0;
        if (s_asm_len + ev->data_len > CW_WS_ASM_MAX) { s_asm_len = 0; break; }  // 异常长帧，丢
        memcpy(s_asm + s_asm_len, ev->data_ptr, (size_t)ev->data_len);
        s_asm_len += ev->data_len;
        if (ev->payload_offset + ev->data_len >= ev->payload_len) {
            ws_rx_push(s_asm, s_asm_len);
            s_asm_len = 0;
        }
        break;
    }
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "WS 错误");
        break;
    case WEBSOCKET_EVENT_CLOSED:
        s_ws_up = false;
        ESP_LOGW(TAG, "WS 已关闭");
        break;
    default:
        break;
    }
}

static bool ws_start(void) {
    // ★ 不填 .uri，只填 host / port / path / transport —— 与 MQTT、OTA 那个坑同源：
    //   填 uri 会走 http_parser_parse_url，而域名里的下划线（cw_station.bubblegear.xyz）
    //   会被它判成非法主机名，直接 "Error parse uri" 让 init 返回 NULL。
    //   分开填就绕开了那一步解析，底下照样是 getaddrinfo。
    esp_websocket_client_config_t cfg = {
        .host = s_ws_host,
        .port = s_ws_port,
        .path = CW_WS_PATH,
        .transport = s_ws_tls ? WEBSOCKET_TRANSPORT_OVER_SSL : WEBSOCKET_TRANSPORT_OVER_TCP,
        .disable_auto_reconnect = false,   // 断了自己重连：公网抖动很常见，别让人重启设备
        .reconnect_timeout_ms = 3000,
        // 握手超时从 15 s 放到 20 s：实测 Cloudflare 边缘 + mbedtls 全握手在小核上要
        // 十几秒，15 s 会把自己掐断（表现为 "Error read response for Upgrade header"）。
        .network_timeout_ms = 20000,
        .buffer_size = 1024,
        .task_stack = 4096,
        .task_prio = 5,
        // Cloudflare 空闲约 100 s 会掐断连接。组件自带 ping（默认 10 s 一次），
        // 加上我们 5 s 一次的心跳，保活是够的。
        .ping_interval_sec = 15,
        .disable_pingpong_discon = false,
    };
    if (s_ws_tls) cfg.cert_pem = CW_ROOT_CA_PEM;   // 验的是马上要照着它发报的那台服务器
    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) { ESP_LOGE(TAG, "WS 客户端创建失败"); return false; }
    // ★ 是 esp_websocket_register_events（不带 client_）：名字差一个词，写错只会得到
    //   "implicit declaration" 然后链接期才炸。
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    if (esp_websocket_client_start(s_ws) != ESP_OK) {
        ESP_LOGE(TAG, "WS 启动失败");
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
        return false;
    }
    return true;
}

static void ws_stop(void) {
    if (!s_ws) return;
    esp_websocket_client_stop(s_ws);
    esp_websocket_client_destroy(s_ws);
    s_ws = NULL;
    s_ws_up = false;
}

// 设备通道建好了吗：WS 模式看连接状态，UDP 模式看 socket。
// 调用处（调频、上线）不能只判 s_sock —— 走 WebSocket 时根本没有 UDP socket。
static bool dev_ready(void) {
    return s_ws_en ? (s_ws != NULL && s_ws_up) : (s_sock >= 0);
}

// 键控、心跳、认证、调频四样都从这一个口子出去：UDP 模式发 UDP 包，WS 模式发二进制帧。
// 帧格式两边逐字节相同，所以协议层完全不知道底下换了通路。
static void dev_send(const uint8_t *frame) {
    if (s_ws_en) {
        if (s_ws && s_ws_up) {
            // ★ 超时给 20 ms 而不是死等：键控帧是从按键回调里发的，卡在那儿界面就僵了。
            //   发不出去就算了 —— 丢一帧只是少一个点划，卡住界面才是真事故。
            (void)esp_websocket_client_send_bin(s_ws, (const char *)frame, CW_PROTO_LEN,
                                                pdMS_TO_TICKS(20));
        }
        return;
    }
    if (s_sock < 0) return;
    (void)sendto(s_sock, frame, CW_PROTO_LEN, 0,
                 (struct sockaddr *)&s_srv, sizeof(s_srv));
}

void cw_net_send_key(int on, uint8_t flags, int64_t t_ms) {
    if (!s_online) return;              // 离线：一个键控帧都不发
    // ★ 未认证 = 没有合法呼号，服务端不会给转发（"只有经过认证的呼号才允许连接"）。
    //   这里先拦掉，免得设备在那边留下一个身份不明的黑户台站。
    if (!s_authed) return;
    if (t_ms <= 0) t_ms = esp_timer_get_time() / 1000;
    uint8_t frame[CW_PROTO_LEN];
    // ★ 帧里带上这一下的时刻（设备端毫秒时钟低 16 位）。服务端算点划长度时用它，
    //   而不是用它自己收到包的时间 —— 后者的抖动足以把点判成划（E → T）。
    cw_proto_build_key(frame, s_uid, s_seq++, s_freq, on, flags,
                       (uint16_t)(t_ms & 0xffff));
    dev_send(frame);
}

void cw_net_tune(uint32_t freq) {
    s_freq = freq;
    int64_t now = esp_timer_get_time() / 1000;

    // ★ 频率一变就立刻补一帧。服务端是从心跳帧里读我的频率的（hb 的 [8..11]），
    //   而心跳 5 s 才一次 —— 不补这一帧的话，按键转发、可闻窗口全按旧频率算，
    //   要等下一个心跳才生效，也就是"松手之后才更新"的来源。
    //   16 字节一帧，丢了也不补，跟键控一个策略。
    if (dev_ready()) {
        uint8_t frame[CW_PROTO_LEN];
        if (s_uid) cw_proto_build_heartbeat(frame, s_uid, s_freq);
        else       cw_proto_build_auth(frame, s_guid, s_freq);
        dev_send(frame);
    }

    if (now - s_last_presence > CW_PRESENCE_MIN_MS) {
        if (s_online) publish_presence(1);
        s_tune_dirty = false;
    } else {
        // 被节流挡掉的这一格记下来，由 net_task 在停下后补发最后一次，
        // 保证服务端最终拿到的一定是手离开时的那个频率。
        s_tune_dirty = true;
        s_tune_last = now;
    }
}

void cw_net_set_wpm(int wpm) {
    s_wpm = wpm;
    if (s_online) publish_presence(1);
}

// 组合键切在线/离线。离线要立刻把名单里的自己摘掉，所以这次发布不受 150ms 节流限制。
void cw_net_set_online(bool on) {
    s_online = on;
    s_last_presence = 0;
    publish_presence(on ? 1 : 0);
    // 回到在线时立刻补一帧，不等下一次 5 秒心跳：
    // 万一服务端那边台站已被超时剔除，这帧心跳会把它按原 uid 建回来，
    // 否则要等到下一个心跳才重新收得到转发（表现为"上线了却没声音"）。
    if (on && dev_ready()) {
        uint8_t frame[CW_PROTO_LEN];
        if (s_uid) cw_proto_build_heartbeat(frame, s_uid, s_freq);
        else       cw_proto_build_auth(frame, s_guid, s_freq);
        dev_send(frame);
    }
    ESP_LOGI(TAG, "在线状态: %s", on ? "在线（可发可收）" : "离线（不发不收）");
}

// 认证应答：服务端按 Global UID 查库，把绑定的虚拟呼号发回来。
//   uid = 0 是"我不认识你，重新认证"（服务端重启过、或这个 uid 已被回收）；
//   status = 1 表示这个号是本次新分配的（0 = 老绑定），只用于日志。
static void on_authack(const cw_frame_t *f) {
    if (f->a == 0) {
        ESP_LOGW(TAG, "服务端要求重新认证，重发 auth（Global UID %s）", s_guid_hex);
        s_authed = false;
        s_uid = 0;
        s_reauth_ms = 0;               // 逼下一轮循环立刻重发
        return;
    }
    // 呼号字段是定长 8 字节、不足补空格，这里要把空格剔掉。
    char vc[CW_CALL_LEN + 1];
    int n = 0;
    for (int i = 0; i < CW_CALL_LEN; i++) {
        char c = f->call[i];
        if (c > 0x20 && c < 0x7f) vc[n++] = c;
    }
    vc[n] = '\0';

    bool first = !s_authed;
    if (first) {
        s_authed = true;
        s_uid = f->a;
        ESP_LOGI(TAG, "认证通过 uid=%u 虚拟呼号 %s（%s）", s_uid,
                 vc[0] ? vc : "(无)", f->status == 1 ? "本次新分配" : "已有绑定");
    }
    if (vc[0] && strcmp(vc, s_vcall) != 0) {
        ESP_LOGI(TAG, "虚拟呼号 %s → %s", s_vcall[0] ? s_vcall : "(空)", vc);
        snprintf(s_vcall, sizeof(s_vcall), "%s", vc);
        // 落盘：下次离线开机时屏幕上不至于空着。虚拟呼号跟 Global UID 永久绑定，
        // 正常一辈子只写这一次（值没变就不写，见调用处的比较）。
        cw_prov_save_vcall(s_vcall);
        apply_call();
    }
    if (first) {
        cw_radio_on_net_state(CW_NET_LINK);
        publish_presence(s_online ? 1 : 0);
    }
}

// 设备帧的统一解析入口。不管这一帧是从 UDP 还是 WebSocket 来的，规则完全一样 ——
// 两种通路传的是逐字节相同的二进制帧，所以搬运层换了，协议层一行都不用动。
static void handle_dev_frame(const uint8_t *buf, size_t n) {
    cw_frame_t f;
    // occupy 帧长度＝10 + 3×条目数，台站少的时候只有十几字节，比 CW_PROTO_LEN 短，
    // 通用入口会先以"帧不够长"为由把它拒掉，所以这条链路必须单独走。
    if (cw_proto_occupy_ok(buf, (size_t)n)) {
        if (cw_proto_parse_occupy(buf, (size_t)n, &f) && f.n > 0) cw_radio_on_occupy(f.bins, f.n);
        return;
    }
    if (!cw_proto_parse(buf, (size_t)n, &f)) return;

    if (f.type == CW_TYPE_HELLO) {                 // 服务端下发的 ack
        if (f.a == 0) {
            // uid=0 是服务端在说"我不认识你，重新自报姓名"：它那边把台站丢了
            // （重启或超时），又不记得这个 uid 对应谁。这时候要是不管，它只能把
            // 我建回成一个叫 U9 的幽灵台站 —— 名字跟我的呼号毫无关系，还永久占位。
            ESP_LOGW(TAG, "服务端要求重新注册，重发 hello（呼号 %s）", hello_call());
            uint8_t hf[CW_PROTO_LEN];
            cw_proto_build_hello(hf, hello_call(), s_freq);
            dev_send(hf);
            s_uid = 0;                             // 退回"未注册"，下面的循环会继续发 hello
            return;
        }
        if (!s_uid) {
            s_uid = f.a;
            // 服务端不一定在 ack 里带回频段，带了才打，免得日志里出现 "7000000–0"。
            if (f.fmax > f.c) {
                ESP_LOGI(TAG, "已注册 uid=%u 频段 %u–%u", s_uid,
                         (unsigned)f.c, (unsigned)f.fmax);
            } else {
                ESP_LOGI(TAG, "已注册 uid=%u", s_uid);
            }
            cw_radio_on_net_state(CW_NET_LINK);
            publish_presence(s_online ? 1 : 0);
        }
    } else if (f.type == CW_TYPE_AUTHACK) {
        on_authack(&f);
    } else if (f.type == CW_TYPE_KEY) {
        if (!cw_proto_parse_down_key(buf, (size_t)n, &f)) return;
        // 下行 key 帧的 a 是发报方的 uid：交给 UI 层去核"他是不是跟我同频"。
        cw_radio_on_rx_key(f.on, f.pitch, f.s, f.wpm, f.flags, f.a);
    } else if (f.type == CW_TYPE_OCCUPY) {
        if (f.bins && f.n > 0) cw_radio_on_occupy(f.bins, f.n);
    }
}

// 收一拍：UDP 模式直接 recvfrom，WS 模式把队列里攒的帧取干净。
// ★ WS 这一边每次最多取 4 帧就返回 —— 键控密集时队列可能有积压，
//   全取完会把心跳和调频末值挤到后面去，留点余量给主循环的其他活。
static void dev_recv_tick(void) {
    if (s_ws_en) {
        int got = 0;
        for (int i = 0; i < 4; i++) {
            uint8_t buf[CW_RXQ_LEN];
            int n = ws_rx_pop(buf);
            if (n <= 0) break;
            handle_dev_frame(buf, (size_t)n);
            got++;
        }
        // UDP 模式靠 recvfrom 的 200 ms 超时帮着节流；WS 这边队列是空的就立刻返回，
        // 不歇一下的话 net_task 会变成忙循环，白烧 CPU 还挤别的任务。
        if (got == 0) vTaskDelay(pdMS_TO_TICKS(20));
        return;
    }
    uint8_t buf[320];
    struct sockaddr_storage src;
    socklen_t srclen = sizeof(src);
    ssize_t n = recvfrom(s_sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &srclen);
    if (n <= 0) return;
    handle_dev_frame(buf, (size_t)n);
}

// 一次完整的联网尝试：起 Wi-Fi → 校准 DNS → 建链路 → 认证 → 起 MQTT → 主循环。
// 返回 true  = 会话是"正常收摊"（s_run 被外面拉低，或者压根没配基站），外层就此为止；
// 返回 false = 某一环失败，整轮可以重来。
// ★ 拆成两个函数而不是在原来的长流程里加 goto：失败路径上每一步都得把已经占住的
//   资源还回去（destroy MQTT / close socket / 停 WS），挪到一处统一做，主流程也不用
//   被一堆"搬家用的"分支切开。
static bool net_run(void) {
    cw_radio_on_net_state(CW_NET_IDLE);

    esp_err_t e = wifi_start();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 启动失败: %s", esp_err_to_name(e));
        cw_radio_on_net_state(CW_NET_ERROR);
        return false;
    }
    cw_radio_on_net_state(CW_NET_WIFI);
    e = wifi_wait(20000);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 未拿到 IP: %s", esp_err_to_name(e));
        cw_radio_on_net_state(CW_NET_ERROR);
        // 连续连不上就不再死等：写 NVS 让下次开机直接进配网，然后重启。
        // 为什么要重启而不是原地切 SoftAP —— STA 与 AP 共存的 RAM 开销比这大，
        // 而且配网模式下不该让电台的音频/LVGL 三屏/MQTT 还占着内存。
        if (cw_prov_note_fail()) {
            ESP_LOGW(TAG, "连续连不上，重启进入配网模式");
            s_run = false;
            vTaskDelay(pdMS_TO_TICKS(2500));     // 留点时间让上面那行状态刷到屏上
            esp_restart();
        }
        return false;
    }
    if (s_ws_en) {
        ESP_LOGI(TAG, "Wi-Fi 就绪，服务器 %s://%s:%d（键控走 WebSocket）",
                 s_ws_tls ? "wss" : "ws", s_ws_host, s_ws_port);
    } else {
        ESP_LOGI(TAG, "Wi-Fi 就绪，服务器 %s:%d（键控走 UDP）",
                 s_srv_host[0] ? s_srv_host : "None", CONFIG_CW_UDP_PORT);
    }
    cw_prov_note_ok();                 // 连上了：清掉"连不上就进配网"的累计计数

    // 没配基站（出厂 / 恢复出厂后 BASE STATION 一页就是 None）：Wi-Fi 通着但没有服务器
    // 可连。空串拿去解析会得到 255.255.255.255 这种假地址（UDP 全发到广播上），必须在
    // 进 resolve_server() 之前拦掉，然后停在这儿，状态行显示 NET FAIL 提示"还没配基站"。
    // ★ 这种情况重试也没用（缺的是地址，不是连接），所以算"正常收摊"，不进重连循环。
    if (!s_srv_host[0]) {
        ESP_LOGW(TAG, "未配置基站服务器：不做 UDP/MQTT，保持离线"
                      "（菜单 BASE STATION → CHANGE 可以填）");
        cw_radio_on_net_state(CW_NET_ERROR);
        return true;
    }

    // ★ 先校准 DNS（1.1.15）：esp-tls 只丢一句 "getaddrinfo 202"，分不清是 DNS 服务器
    //   抽风还是域名真的不存在。这里先自己解一次，解不开就换公共 DNS 再解 —— 换完对
    //   lwIP 是全局生效的，后面 WS 与 MQTT 走的是同一个解析器，跟着一起好。
    if (!dns_ensure(cw_net_srv_host())) dns_probe(cw_net_srv_host());

    if (s_ws_en) {
        // WebSocket 模式：DNS 解析与断线重连都由组件自己管，这里只管等它连上。
        // ★ 等到 30 s 而不是 15 s：实测从开机到握手成功要 30 s 左右（DNS 冷启动 +
        //   TLS 握手 + Cloudflare 边缘节点首次接入）。以前等 15 s 就判失败、net_task
        //   直接退出 —— 结果组件在 30 s 时其实连上了，却已经没人做认证和心跳了，
        //   屏幕一直是 NET FAIL。
        if (!ws_start()) {
            cw_radio_on_net_state(CW_NET_ERROR);
            return false;
        }
        for (int i = 0; i < 300 && !s_ws_up && s_run; i++) vTaskDelay(pdMS_TO_TICKS(100));
        if (!s_run || !s_ws_up) {
            if (!s_ws_up) {
                dns_probe(s_ws_host);       // 连不上时把解析情况打出来：分得清 DNS 还是连通性
                ESP_LOGE(TAG, "WebSocket 连不上：%s://%s:%d", s_ws_tls ? "wss" : "ws",
                         s_ws_host, s_ws_port);
            }
            // 组件自己会重连（3 s 一次），但既然要整轮重来，就把这一条彻底收掉：
            // 留着它，下一轮的 ws_start() 会再开一条，两条 TLS 抢那点内存 —— 上次
            // MQTT 建第三条连接时的 ALLOC_FAILED 就是这么来的。
            ws_stop();
            cw_radio_on_net_state(CW_NET_ERROR);
            return false;
        }
    } else {
        memset(&s_srv, 0, sizeof(s_srv));
        s_srv.sin_family = AF_INET;
        s_srv.sin_port = htons((uint16_t)CONFIG_CW_UDP_PORT);

        // ★ 域名要变 IP：UDP 这一跳只认 sockaddr。开机刚拿到 DHCP 时 DNS 可能还没热，
        //   给它三次机会再放弃 —— 否则一次临时解析失败就直接 NET FAIL，得手动重启。
        bool ok = false;
        for (int i = 0; i < 3 && !ok; i++) {
            ok = resolve_server(CONFIG_CW_UDP_PORT);
            if (!ok && i < 2) vTaskDelay(pdMS_TO_TICKS(1000));
        }
        if (!ok) {
            ESP_LOGE(TAG, "服务器地址解析失败：%s", s_srv_host);
            cw_radio_on_net_state(CW_NET_ERROR);
            return false;
        }

        s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (s_sock < 0) {
            ESP_LOGE(TAG, "UDP socket 创建失败");
            cw_radio_on_net_state(CW_NET_ERROR);
            return false;
        }
        // 收包超时 200 ms：这个循环同时兼着"补发调频末值"的活，
        // 超时 1 s 的话末值要拖 1 s 才报到服务端。空转代价只是每秒多次 recvfrom 超时。
        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
        setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    // ★ 第一道门：Global UID 身份认证。
    //   没拿到服务端下发的虚拟呼号之前，不连 MQTT、不进名单、也不发键控 ——
    //   "只有经过认证的呼号才允许连接服务器"。所以这一段是阻塞的：认证不成
    //   就每 3 秒重发一帧 auth（UDP 会丢，且服务器可能还没起来）。
    //   电台界面不受影响（它在另一个任务里，本来也是离线起机的）。
    {
        int64_t last_auth = -CW_AUTH_RETRY_MS;
        while (s_run && !s_authed) {
            dev_recv_tick();
            int64_t t = esp_timer_get_time() / 1000;
            if (t - last_auth >= CW_AUTH_RETRY_MS && t >= s_reauth_ms) {
                uint8_t frame[CW_PROTO_LEN];
                cw_proto_build_auth(frame, s_guid, s_freq);
                dev_send(frame);
                last_auth = t;
                // ★ 用 D 而不是 I：认证没通过之前这一句每 3 秒一行，服务器不在时
                //   串口全是它，反而把真正有用的报错淹了。
                ESP_LOGD(TAG, "请求认证 Global UID %s", s_guid_hex);
            }
        }
    }
    if (!s_run) {                     // 退出中：直接收摊
        if (s_sock >= 0) { close(s_sock); s_sock = -1; }
        ws_stop();
        return true;
    }
    mqtt_start();

    bool retry = false;
    int64_t last_hb = 0, last_hello = -10000, last_rehello = 0, now;
    // 运行期巡检的基准：最近一次"链路确实可用"的时刻。
    int64_t last_link_ok = esp_timer_get_time() / 1000;
    while (s_run) {
        dev_recv_tick();
        now = esp_timer_get_time() / 1000;
        // ★ 运行期巡检（1.1.17）：组件的重连只负责 TCP/TLS 那一层，遇到"TCP 连得上、
        //   组件也自认为在线，但链路事实上断了"（NAT 老化、内存不足把 socket 掐了、
        //   Cloudflare 长时间 522）就没人管了 —— 屏幕还是绿的，键控全丢。
        //   WebSocket 模式下单独盯着 dev_ready()：连不上就计时，超过 CW_LINK_LOST_MS
        //   整轮重来。（UDP 模式没有可靠的可达性信号 —— socket 一直"就绪"，
        //   所以这一路实际上只在 WS 模式下生效。）
        if (dev_ready()) last_link_ok = now;
        else if (s_ws_en && now - last_link_ok > CW_LINK_LOST_MS) {
            ESP_LOGW(TAG, "链路中断已超 %d 秒，重来一轮", (int)(CW_LINK_LOST_MS / 1000));
            cw_radio_on_net_state(CW_NET_ERROR);
            retry = true;
            break;
        }
        // 连发调频的末值补发：手停下来 150 ms 后一定报一次，
        // 否则服务端停在倒数第二个上报值上（被节流挡掉的那一格）。
        if (s_tune_dirty && now - s_tune_last > CW_PRESENCE_MIN_MS) {
            s_tune_dirty = false;
            if (s_online) publish_presence(1);
        }
        if (s_uid) {
            if (now - last_hb > CW_HEARTBEAT_MS) {
                uint8_t frame[CW_PROTO_LEN];
                cw_proto_build_heartbeat(frame, s_uid, s_freq);
                dev_send(frame);
                last_hb = now;
            }
            // 离线时不补 hello：服务端见 hello 会把 off 标记清掉，等于偷偷上线。
            if (s_online && now - last_rehello > CW_REHELLO_MS) {
                uint8_t frame[CW_PROTO_LEN];
                cw_proto_build_hello(frame, hello_call(), s_freq);
                dev_send(frame);
                last_rehello = now;
            }
        } else if (now - last_hello > CW_AUTH_RETRY_MS) {
            // 认证掉了（服务端回了 uid=0）：补发 auth 而不是 hello ——
            // 现在认人靠 Global UID，不是靠呼号。
            uint8_t frame[CW_PROTO_LEN];
            cw_proto_build_auth(frame, s_guid, s_freq);
            dev_send(frame);
            last_hello = now;
        }
    }

    if (s_mqtt) { publish_presence(0); esp_mqtt_client_stop(s_mqtt); esp_mqtt_client_destroy(s_mqtt); s_mqtt = NULL; }
    if (s_sock >= 0) { close(s_sock); s_sock = -1; }
    ws_stop();
    return !retry;
}

// ★ NET FAIL 之后的重连就编排在这里（1.1.17）：以前任何一环失败，net_task 都会
//   vTaskDelete 自己 —— 屏幕上 NET FAIL 之后设备就成了半个砖，只有手动重启才会再试，
//   而服务器那边的抖动（上一轮抓到过 CF 回 522、HTTPS 延迟在 0.36~4.5 s 之间跳）
//   本来几十秒后就自己恢复。现在失败了就隔 CONFIG_CW_RETRY_SEC 秒再来一轮，
//   起不了 Wi-Fi / DNS 解不开 / WebSocket 建不起来，统统适用。
static void net_task(void *arg) {
    (void)arg;
    int attempt = 0;
    while (s_run) {
        if (net_run()) break;               // 正常运行到收摊（多半是 s_run 被拉低）
        if (!s_run) break;
        ESP_LOGW(TAG, "联网失败（第 %d 次），%d 秒后重连", ++attempt, CONFIG_CW_RETRY_SEC);
        cw_radio_on_net_state(CW_NET_ERROR);
        // 切片等待而不是一觉睡死：收到 stop（例如 OTA 前的 link_pause）要立刻能退，
        // 否则那边会干等 8 s 超时。
        for (int i = 0; i < CONFIG_CW_RETRY_SEC * 10 && s_run; i++)
            vTaskDelay(pdMS_TO_TICKS(100));
        // ★ 新的一轮 = 一条新连接：服务端那边的台账（uid / 认证状态）已经随旧连接一起
        //   没了，这几个位不清的话，新任务会以为自己还认证着，直接跳过 auth 发心跳。
        s_uid = 0;
        s_seq = 0;
        s_authed = false;
        s_reauth_ms = 0;
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

// 只把呼号与服务器地址取好，不起网络任务、不连 Wi-Fi。
// 跳过配网以离线模式开机时用得上：那一屏左上角照样要显示自己的呼号，
// 否则 cw_net_start 没走过、呼号还空着，顶行是一片空白。
void cw_net_ids_init(void) {
    cw_cred_t cred;
    bool have = cw_prov_load(&cred);
    snprintf(s_srv_host, sizeof(s_srv_host), "%s", cw_prov_server_addr());
    parse_server_scheme();      // 地址栏可能带 ws:// wss:// 前缀，先定下三条通路怎么走
    build_mqtt_uri();
    snprintf(s_call_cfg, sizeof(s_call_cfg), "%s", (have && cred.call[0]) ? cred.call : "");
    // 上次认证拿到的虚拟呼号：离线开机时屏幕上照样显示它（不是这次新拿的，但号是绑定的）。
    snprintf(s_vcall, sizeof(s_vcall), "%s", (have && cred.vcall[0]) ? cred.vcall : "");
    build_callsign();
    build_device_info();     // 离线开机也要能显示设备 ID / MAC / Global UID
}

esp_err_t cw_net_start(uint32_t freq, int wpm) {
    if (s_task) return ESP_OK;
    s_freq = freq;
    s_wpm = wpm;
    s_uid = 0;
    s_seq = 0;
    s_authed = false;
    s_reauth_ms = 0;
    s_run = true;

    // 服务器地址与呼号都可以在配网页里改，这里统一取一次：配过用配的，
    // 没配过用 Kconfig 兜底（可能是空串 = 没配基站，见 cw_prov_server_addr）。
    cw_cred_t cred;
    bool have = cw_prov_load(&cred);
    snprintf(s_srv_host, sizeof(s_srv_host), "%s", cw_prov_server_addr());
    parse_server_scheme();      // ws:// wss:// 前缀在这里剥掉，之后 dev_send/mqtt/OTA 都按它走
    build_mqtt_uri();
    snprintf(s_call_cfg, sizeof(s_call_cfg), "%s", (have && cred.call[0]) ? cred.call : "");
    snprintf(s_vcall, sizeof(s_vcall), "%s", (have && cred.vcall[0]) ? cred.vcall : "");

    build_callsign();
    build_device_info();
    ESP_LOGI(TAG, "Ham %s · 虚拟呼号 %s · Device ID %s，启动网络任务",
             s_ham[0] ? s_ham : "--", s_vcall[0] ? s_vcall : "--", s_dev_id_hex);
    if (xTaskCreate(net_task, "cw_net", 4096, NULL, 5, &s_task) != pdPASS) {
        s_run = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// ★ 链路暂停/恢复：OTA 专用
// ---------------------------------------------------------------------------
// ESP32-C3 上容不下三条 TLS 同时在场。稳态时（WS 键控 + MQTT 信令都连着）堆只剩
// 十几 KB，而一条新的 mbedTLS 握手开口就要二十几 KB —— 实测第三条连接在建的时候
// 连 4770 字节的 alloc 都失败：
//     E esp-tls-mbedtls: mbedtls_ssl_handshake returned -0x7F00   ← ALLOC_FAILED
// 于是 OTA 拉版本就停在 "SERVER UNREACHABLE"，看上去像服务器不通，其实是内存不够。
// 办法是拉固件前先把那两条连接收掉，用完再拉回来。注意这里**不能**碰 Wi-Fi，
// 否则固件就没法从网上下来了 —— 所以是这一对函数，不是 cw_net_stop()。
bool cw_net_link_pause(void) {
    if (!s_task) return false;              // 本来就没在跑（离线/没配基站），不用管
    s_run = false;
    // 有界等待：net_task 下一次循环就退出并 destroy MQTT / close socket / 停 WS。
    for (int i = 0; i < 80 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(100));
    if (s_task) ESP_LOGW(TAG, "链路暂停超时：任务没能退出，内存可能没还回来");
    return true;
}

void cw_net_link_resume(void) {
    if (s_task) return;
    // 这几个状态位不清的话，新任务会以为自己还认证着 —— 认证是服务端那边的台账，
    // 连接一断就没了，必须重新走一遍 auth。wifi_start() 本身是幂等的（已 start 会跳过）。
    s_uid = 0;
    s_seq = 0;
    s_authed = false;
    s_reauth_ms = 0;
    s_run = true;
    if (xTaskCreate(net_task, "cw_net", 4096, NULL, 5, &s_task) != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "链路恢复失败：起不了网络任务");
    }
}

void cw_net_stop(void) {
    s_run = false;
    // 有界等待：任务自己会在下一次循环退出并清理 socket / MQTT。
    for (int i = 0; i < 60 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(100));
    if (s_wifi_started) {
        esp_wifi_stop();
        esp_wifi_deinit();
        s_wifi_started = false;
    }
    s_uid = 0;
    s_authed = false;
}
