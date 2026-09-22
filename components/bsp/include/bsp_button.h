// components/bsp/include/bsp_button.h
// 三个按键共用一个 ADC 引脚,靠分压电阻区分。电压窗口见 bsp_pins.h。
#pragma once

#include "esp_err.h"

// 按键索引。数量用 bsp_pins.h 的 BSP_BTN_COUNT(硬件属性,归引脚表管),
// 这里不再定义尾项计数,避免出现 BSP_BTN_COUNT / BSP_BTN_COUNT_ 两个近似名字。
typedef enum {
    BSP_BTN_UP = 0,
    BSP_BTN_DOWN,
    BSP_BTN_OK,
    // 组合键【下+确定】。它不是第四个真按键 —— 两个分压电阻并联出一个第三档电压，
    // BSP 把它当独立的一个键上报（见 bsp_pins.h 的电压表）。
    // ★ 单按 DOWN 或 OK 时会先各自报一次 PRESS，等两键都按下才报 COMBO；松开时
    //   顺序相反。调用方必须自己吃掉这些"边角料"事件，否则会变成误动作。
    BSP_BTN_COMBO,
} bsp_btn_t;

typedef enum {
    BSP_BTN_PRESS = 0,   // 按下瞬间(低延迟,适合游戏类即时响应)
    BSP_BTN_CLICK,       // 单击(按下并抬起)
    BSP_BTN_DOUBLE,      // 双击
    BSP_BTN_LONG,        // 长按
    // 抬起瞬间。CW 电键必须靠"按下/抬起"两个边沿测时长,才能区分点与划,
    // 也才能自己定义长按阈值;补在枚举末尾,不影响已有调用方。
    BSP_BTN_RELEASE,
} bsp_btn_ev_t;

// 按键事件回调。运行于 button 组件使用的共享 esp_timer 任务,只能入队或执行同等级
// 的有界操作；勿在其中阻塞、访问 LVGL 或做重活。
typedef void (*bsp_btn_cb_t)(bsp_btn_t btn, bsp_btn_ev_t ev, void *user);

// 成功调用可重复，并更新回调与 user；失败会回滚本次已创建的按键和 ADC 资源。
esp_err_t bsp_button_init(bsp_btn_cb_t cb, void *user);

// 读当前 ADC 原始电压(mV)。松开时约 3300;按住某键时约为该键的分压值。
// ★ 换了分压/上拉阻值后,用它测出自己的三档电压,再改 bsp_pins.h 的 BSP_BTN_MV_TABLE。
// 读取失败返回 -1。
int bsp_button_read_mv(void);
