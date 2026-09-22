// main/main.c —— CW 网络电台的产品固件入口：外设初始化 + 按需配网 + 按键分发。
//
// 开机流程：I2C / 显示 / LVGL → 需要时进配网（SoftAP + 配置页）→ 进电台。
// 没有 demo 菜单：这台机器就是一台 CW 电台，开机直进电台页。
//
// 按键语义：
//   物理键在分发这一层重排过（见 btn_remap）：按物理 OK 键做的是"逻辑 OK"的事，
//   其余两个同理 —— 活跟着键的新名字走。UP+DOWN 同按 = 在线/离线开关，
//   OK 长按 900ms（仅在离线时）= 进/出设置菜单。
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_pins.h"      // 错误日志里要打印 BSP_LCD_* 引脚号
#include "cw_radio.h"
#include "cw_prov.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "main";

#define INPUT_QUEUE_DEPTH 8

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static QueueHandle_t s_input_queue;
static TaskHandle_t  s_input_task;
static volatile bool s_input_ready;

static void process_input(const input_event_t *input) {
    // 电台起来之前（配网屏）按键归配网屏，已经在 on_key 里分流，走不到这里。
    // cw_radio_key 只是把事件塞进电台自己的队列，不碰 LVGL，可以直接调。
    cw_radio_key(input->btn, input->event);
}

static void input_task(void *arg) {
    (void)arg;
    input_event_t input;
    for (;;) {
        if (xQueueReceive(s_input_queue, &input, portMAX_DELAY) == pdTRUE) {
            process_input(&input);
        }
    }
}

static esp_err_t input_dispatch_init(void) {
    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (!s_input_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(input_task, "cw_input", 4096, NULL, 5, &s_input_task) != pdPASS) {
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void input_dispatch_deinit(void) {
    s_input_ready = false;
    if (s_input_task) {
        vTaskDelete(s_input_task);
        s_input_task = NULL;
    }
    if (s_input_queue) {
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
    }
}

// 按键回调跑在共享的 esp_timer 任务上，只入队、立刻返回。
// ★ 按键重排（用户要求）：OK 键干原来 UP 的活，UP 干原来 DOWN 的活，DOWN 干原来
//   OK 的活（也就是电键 + 长按 900ms 进菜单）。
//   三个键共用一路 ADC，硬件索引由分压电阻定死，改不了物理顺序，只能在分发这一层
//   换一次 —— 换在这里，配网屏（UP/DOWN 调亮度）和电台页会一起跟着变，不会出现
//   两个页面方向相反的情况。COMBO 是虚拟出来的第四档，语义固定，不参与重排。
// 物理键 → 逻辑键。新布局是"OK = 原来的 UP，UP = 原来的 DOWN，DOWN = 原来的 OK"，
// 所以活（电键/长按进菜单）必须跟着新名字走：按物理 UP 键，做的是 OK 键的事。
static bsp_btn_t btn_remap(bsp_btn_t b) {
    switch (b) {
        case BSP_BTN_UP:   return BSP_BTN_OK;
        case BSP_BTN_DOWN: return BSP_BTN_UP;
        case BSP_BTN_OK:   return BSP_BTN_DOWN;
        default:           return b;        // COMBO 组合档不参与重排
    }
}

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    const bsp_btn_t b = btn_remap(btn);
    // 配网屏期间电台还没启动（s_input_ready 此时是 false），按键归配网屏：
    // UP/DOWN 调亮度、任意键唤醒背光、长按 OK 跳过。cw_prov 内部会判断自己是否已经接手。
    if (!s_input_ready) { cw_prov_on_key(b, ev); return; }
    if (!s_input_queue) return;
    // 电键的起落时刻在按键回调里就落下来，别跟着队列和 LVGL 锁一起排队 ——
    // 那一等最多 200ms，足够把一记点（66ms）拖成划。
    cw_radio_key_edge(b, ev);
    const input_event_t input = { .btn = b, .event = ev };
    (void)xQueueSend(s_input_queue, &input, 0);
}

void app_main(void) {
    ESP_LOGI(TAG, "CW 电台启动");
    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "休眠唤醒原因: %d", wakeup);
    }

    bsp_i2c_init();
    bsp_i2c_scan();

    // 屏幕是唯一的 UI 载体，起不来就没有电台可言 —— 打清楚日志后退出，
    // 不做"无屏降级"（那会让本文件复杂一倍，而实际没法用）。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败，无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(CW_BL_DEFAULT);   // 开机先按默认亮度点亮，随后由配网屏/电台各自改成存过的值

    esp_err_t input_err = input_dispatch_init();
    esp_err_t button_err = input_err == ESP_OK
                         ? bsp_button_init(on_key, NULL)
                         : ESP_ERR_INVALID_STATE;
    if (input_err != ESP_OK) {
        ESP_LOGE(TAG, "按键事件任务创建失败: %s", esp_err_to_name(input_err));
    } else if (button_err != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败: %s", esp_err_to_name(button_err));
        input_dispatch_deinit();
    }
    // 音频与电池起不来不影响开机：侧音没了还能看，屏幕没了就是砖。
    if (bsp_audio_init() != ESP_OK) ESP_LOGW(TAG, "音频初始化失败，侧音不可用");
    if (bsp_battery_init() != ESP_OK) ESP_LOGW(TAG, "电量检测初始化失败，顶行电量不显示");

    // 配网优先：没配过 Wi-Fi，或者被要求重配（菜单里手动选的、或连续连不上自动置位），
    // 就先起 SoftAP + 配置页。用户在页面上保存后 cw_prov_run() 会自己重启，不返回；
    // 只有 SoftAP 起不来时才返回，那就退回原来的电台流程。
    cw_cred_t cred;
    bool have_cred = cw_prov_load(&cred);
    // 编译期就把 SSID 填好了（不是占位符）也算"有凭据"，保留原来的用法。
    if (!have_cred && strcmp(CONFIG_CW_WIFI_SSID, "myssid") != 0) have_cred = true;
    // 两种"只改一栏"的配网都只有在已经配过网时才成立：没凭据就无从"保留其余字段"，
    // 退回完整配网（srvmode = 只改服务器地址，callmode = 只改 Ham 呼号）。
    cw_prov_mode_t pmode = CW_PROV_FULL;
    if (have_cred && cw_prov_srv_mode())  pmode = CW_PROV_SERVER;
    if (have_cred && cw_prov_call_mode()) pmode = CW_PROV_CALL;
    bool part_mode = (pmode != CW_PROV_FULL);
    // 上次在配网屏长按 OK 跳过过：这次别再拉进配网，直接以离线方式进电台。
    // 标记用完就清 —— 它只对紧接着的那一次开机有效，想配网随时在菜单里选。
    bool skip_prov = cw_prov_skip_pending();
    if (skip_prov) {
        cw_prov_clear_skip();
        ESP_LOGW(TAG, "上次跳过了配网：本次直接进电台（离线，菜单 → WI-FI SETUP 可再配）");
    }
    if ((!have_cred || cw_prov_forced() || part_mode) && !skip_prov) {
        ESP_LOGW(TAG, "进入配网模式: %s",
                 pmode == CW_PROV_SERVER ? "只改服务器地址" :
                 pmode == CW_PROV_CALL   ? "只改 Ham 呼号" :
                 (have_cred ? "被要求重配" : "NVS 里没有凭据"));
        cw_prov_run(pmode);
    }

    if (bsp_lvgl_lock(1000)) {
        cw_radio_enter();                   // 建主屏。网络任务在 start 里按需起
        bsp_lvgl_unlock();
        s_input_ready = true;               // 从这一刻起按键归电台
    }
    if (cw_radio_start() != ESP_OK) {
        ESP_LOGE(TAG, "电台启动失败");
    }

    ESP_LOGI(TAG, "就绪");
}
