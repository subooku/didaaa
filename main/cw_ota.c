// main/cw_ota.c —— 固件空中升级：从 CW 服务器拉镜像写进另一个 app 槽。
//
// 三个决定，写下来免得后人踩：
//   ① 传输方式由 CW_TLS 决定：默认明文 HTTP（局域网，省几十 KB 的 TLS 缓冲），
//      公网部署把 CW_TLS 打开就切到 https://<域名>/fw/*，并用 root_ca.pem 校验证书。
//      ★ 注意 CW_TLS 只管这一跳和 MQTT：UDP 键控永远是明文，见 docs/deployment.md。
//   ② 版本号按点分数字逐段比，不做字符串比较 —— 否则 "1.0.10" 会被判成比
//      "1.0.9" 旧，这种 bug 只在第十次发版时才冒出来，很难查。
//   ③ 下载整块写入前不做 SHA-256：esp_ota_end 会校验镜像本身的 checksum，
//      更彻底的完整性校验留给后续的签名方案。局域网内够用了。
#include "cw_ota.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cw_net.h"         // CW_FW_SCHEME / CW_FW_PORT / CW_ROOT_CA_PEM
#include "cw_prov.h"
#include "cw_radio.h"       // CW_FW_VERSION
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"    // esp_get_free_heap_size / esp_get_minimum_free_heap_size
#include "esp_timer.h"     // esp_timer_get_time：诊断里给下载计时用
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cw_ota";

#define FW_VER_MAX  16
#define OTA_BUF     2048        // 下载用的中转缓冲，放在 .bss（内部 DRAM）
// 分块下载：一次只向服务器要这么多字节。★ 源于实测：一口气读 1.5 MB 的连接每次都
// 在同一处（约 214 KB）断掉并伴随 mbedtls 分配失败；切成小块后单次的内存用量就
// 变成了常数，跟固件多大无关。服务器那边支持 Range（"accept-ranges: bytes"）。
#define OTA_CHUNK   (64u * 1024u)

typedef enum { JOB_QUERY = 0, JOB_UPGRADE } job_t;

static volatile cw_ota_state_t s_state = CW_OTA_IDLE;
static volatile int            s_pct;
static char   s_msg[80];
static char   s_newver[FW_VER_MAX];
static uint32_t s_newsize;
static job_t  s_job;
static TaskHandle_t s_task;

// 跨任务读的那两个加了 volatile，这两个只在任务里写、UI 任务里读（有 LVGL 锁顺序
// 保证），不加 volatile 也能工作，但保持一致的写法更安全。
static void set_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_msg, sizeof(s_msg), fmt, ap);
    va_end(ap);
}

// ===========================================================================
// 极简 HTTP GET：把整个响应读进 out（只用于 /fw/version 这种小文本）
// ===========================================================================
// ★ 这里收的是 path（"/fw/version"）而不是完整 URL —— 与 MQTT 那个坑同源：
//   域名里带下划线（cw_station.bubblegear.xyz）时，esp_http_client 的 URL 解析器
//   直接报 "Error parse url"，整条 OTA 通道就废了。拆成 host + path 给它就绕开了
//   那一步解析，底下照样是 getaddrinfo。
static int http_get_body_once(const char *path, char *out, size_t cap, int timeout_ms) {
    // ★ 端口与协议都问 cw_net 要，不再看编译期的 CW_FW_PORT / CONFIG_CW_TLS：
    //   服务器地址填 wss:// 时固件要走 https:443（过 Cloudflare 只认这一条），
    //   填域名/IP 时走原来的 CW_FW_PORT 明文。同一个固件两种部署都得能升级。
    esp_http_client_config_t cfg = {
        .host = cw_net_srv_host(),
        .port = cw_net_fw_port(),
        .path = path,
        .timeout_ms = timeout_ms,
        .keep_alive_enable = false,
    };
    if (cw_net_fw_tls()) {
        cfg.transport_type = HTTP_TRANSPORT_OVER_SSL;
        cfg.cert_pem = CW_ROOT_CA_PEM;
    }
    // ★ 这三个"静默 return -1"是真凶藏身处：连不上时 UI 只写一句 SERVER UNREACHABLE，
    //   串口上什么都看不到。这里把每一步的真实返回值打出来。
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) { ESP_LOGE(TAG, "http_client_init 失败"); return -1; }
    esp_http_client_set_method(h, HTTP_METHOD_GET);
    err_t open_err = esp_http_client_open(h, 0);
    if (open_err != ESP_OK) {
        ESP_LOGE(TAG, "http_client_open 失败: %s (%d) · host=%s port=%d tls=%d · 剩余堆 %u / 最低 %u",
                 esp_err_to_name(open_err), (int)open_err, cw_net_srv_host(),
                 cw_net_fw_port(), (int)cw_net_fw_tls(),
                 esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
        esp_http_client_cleanup(h);
        return -1;
    }
    esp_http_client_fetch_headers(h);
    int status = esp_http_client_get_status_code(h);
    size_t got = 0;
    if (status == 200) {
        for (;;) {
            int r = esp_http_client_read(h, out + got, (int)(cap - 1 - got));
            if (r > 0) {
                got += (size_t)r;
                if (got >= cap - 1) break;
            } else if (r == 0) {
                break;                              // 服务端收尾，正常结束
            } else {
                if (r != -ESP_ERR_HTTP_EAGAIN) { got = 0; status = -1; }
                break;
            }
        }
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    out[got] = '\0';
    ESP_LOGI(TAG, "GET %s://%s:%d%s %s -> %d (%u B) · 剩余堆 %u / 最低 %u",
             cw_net_fw_tls() ? "https" : "http", cw_net_srv_host(), cw_net_fw_port(),
             path, cw_net_fw_tls() ? "(TLS)" : "(明文)", status, (unsigned)got,
             esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
    return status == 200 ? (int)got : -1;
}

// ★ 外面套一层重试：真机上抓到过 Cloudflare 返回 522（连接源站超时），重试就成了。
//   服务端那种抖动（本机实测同一接口延迟在 0.36 s 到 4.5 s 之间跳）挡不住，
//   但重发一次的代价很小 —— 比起让用户看到"连不上服务器"然后自己去按 RETRY，
//   这里多试两回更实在。
static int http_get_body(const char *path, char *out, size_t cap, int timeout_ms) {
    int got = -1;
    for (int attempt = 0; attempt < 3; attempt++) {
        got = http_get_body_once(path, out, cap, timeout_ms);
        if (got >= 0) return got;
        if (attempt < 2) {
            ESP_LOGW(TAG, "第 %d 次没拿到 %s，1.5 s 后重试", attempt + 1, path);
            vTaskDelay(pdMS_TO_TICKS(1500));
        }
    }
    ESP_LOGE(TAG, "三次都没拿到 %s", path);
    return -1;
}

// ===========================================================================
// JSON：只取两个字段，不引解析器
// ===========================================================================
static const char *json_val(const char *json, const char *key) {
    char k[32];
    snprintf(k, sizeof(k), "\"%s\"", key);
    const char *p = strstr(json, k);
    if (!p) return NULL;
    p = strchr(p + strlen(k), ':');
    return p ? p + 1 : NULL;
}

static bool json_str(const char *json, const char *key, char *out, size_t n) {
    const char *p = json_val(json, key);
    if (!p) return false;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return false;
    p++;
    const char *e = strchr(p, '"');
    if (!e) return false;
    size_t len = (size_t)(e - p);
    if (len >= n) len = n - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

static long json_int(const char *json, const char *key) {
    const char *p = json_val(json, key);
    if (!p) return -1;
    while (*p == ' ' || *p == '\t') p++;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    return (end && end != p) ? v : -1;
}

// ===========================================================================
// 版本比较
// ===========================================================================
bool cw_ota_ver_newer(const char *a, const char *b) {
    unsigned av[3] = { 0 }, bv[3] = { 0 };
    sscanf(a, "%u.%u.%u", &av[0], &av[1], &av[2]);
    sscanf(b, "%u.%u.%u", &bv[0], &bv[1], &bv[2]);
    for (int i = 0; i < 3; i++) if (av[i] != bv[i]) return av[i] > bv[i];
    return false;
}

// ===========================================================================
// 下载并写入 OTA 槽
// ===========================================================================
static bool ota_download(const char *path, uint32_t expect) {
    // 目标槽：驱动自己会挑"不是当前这个"的那个。
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) { set_msg("NO OTA SLOT"); return false; }
    ESP_LOGI(TAG, "写入分区: %s @0x%x (%u B)", part->label, (unsigned)part->address,
             (unsigned)part->size);

    esp_ota_handle_t handle = 0;
    // 已知大小就用它（驱动能顺带校验槽够不够），否则交给 OTA_SIZE_UNKNOWN。
    esp_err_t e = esp_ota_begin(part, expect ? (size_t)expect : OTA_SIZE_UNKNOWN, &handle);
    if (e != ESP_OK) { set_msg("OTA BEGIN FAIL %d", e); return false; }

    // ★ 分块下载：一次只向服务器要 OTA_CHUNK 字节，块之间用 keep-alive 复用同一条
    //   TLS 连接。这么改是有实测依据的 —— 原来一口气把 1.5 MB 读完的那种做法，每次都
    //   卡在约 214 KB 处报 mbedtls 分配失败（-0x7F00）后被服务端断连（-0x7280），
    //   现象是"下载到一成多就失败"。切成小块后单次传输的内存占用成了常数，跟固件多大
    //   无关；keep-alive 则是不想每块再付一次经 Cloudflare 的 TLS 握手（实测 7~11 s）。
    //   服务器回 206 Partial Content 才说明它认 Range（Cloudflare 与 1Panel 都支持）。
    esp_http_client_config_t cfg = {
        .host = cw_net_srv_host(),
        .port = cw_net_fw_port(),
        .path = path,
        .timeout_ms = 30000,        // 单块 64 KB 的超时：正常 5 s 到手，给到 30 s 留富余
        .keep_alive_enable = true,
    };
    if (cw_net_fw_tls()) {
        cfg.transport_type = HTTP_TRANSPORT_OVER_SSL;
        cfg.cert_pem = CW_ROOT_CA_PEM;  // 验证服务器是真站：拉的是马上要烧进 Flash 的镜像
    }
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) { esp_ota_abort(handle); set_msg("HTTP INIT FAIL"); return false; }
    esp_http_client_set_method(h, HTTP_METHOD_GET);

    bool ok = true;
    uint32_t written = 0;
    int last_log_pct = 0;
    static uint8_t buf[OTA_BUF];
    // 块数上限兜底：正常的 1.5 MB / 64 KB 也就 25 块，真跑到 200 块一定是哪里在兜圈。
    for (int round = 0; ok && round < 200; round++) {
        char rng[40];
        snprintf(rng, sizeof(rng), "bytes=%u-%u", (unsigned)written,
                 (unsigned)(written + OTA_CHUNK - 1));
        esp_http_client_set_header(h, "Range", rng);
        int status = 0;
        uint32_t len = 0;
        // ★ 单块也重试：服务端那边会抖（真机上见过 Cloudflare 回 522），
        //   整块重来一次比整轮升级失败划算。
        for (int attempt = 0; attempt < 3; attempt++) {
            esp_err_t oe = esp_http_client_open(h, 0);
            if (oe != ESP_OK) {
                ESP_LOGW(TAG, "第 %d 块 open 失败(%d)：%s", round, attempt + 1, esp_err_to_name(oe));
                esp_http_client_close(h);
                if (attempt < 2) { vTaskDelay(pdMS_TO_TICKS(1500)); continue; }
                set_msg("HTTP OPEN FAIL");
                ok = false;
                break;
            }
            esp_http_client_fetch_headers(h);
            status = esp_http_client_get_status_code(h);
            len = (uint32_t)esp_http_client_get_content_length(h);
            if (status >= 500) {                  // 5xx = 服务端那头的事，换一次
                ESP_LOGW(TAG, "第 %d 块 HTTP %d（服务端抖动），重来", round, status);
                esp_http_client_close(h);
                if (attempt < 2) { vTaskDelay(pdMS_TO_TICKS(1500)); continue; }
                set_msg("HTTP %d", status);
                ok = false;
                break;
            }
            break;
        }
        if (!ok) break;
        if (len == 0 || (status != 200 && status != 206)) {
            ESP_LOGE(TAG, "第 %d 块异常应答: HTTP %d, %u B", round, status, (unsigned)len);
            set_msg("HTTP %d", status);
            ok = false;
            esp_http_client_close(h);
            break;
        }
        if (status == 200 && len > OTA_CHUNK) {
            // 服务器没认 Range，把整个文件塞回来了 —— 这正是会被撑爆的读法。
            // 宁可在这里停手，也别写到一半崩在 mbedtls 的分配失败上。
            ESP_LOGE(TAG, "服务器不支持 Range（返回 200 且 %u B），中止", (unsigned)len);
            set_msg("NO RANGE");
            ok = false;
            esp_http_client_close(h);
            break;
        }
        uint32_t got = 0;
        while (got < len) {
            int r = esp_http_client_read(h, (char *)buf, OTA_BUF);
            if (r > 0) {
                if (esp_ota_write(handle, buf, (size_t)r) != ESP_OK) {
                    set_msg("WRITE FAIL");
                    ok = false;
                    break;
                }
                got += (uint32_t)r;
            } else if (r == 0) {
                break;                                  // 连接提前收尾
            } else if (r != -ESP_ERR_HTTP_EAGAIN) {
                ESP_LOGE(TAG, "第 %d 块读失败 %d @%u B", round, r, (unsigned)got);
                set_msg("READ FAIL %d", r);
                ok = false;
                break;
            } else {
                vTaskDelay(pdMS_TO_TICKS(2));           // 还没到数据，让出 CPU
            }
        }
        esp_http_client_close(h);        // keep-alive：连接留着，下一块直接复用
        written += got;
        ESP_LOGI(TAG, "第 %d 块 %s -> %u/%u B，累计 %u B · 剩余堆 %u",
                 round, rng, (unsigned)got, (unsigned)len, (unsigned)written,
                 esp_get_free_heap_size());
        if (ok && got != len) { set_msg("TRUNCATED %u/%u", (unsigned)got, (unsigned)len); ok = false; }
        if (!ok) break;
        if (expect) s_pct = (int)((uint64_t)written * 100 / expect);
        if (s_pct >= last_log_pct + 10) {
            last_log_pct = s_pct / 10 * 10;
            ESP_LOGI(TAG, "下载 %d%%", last_log_pct);
        }
        if (got < OTA_CHUNK) break;                     // 最后一块到手
    }
    esp_http_client_cleanup(h);

    if (ok && expect && written != expect) {
        set_msg("SIZE %u/%u", (unsigned)written, (unsigned)expect);
        ok = false;
    }
    if (!ok) { esp_ota_abort(handle); return false; }
    // 收尾这一步会校验整个镜像的 checksum —— 传坏了在这里被拦下，不会去启动坏固件。
    if (esp_ota_end(handle) != ESP_OK) { set_msg("IMAGE INVALID"); return false; }
    if (esp_ota_set_boot_partition(part) != ESP_OK) { set_msg("SET BOOT FAIL"); return false; }
    return true;
}

// ===========================================================================
// 任务
// ===========================================================================
static void ota_task(void *arg) {
    (void)arg;
    job_t job = s_job;
    bool  resume_net = false;       // 拉固件前有没有把链路停掉（事后要不要恢复）
    s_pct = 0;
    s_newver[0] = '\0';
    s_newsize = 0;

    const char *srv = cw_prov_server_addr();
    if (!srv || !srv[0]) {
        set_msg("NO BASE STATION");
        s_state = CW_OTA_ERR;
        goto done;
    }
    // ★ 拉固件前先收掉键控/信令那两条连接（Wi-Fi 保留）。这一步是必须的：
    //   过 Cloudflare 时 OTA 走第三条 TLS，而它能用的余量比前两条加起来还少，
    //   不腾内存就一定握手失败，症状看起来像"服务器连不上"。详见 cw_net_link_pause。
    resume_net = cw_net_link_pause();
    vTaskDelay(pdMS_TO_TICKS(400));         // 等 socket / TLS 上下文真正释放干净
    ESP_LOGI(TAG, "腾出内存后：剩余堆 %u 字节", esp_get_free_heap_size());
    // 只传 path：host 与端口由 http_get_body / ota_download 自己填（见那两处的注释）。
    char body[256];
    // timeout 给 20 s：★ 过 Cloudflare 时一次 TLS 握手实测要 7~11 s（域名分布的边缘
    //   节点首次接入更慢），原来给 8 s，手慢一点就被判成"连不上服务器"，其实只是没握手完。
    if (http_get_body("/fw/version", body, sizeof(body), 20000) <= 0) {
        set_msg("SERVER UNREACHABLE");
        s_state = CW_OTA_ERR;
        goto done;
    }
    if (!json_str(body, "version", s_newver, sizeof(s_newver))) {
        set_msg("BAD VERSION FILE");
        s_state = CW_OTA_ERR;
        goto done;
    }
    long size = json_int(body, "size");
    s_newsize = size > 0 ? (uint32_t)size : 0;
    ESP_LOGI(TAG, "服务器固件: %s (%u B)，本机 %s", s_newver, (unsigned)s_newsize,
             CW_FW_VERSION);

    if (job == JOB_QUERY) {
        if (cw_ota_ver_newer(s_newver, CW_FW_VERSION)) set_msg("NEW: %s", s_newver);
        else                                                    set_msg("UP TO DATE");
        s_state = CW_OTA_IDLE;
        goto done;
    }

    if (ota_download("/fw/cw.bin", s_newsize)) {
        s_pct = 100;
        set_msg("DONE - REBOOTING");
        s_state = CW_OTA_OK;
    } else {
        s_state = CW_OTA_ERR;
    }

done:
    // 把链路拉回来：查版本的代价是电台离线几秒，之后自动重新认证。
    // 升级成功时马上就要重启了，不必再折腾一遍连接。
    if (resume_net && s_state != CW_OTA_OK) cw_net_link_resume();
    s_task = NULL;
    vTaskDelete(NULL);
}

static void job_start(job_t j) {
    if (s_task) return;                 // 已经在跑，别起第二个
    s_job = j;
    s_state = CW_OTA_BUSY;
    s_pct = 0;
    set_msg(j == JOB_QUERY ? "CHECKING..." : "DOWNLOADING...");
    // 优先级 5：高于 LVGL(4)，低于音频(6)。★ 原来是 4，跟 LVGL 平级 ——
    //   实测平级时 lwIP 收到的数据来不及被 esp_http_client_read 取走（LVGL 占着 CPU
    //   不放），堆被数据缓冲吃得飞快，最后握手/读数据直接失败。放到 LVGL 之上，
    //   数据一到就被读走，任务自己大部分时间是阻塞在读上，CPU 反而让得出去。
    //   下载是后台活儿，不该抢在刷屏前面；但它又得及时把块写进去，不然 HTTP 那边会超时。
    if (xTaskCreate(ota_task, "cw_ota", 5120, NULL, 5, &s_task) != pdPASS) {
        s_task = NULL;
        s_state = CW_OTA_ERR;
        set_msg("TASK FAIL");
    }
}

// ===========================================================================
// 对外接口
// ===========================================================================
void cw_ota_init(void) {
    // 新固件首次启动是"待确认"态：这里代表"程序跑起来了、能进电台"，于是确认。
    // 若这一步之前就崩（或卡死触发看门狗），bootloader 会自动弹回上一槽。
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_VALID;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "新固件待确认 -> 标记有效（%s @0x%x）", run->label,
                 (unsigned)run->address);
        esp_ota_mark_app_valid_cancel_rollback();
    }
    ESP_LOGI(TAG, "OTA 就绪: 运行 %s，版本 %s",
             run ? run->label : "?", CW_FW_VERSION);
}

void cw_ota_reset(void) {
    if (s_task) return;             // 有任务在跑，别去动它的状态
    s_state = CW_OTA_IDLE;
    s_pct = 0;
    s_newver[0] = '\0';
    s_newsize = 0;
    s_msg[0] = '\0';
}

void cw_ota_query_start(void)    { job_start(JOB_QUERY); }
void cw_ota_upgrade_start(void)  { job_start(JOB_UPGRADE); }

cw_ota_state_t cw_ota_state(void) { return s_state; }
int            cw_ota_pct(void)   { return s_pct; }
const char    *cw_ota_msg(void)   { return s_msg; }
const char    *cw_ota_newver(void){ return s_newver; }
uint32_t       cw_ota_newsize(void){ return s_newsize; }
bool           cw_ota_busy(void)  { return s_task != NULL; }
