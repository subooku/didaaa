// components/bsp/src/bsp_button.c
// 移植自 trae_card/components/platform/platform_esp32/src/btn_iot_button.c
#include "bsp_button.h"
#include "bsp_pins.h"
#include "iot_button.h"
#include "button_types.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "bsp_btn";

static const uint16_t BTN_MV[BSP_BTN_COUNT][2] = BSP_BTN_MV_TABLE;

static button_handle_t s_btn[BSP_BTN_COUNT];
static bsp_btn_cb_t    s_cb;
static void           *s_user;
static bool            s_ready;

// ============================================================================
// 判档的三道保险（这一段取代了 button 组件自带的 button_adc.c）
// ----------------------------------------------------------------------------
// 为什么不用组件自带的 ADC 按键：它只做「读数落在 [min,max] 就算按下」，四个档的
// 窗口首尾相接（150/250/447mV 是共享边界），噪声把读数推过边界时，会在两个档之间
// 来回跳 —— 每个按键实例一套状态机，彼此不沟通，于是同一时刻可能两个键都被判成按下。
//
// 这里改成 BSP 自己提供 driver（iot_button_create() 是公开 API），把判决做三件事：
//   ① 多次采样求平均 —— 先把噪声本身压下去（σ 约降到 1/√N）。
//   ② 迟滞 —— 已经判中的档把窗口放宽 HYST，没判中的档把窗口收紧 HYST。两边之间
//      那 2×HYST 就是死区：噪声在死区里晃，判决纹丝不动。
//   ③ 唯一档仲裁 —— 放宽后的窗口会互相重叠，这里保证一轮只认一个档，且优先维持
//      当前档。这才是"进入难、离开也难"能同时成立的原因。
// ============================================================================
#define BSP_BTN_SAMPLES    4      // 每次判决取几次 ADC 平均
#define BSP_BTN_HYST_MV    12     // 迟滞半径(mV)。4 次平均后噪声只剩几 mV，12mV 死区够用
#define BSP_BTN_SAMPLE_US  2000   // 采样节流:组件每 5ms 把 4 个键各问一遍,这里保证一轮只采一次
#define BSP_BTN_NONE       0xFF   // 没有档命中（松开态约 3300mV）

// 一个键一个 driver。base 必须放在首位：靠它反推外层拿 index。
typedef struct {
    button_driver_t base;
    uint8_t         index;
} bsp_adc_btn_t;

static bsp_adc_btn_t s_drv[BSP_BTN_COUNT];
static uint8_t       s_cur    = BSP_BTN_NONE;   // 当前判中的档
static int           s_mv     = -1;             // 最近一次判决用的电压
static int64_t       s_last_us;                 // 上次采样的时刻
static uint32_t      s_win_ms;                  // 抖动自检:100ms 窗口的起点
static uint32_t      s_warn_ms;                 // 抖动自检:上次告警的时刻
static int           s_flips;                   // 抖动自检:当前窗口内翻了几次

// ADC1 是 unit 级独占资源:iot_button 与 bsp_button_read_mv() 必须共用同一个 oneshot
// 句柄。谁第二个调 adc_oneshot_new_unit() 谁就拿到 "adc1 is already in use"。
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;

// 衰减档决定量程:必须用 12dB(约 0~3100mV)才盖得住松开态的 3300mV —— 换成 6dB
// 会把松开态和高电压档一起削到顶,判档全乱。通道由本文件在 init 里配置,只配一次。
// (原来走组件自带的 button_adc.c 时也得对齐它的 ADC_BUTTON_ATTEN,现在不用了。)
#define BSP_BTN_ATTEN  ADC_ATTEN_DB_12       // 量程约 0~3100mV,覆盖松开态

// 每个按键把"哪个键"随回调带回来。button 组件的回调签名固定,故用 usr_data 传索引。
static void on_event(void *arg, void *usr_data, bsp_btn_ev_t ev) {
    (void)arg;
    if (!s_cb) return;
    s_cb((bsp_btn_t)(intptr_t)usr_data, ev, s_user);
}
static void cb_press (void *a, void *u) { on_event(a, u, BSP_BTN_PRESS);  }
static void cb_click (void *a, void *u) { on_event(a, u, BSP_BTN_CLICK);  }
static void cb_double(void *a, void *u) { on_event(a, u, BSP_BTN_DOUBLE); }
static void cb_long  (void *a, void *u) { on_event(a, u, BSP_BTN_LONG);   }
static void cb_up    (void *a, void *u) { on_event(a, u, BSP_BTN_RELEASE); }

// raw → mV。校准句柄建不出来时退回线性估算：12bit × 12dB 衰减，量程约 3100mV。
// 精度差一些，但四个档的窗口都很宽，不至于判错档。
static int raw_to_mv(int raw) {
    int mv = 0;
    if (s_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) return mv;
    return raw * 3100 / 4095;
}

// 每个档的迟滞半径。窄档（组合档只有 100mV 宽）不能按 12mV 硬收，否则电压稍微
// 偏一点就永远进不去 —— 所以再拿窗口宽度的 1/4 兜个上限。
static int hyst_of(int i) {
    int h = BSP_BTN_HYST_MV;
    int quarter = ((int)BTN_MV[i][1] - (int)BTN_MV[i][0]) / 4;
    if (quarter < h) h = quarter;
    return h > 0 ? h : 1;
}

// 一次判决：采样 → 平均 → 带迟滞的唯一档仲裁。
// 只在 button 组件的轮询上下文(esp_timer 任务)里跑，不需要加锁。
static void key_judge(void) {
    int sum = 0;
    for (int i = 0; i < BSP_BTN_SAMPLES; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, BSP_BTN_ADC_CHANNEL, &raw) != ESP_OK) return;
        sum += raw;
    }
    s_mv = raw_to_mv(sum / BSP_BTN_SAMPLES);

    uint8_t sel = BSP_BTN_NONE;

    // ① 维持当前档：窗口放宽，要比边界再偏出 HYST 才算离开
    if (s_cur < BSP_BTN_COUNT) {
        int h  = hyst_of(s_cur);
        int lo = (int)BTN_MV[s_cur][0] - h;
        int hi = (int)BTN_MV[s_cur][1] + h;
        if (s_mv >= lo && s_mv <= hi) sel = s_cur;
    }

    // ② 没维持住就重新选档：窗口收紧，要比边界再偏进 HYST 才算进入
    if (sel == BSP_BTN_NONE) {
        for (int i = 0; i < BSP_BTN_COUNT; i++) {
            int h = hyst_of(i);
            // 下沿 0 是 ADC 的物理下限，不能再往上收，否则这一档永远进不去
            int lo = BTN_MV[i][0] ? (int)BTN_MV[i][0] + h : 0;
            int hi = (int)BTN_MV[i][1] - h;
            if (hi < lo) {                    // 窗口比死区还窄：退化成贴着中心判
                int mid = ((int)BTN_MV[i][0] + (int)BTN_MV[i][1]) / 2;
                lo = mid - 1;
                hi = mid + 1;
            }
            if (s_mv >= lo && s_mv <= hi) { sel = (uint8_t)i; break; }
        }
    }

    // 自检：迟滞到底有没有兜住？正常按键一次就一两次翻转，噪声穿档则是几十次。
    // 100ms 内翻满 6 次才报（人手最快也就每秒十几次，不会误报），且 1 秒最多报一条，
    // 免得刷屏。看到这条日志 = 分压电阻或档位窗口该重新标定了。
    if (sel != s_cur) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (now - s_win_ms > 100) { s_win_ms = now; s_flips = 0; }
        if (++s_flips >= 6 && now - s_warn_ms > 1000) {
            s_warn_ms = now;
            ESP_LOGW(TAG, "判档抖动:100ms 内翻了 %d 次（%d mV）—— 迟滞没兜住，"
                          "检查分压电阻与 BSP_BTN_MV_TABLE", s_flips, s_mv);
        }
        ESP_LOGD(TAG, "档位 %d → %d（%d mV）", s_cur, sel, s_mv);
        s_cur = sel;
    }
}

// 组件的轮询入口：4 个键共用一路 ADC，这里做节流，保证一轮轮询只采一次样，
// 于是 4 个实例读到的都是同一个仲裁结果 —— 不会出现"这一轮里两个键同时按下"。
static uint8_t bsp_key_get_level(button_driver_t *bd) {
    bsp_adc_btn_t *d = __containerof(bd, bsp_adc_btn_t, base);
    int64_t now = esp_timer_get_time();
    if (now - s_last_us >= BSP_BTN_SAMPLE_US) {
        s_last_us = now;
        key_judge();
    }
    return (s_cur == d->index) ? BUTTON_ACTIVE : BUTTON_INACTIVE;
}

// iot_button_delete() 会无条件调用 driver->del，留 NULL 会直接崩在这里。
// driver 是静态数组、生命周期与进程同长，无需释放，这里只认个门。
static esp_err_t bsp_key_del(button_driver_t *bd) {
    (void)bd;
    return ESP_OK;
}

// 初始化中途失败时先停掉所有 button driver，再释放本文件持有的校准与 ADC unit。
// button driver 仍在轮询时不能先删 ADC，否则 timer callback 会访问失效句柄。
static void button_cleanup(void) {
    s_cb = NULL;
    s_user = NULL;
    s_ready = false;
    s_cur = BSP_BTN_NONE;
    s_mv  = -1;

    for (int i = BSP_BTN_COUNT - 1; i >= 0; i--) {
        if (!s_btn[i]) continue;
        esp_err_t e = iot_button_delete(s_btn[i]);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "按键 %d 回滚失败: %s", i, esp_err_to_name(e));
            continue;
        }
        s_btn[i] = NULL;
    }

    if (s_cali) {
        esp_err_t e = adc_cali_delete_scheme_curve_fitting(s_cali);
        if (e != ESP_OK) ESP_LOGE(TAG, "ADC 校准回滚失败: %s", esp_err_to_name(e));
        else s_cali = NULL;
    }
    if (s_adc) {
        esp_err_t e = adc_oneshot_del_unit(s_adc);
        if (e != ESP_OK) ESP_LOGE(TAG, "ADC unit 回滚失败: %s", esp_err_to_name(e));
        else s_adc = NULL;
    }
}

static esp_err_t register_callbacks(button_handle_t button, void *index) {
    esp_err_t e = iot_button_register_cb(button, BUTTON_PRESS_DOWN, NULL, cb_press, index);
    if (e == ESP_OK) e = iot_button_register_cb(button, BUTTON_SINGLE_CLICK, NULL, cb_click, index);
    if (e == ESP_OK) e = iot_button_register_cb(button, BUTTON_DOUBLE_CLICK, NULL, cb_double, index);
    if (e == ESP_OK) e = iot_button_register_cb(button, BUTTON_LONG_PRESS_START, NULL, cb_long, index);
    // 抬起边沿:CW 电键靠 press/release 两个边沿的间隔判定点划,必须有这个事件。
    if (e == ESP_OK) e = iot_button_register_cb(button, BUTTON_PRESS_UP, NULL, cb_up, index);
    return e;
}

esp_err_t bsp_button_init(bsp_btn_cb_t cb, void *user) {
    if (s_ready) {
        s_cb = cb;
        s_user = user;
        return ESP_OK;
    }
    if (s_adc || s_cali) {
        ESP_LOGE(TAG, "上次按键初始化回滚不完整，拒绝覆盖仍存活的 ADC 句柄");
        return ESP_ERR_INVALID_STATE;
    }
    for (int i = 0; i < BSP_BTN_COUNT; i++) {
        if (s_btn[i]) {
            ESP_LOGE(TAG, "上次按键 %d 回滚不完整，拒绝重复分配资源", i);
            return ESP_ERR_INVALID_STATE;
        }
    }

    s_cb = cb; s_user = user;

    // 先由 BSP 建 unit,再把句柄交给 button 组件(button_adc.h:adc_handle 非 NULL 即复用),
    // 这样本文件的 bsp_button_read_mv() 也能读同一路 ADC。
    const adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = BSP_BTN_ADC_UNIT };
    esp_err_t ae = adc_oneshot_new_unit(&ucfg, &s_adc);
    if (ae != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit 创建失败 (%s)", esp_err_to_name(ae));
        s_adc = NULL;
        button_cleanup();
        return ae;
    }

    // 通道与校准都必须在按键创建之前：本文件的 key_judge() 一被轮询就会用到它们。
    const adc_oneshot_chan_cfg_t ccfg = {
        .atten    = BSP_BTN_ATTEN,          // 与 button 组件默认的 12dB 一致
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ae = adc_oneshot_config_channel(s_adc, BSP_BTN_ADC_CHANNEL, &ccfg);
    if (ae != ESP_OK) {
        ESP_LOGE(TAG, "ADC 通道配置失败 (%s)", esp_err_to_name(ae));
        button_cleanup();
        return ae;
    }

    // 校准句柄给 key_judge() 做 raw→mV。失败不致命：raw_to_mv() 会退回线性估算，
    // 只是 Button 页显示的电压会有偏差，判档不受影响（窗口远宽于那个偏差）。
    const adc_cali_curve_fitting_config_t cal = {
        .unit_id  = BSP_BTN_ADC_UNIT,
        .chan     = BSP_BTN_ADC_CHANNEL,
        .atten    = BSP_BTN_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "ADC 校准创建失败，判档改用线性估算（电压读数可能有偏差）");
        s_cali = NULL;
    }

    for (int i = 0; i < BSP_BTN_COUNT; i++) {
        s_drv[i].base.get_key_level = bsp_key_get_level;
        s_drv[i].base.del           = bsp_key_del;      // 必填,组件删除时会直接调
        s_drv[i].index              = (uint8_t)i;
        const button_config_t bc = { 0 };
        esp_err_t e = iot_button_create(&bc, &s_drv[i].base, &s_btn[i]);
        if (e != ESP_OK || !s_btn[i]) {
            ESP_LOGE(TAG, "按键 %d 创建失败 (%s) —— 检查 GPIO%d 的 ADC 配置与分压电阻",
                     i, esp_err_to_name(e), BSP_BTN_ADC_CHANNEL);
            e = e == ESP_OK ? ESP_FAIL : e;
            button_cleanup();
            return e;
        }
        void *idx = (void *)(intptr_t)i;
        e = register_callbacks(s_btn[i], idx);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "按键 %d 回调注册失败: %s", i, esp_err_to_name(e));
            button_cleanup();
            return e;
        }
    }

    s_cur    = BSP_BTN_NONE;
    s_mv     = -1;
    s_last_us = 0;
    s_ready  = true;
    ESP_LOGI(TAG, "按键就绪:ADC1_CH%d，%d 档电压（含【下+确定】组合档），%d 次平均 + %dmV 迟滞",
             BSP_BTN_ADC_CHANNEL, BSP_BTN_COUNT, BSP_BTN_SAMPLES, BSP_BTN_HYST_MV);
    return ESP_OK;
}

int bsp_button_read_mv(void) {
    // 直接给判档用的那个读数：已经过 4 次平均，也是做了迟滞判决的同一份数据。
    // 标分压电阻时看它最准 —— 看单次采样的话，抖动会让你对档位边界产生误判。
    if (!s_ready) return -1;
    return s_mv;
}
