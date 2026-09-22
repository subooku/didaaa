// main/cw_ota.c —— 固件空中升级：从 CW 服务器拉镜像写进另一个 app 槽。
//
// 三个决定，写下来免得后人踩：
//   ① 用明文 HTTP 而不是 HTTPS：局域网内传输，HTTPS 要额外几十 KB 的 TLS 缓冲，
//      C3 现在同时跑着 LVGL + Wi-Fi + I2S DMA，不值得。公网部署再谈签名校验。
//   ② 版本号按点分数字逐段比，不做字符串比较 —— 否则 "1.0.10" 会被判成比
//      "1.0.9" 旧，这种 bug 只在第十次发版时才冒出来，很难查。
//   ③ 下载整块写入前不做 SHA-256：esp_ota_end 会校验镜像本身的 checksum，
//      更彻底的完整性校验留给后续的签名方案。局域网内够用了。
#include "cw_ota.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cw_prov.h"
#include "cw_radio.h"       // CW_FW_VERSION
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cw_ota";

#define FW_VER_MAX  16
#define OTA_BUF     2048        // 下载用的中转缓冲，放在 .bss（内部 DRAM）

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
static int http_get_body(const char *url, char *out, size_t cap, int timeout_ms) {
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = timeout_ms,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) return -1;
    esp_http_client_set_method(h, HTTP_METHOD_GET);
    if (esp_http_client_open(h, 0) != ESP_OK) { esp_http_client_cleanup(h); return -1; }
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
    ESP_LOGI(TAG, "GET %s -> %d (%u B)", url, status, (unsigned)got);
    return status == 200 ? (int)got : -1;
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
static bool ota_download(const char *url, uint32_t expect) {
    // 目标槽：驱动自己会挑"不是当前这个"的那个。
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) { set_msg("NO OTA SLOT"); return false; }
    ESP_LOGI(TAG, "写入分区: %s @0x%x (%u B)", part->label, (unsigned)part->address,
             (unsigned)part->size);

    esp_ota_handle_t handle = 0;
    // 已知大小就用它（驱动能顺带校验槽够不够），否则交给 OTA_SIZE_UNKNOWN。
    esp_err_t e = esp_ota_begin(part, expect ? (size_t)expect : OTA_SIZE_UNKNOWN, &handle);
    if (e != ESP_OK) { set_msg("OTA BEGIN FAIL %d", e); return false; }

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 30000,        // 局域网 1.5 MB 很快，但别卡在慢速连接上
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) { esp_ota_abort(handle); set_msg("HTTP INIT FAIL"); return false; }
    esp_http_client_set_method(h, HTTP_METHOD_GET);
    bool ok = false;
    if (esp_http_client_open(h, 0) != ESP_OK) {
        set_msg("HTTP OPEN FAIL");
    } else {
        esp_http_client_fetch_headers(h);
        int status = esp_http_client_get_status_code(h);
        uint32_t len = (uint32_t)esp_http_client_get_content_length(h);
        ESP_LOGI(TAG, "固件下载: HTTP %d, %u B", status, (unsigned)len);
        if (status != 200 || len == 0) {
            set_msg("HTTP %d", status);
        } else {
            static uint8_t buf[OTA_BUF];
            uint32_t got = 0;
            int last_log_pct = 0;
            ok = true;
            while (got < len) {
                int r = esp_http_client_read(h, (char *)buf, OTA_BUF);
                if (r > 0) {
                    if (esp_ota_write(handle, buf, (size_t)r) != ESP_OK) {
                        set_msg("WRITE FAIL");
                        ok = false;
                        break;
                    }
                    got += (uint32_t)r;
                    s_pct = (int)((uint64_t)got * 100 / len);
                    // 每 10% 留一行日志：串口上一眼能看出下载在走（也方便事后核对
                    // 卡在哪一段 —— HTTP 还是写槽）。
                    if (s_pct >= last_log_pct + 10) {
                        last_log_pct = s_pct / 10 * 10;
                        ESP_LOGI(TAG, "下载 %d%% (%u/%u B)", s_pct, (unsigned)got, (unsigned)len);
                    }
                } else if (r == 0) {
                    break;                                  // 连接提前收尾
                } else if (r != -ESP_ERR_HTTP_EAGAIN) {
                    set_msg("READ FAIL %d", r);
                    ok = false;
                    break;
                } else {
                    vTaskDelay(pdMS_TO_TICKS(2));           // 还没到数据，让出 CPU
                }
            }
            if (ok && got != len) { set_msg("TRUNCATED %u/%u", (unsigned)got, (unsigned)len); ok = false; }
        }
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);

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
    s_pct = 0;
    s_newver[0] = '\0';
    s_newsize = 0;

    const char *srv = cw_prov_server_addr();
    if (!srv || !srv[0]) {
        set_msg("NO BASE STATION");
        s_state = CW_OTA_ERR;
        goto done;
    }
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d/fw/version", srv, CONFIG_CW_FW_PORT);

    char body[256];
    if (http_get_body(url, body, sizeof(body), 8000) <= 0) {
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

    snprintf(url, sizeof(url), "http://%s:%d/fw/cw.bin", srv, CONFIG_CW_FW_PORT);
    if (ota_download(url, s_newsize)) {
        s_pct = 100;
        set_msg("DONE - REBOOTING");
        s_state = CW_OTA_OK;
    } else {
        s_state = CW_OTA_ERR;
    }

done:
    s_task = NULL;
    vTaskDelete(NULL);
}

static void job_start(job_t j) {
    if (s_task) return;                 // 已经在跑，别起第二个
    s_job = j;
    s_state = CW_OTA_BUSY;
    s_pct = 0;
    set_msg(j == JOB_QUERY ? "CHECKING..." : "DOWNLOADING...");
    // 优先级 4：跟 LVGL 同级，低于音频(6)和按键(5)。下载是后台活儿，
    // 不该抢在刷屏前面；但它又得及时把块写进去，不然 HTTP 那边会超时。
    if (xTaskCreate(ota_task, "cw_ota", 5120, NULL, 4, &s_task) != pdPASS) {
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
