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

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"   // getaddrinfo：服务器那一栏现在可以填域名，得靠它解析
#include "lwip/ip_addr.h"

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
// 注册成功后仍然定期补发 hello。不是为了保活（心跳在做），而是给服务端一个把呼号
// 改回来的机会：撞名时它会把我的名字改成 GH1BHU2 这种，等撞的那个走了也改不回来 ——
// 名字是在 hello 里认的，不发 hello 就永远纠正不了。20 s 一次，一帧而已。
#define CW_REHELLO_MS         20000

// ★ 同频判定："频道"不是一个点，而是一格。两台设备的频率差不超过这个数就算同频，
//   互相收得到（真机上就是"都落在对方接收机通带里"）。100 Hz 与服务端 BUCKET 一致，
//   改这一处要同步改 cw-server/server.js 的 CHANNEL_TOL。
//   以前是 ±450 Hz 的"零拍通带"，失谐半千赫还能听见 —— 那不是电台，是对讲机。
#define CW_CHANNEL_TOL_HZ     100

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

// MQTT 地址也由那一个 host 派生：scheme 与端口跟着 CW_TLS 走（常量见 cw_net.h）。
// s_mqtt_uri 现在只用来打日志（见 mqtt_start），不再喂给 esp-mqtt —— 原因见下面那段。
static void build_mqtt_uri(void) {
    snprintf(s_mqtt_uri, sizeof(s_mqtt_uri), "%s://%s:%d",
             CW_MQTT_SCHEME, s_srv_host, CW_MQTT_PORT);
}

static void mqtt_start(void) {
    char lwt_topic[64];
    snprintf(lwt_topic, sizeof(lwt_topic), "cw/v1/sta/%s/presence", s_call);

    // ★ 这里填 hostname + port，而不是拼好的 URI —— 踩过的坑：
    //   域名里带下划线（cw_station.bubblegear.xyz）时，esp-mqtt 的 URI 解析器直接报
    //   "Error parse uri (1)" 并让 esp_mqtt_client_init 返回 NULL，整个信令通道就没了。
    //   而 UDP 那一路走 getaddrinfo 是容忍下划线的，于是出现"认证通过但永远 OFFLINE"的怪象。
    //   直接给 hostname 就绕开了 URI 解析这一步，底下照样是 getaddrinfo。
    //   （下划线主机名本身不合规：RFC 1123 只允许字母、数字和连字符，建议域名改用连字符。）
    esp_mqtt_client_config_t cfg = {
        .broker.address.hostname = s_srv_host,
        .broker.address.port     = CW_MQTT_PORT,
        .credentials.client_id = s_call,
        // 掉电/断网时由 broker 代发：别人的名单里立刻看不到我。
        .session.last_will.topic = lwt_topic,
        .session.last_will.msg = "0",
        .session.last_will.msg_len = 1,
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
        .session.keepalive = 30,
        .network.disable_auto_reconnect = false,
    };
#if CONFIG_CW_TLS
    // ★ 校验证书这一步不能图省事跳过：不校验的 TLS 只加密、不认服务器是谁，
    //   中间人拿一张自签证书就能冒充 broker。那比明文更糟 —— 明文至少一眼看得出来。
    cfg.broker.address.transport = MQTT_TRANSPORT_OVER_SSL;
    cfg.broker.verification.certificate = CW_ROOT_CA_PEM;
#else
    cfg.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
#endif
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

static void udp_send(const uint8_t *frame) {
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
    udp_send(frame);
}

void cw_net_tune(uint32_t freq) {
    s_freq = freq;
    int64_t now = esp_timer_get_time() / 1000;

    // ★ 频率一变就立刻补一帧 UDP。服务端是从心跳帧里读我的频率的（hb 的 [8..11]），
    //   而心跳 5 s 才一次 —— 不补这一帧的话，按键转发、可闻窗口全按旧频率算，
    //   要等下一个心跳才生效，也就是"松手之后才更新"的来源。
    //   16 字节一帧，丢了也不补，跟键控一个策略。
    if (s_sock >= 0) {
        uint8_t frame[CW_PROTO_LEN];
        if (s_uid) cw_proto_build_heartbeat(frame, s_uid, s_freq);
        else       cw_proto_build_auth(frame, s_guid, s_freq);
        udp_send(frame);
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
    // 回到在线时立刻补一帧 UDP，不等下一次 5 秒心跳：
    // 万一服务端那边台站已被超时剔除，这帧心跳会把它按原 uid 建回来，
    // 否则要等到下一个心跳才重新收得到转发（表现为"上线了却没声音"）。
    if (on && s_sock >= 0) {
        uint8_t frame[CW_PROTO_LEN];
        if (s_uid) cw_proto_build_heartbeat(frame, s_uid, s_freq);
        else       cw_proto_build_auth(frame, s_guid, s_freq);
        udp_send(frame);
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

static void udp_recv_tick(void) {
    uint8_t buf[320];
    struct sockaddr_storage src;
    socklen_t srclen = sizeof(src);
    ssize_t n = recvfrom(s_sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &srclen);
    if (n <= 0) return;

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
            udp_send(hf);
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

static void net_task(void *arg) {
    (void)arg;
    cw_radio_on_net_state(CW_NET_IDLE);

    esp_err_t e = wifi_start();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 启动失败: %s", esp_err_to_name(e));
        cw_radio_on_net_state(CW_NET_ERROR);
        s_run = false;
        vTaskDelete(NULL);
        return;
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
        s_run = false;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Wi-Fi 就绪，服务器 %s:%d", s_srv_host[0] ? s_srv_host : "None",
             CONFIG_CW_UDP_PORT);
    cw_prov_note_ok();                 // 连上了：清掉"连不上就进配网"的累计计数

    // 没配基站（出厂 / 恢复出厂后 BASE STATION 一页就是 None）：Wi-Fi 通着但没有服务器
    // 可连。空串拿去解析会得到 255.255.255.255 这种假地址（UDP 全发到广播上），必须在
    // 进 resolve_server() 之前拦掉，然后停在这儿，状态行显示 NET FAIL 提示"还没配基站"。
    if (!s_srv_host[0]) {
        ESP_LOGW(TAG, "未配置基站服务器：不做 UDP/MQTT，保持离线"
                      "（菜单 BASE STATION → CHANGE 可以填）");
        cw_radio_on_net_state(CW_NET_ERROR);
        s_run = false;
        vTaskDelete(NULL);
        return;
    }

    memset(&s_srv, 0, sizeof(s_srv));
    s_srv.sin_family = AF_INET;
    s_srv.sin_port = htons((uint16_t)CONFIG_CW_UDP_PORT);

    // ★ 域名要变 IP：UDP 这一跳只认 sockaddr。开机刚拿到 DHCP 时 DNS 可能还没热，
    //   给它三次机会再放弃 —— 否则一次临时解析失败就直接 NET FAIL，得手动重启。
    {
        bool ok = false;
        for (int i = 0; i < 3 && !ok; i++) {
            ok = resolve_server(CONFIG_CW_UDP_PORT);
            if (!ok && i < 2) vTaskDelay(pdMS_TO_TICKS(1000));
        }
        if (!ok) {
            ESP_LOGE(TAG, "服务器地址解析失败：%s", s_srv_host);
            cw_radio_on_net_state(CW_NET_ERROR);
            s_run = false;
            vTaskDelete(NULL);
            return;
        }
    }

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "UDP socket 创建失败");
        cw_radio_on_net_state(CW_NET_ERROR);
        s_run = false;
        vTaskDelete(NULL);
        return;
    }
    // 收包超时 200 ms：这个循环同时兼着"补发调频末值"的活，
    // 超时 1 s 的话末值要拖 1 s 才报到服务端。空转代价只是每秒多次 recvfrom 超时。
    struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // ★ 第一道门：Global UID 身份认证。
    //   没拿到服务端下发的虚拟呼号之前，不连 MQTT、不进名单、也不发键控 ——
    //   "只有经过认证的呼号才允许连接服务器"。所以这一段是阻塞的：认证不成
    //   就每 3 秒重发一帧 auth（UDP 会丢，且服务器可能还没起来）。
    //   电台界面不受影响（它在另一个任务里，本来也是离线起机的）。
    {
        int64_t last_auth = -CW_AUTH_RETRY_MS;
        while (s_run && !s_authed) {
            udp_recv_tick();
            int64_t t = esp_timer_get_time() / 1000;
            if (t - last_auth >= CW_AUTH_RETRY_MS && t >= s_reauth_ms) {
                uint8_t frame[CW_PROTO_LEN];
                cw_proto_build_auth(frame, s_guid, s_freq);
                udp_send(frame);
                last_auth = t;
                ESP_LOGI(TAG, "请求认证 Global UID %s", s_guid_hex);
            }
        }
    }
    if (!s_run) {                     // 退出中：直接收摊
        if (s_sock >= 0) { close(s_sock); s_sock = -1; }
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    mqtt_start();

    int64_t last_hb = 0, last_hello = -10000, last_rehello = 0, now;
    while (s_run) {
        udp_recv_tick();
        now = esp_timer_get_time() / 1000;
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
                udp_send(frame);
                last_hb = now;
            }
            // 离线时不补 hello：服务端见 hello 会把 off 标记清掉，等于偷偷上线。
            if (s_online && now - last_rehello > CW_REHELLO_MS) {
                uint8_t frame[CW_PROTO_LEN];
                cw_proto_build_hello(frame, hello_call(), s_freq);
                udp_send(frame);
                last_rehello = now;
            }
        } else if (now - last_hello > CW_AUTH_RETRY_MS) {
            // 认证掉了（服务端回了 uid=0）：补发 auth 而不是 hello ——
            // 现在认人靠 Global UID，不是靠呼号。
            uint8_t frame[CW_PROTO_LEN];
            cw_proto_build_auth(frame, s_guid, s_freq);
            udp_send(frame);
            last_hello = now;
        }
    }

    if (s_mqtt) { publish_presence(0); esp_mqtt_client_stop(s_mqtt); esp_mqtt_client_destroy(s_mqtt); s_mqtt = NULL; }
    if (s_sock >= 0) { close(s_sock); s_sock = -1; }
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
