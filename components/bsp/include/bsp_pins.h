// components/bsp/include/bsp_pins.h
// FoloToy AI Passport 硬件引脚与参数的【单一事实来源】。换板/改硬件只需改这一个文件。
// 每项都注明"为什么是这个值",便于二次开发时判断能不能改。
#pragma once

#include "driver/spi_master.h"
#include "driver/i2c_types.h"
#include "hal/adc_types.h"

// ============================================================================
// 显示:ST7789P3 240x320,4-line SPI
// ============================================================================
#define BSP_LCD_W            240
#define BSP_LCD_H            320
#define BSP_LCD_SPI_HOST     SPI2_HOST
#define BSP_LCD_MOSI         9
#define BSP_LCD_SCLK         8
#define BSP_LCD_CS           1
#define BSP_LCD_DC           20
// -1 = 复位脚未接 MCU(硬接 3.3V),由 esp_lcd_panel_reset() 走 SWRESET 软复位。
#define BSP_LCD_RST          (-1)
#define BSP_LCD_BL           21          // 背光,LEDC PWM 调光
#define BSP_LCD_PCLK_HZ      (40 * 1000 * 1000)
// ST7789 SCK 空闲低、上升沿采样 → SPI mode 0。
#define BSP_LCD_SPI_MODE     0
// 本屏出厂即需反色(参考例程 TFT_init() 末尾无条件发 0x21 INVON)。
// 若换屏后画面呈负片,把这里改成 0。
#define BSP_LCD_INVERT_COLOR 1

// 背光 LEDC 参数。
// ★ 频率必须是超声：5 kHz 落在人耳最敏感的 2..5 kHz 区间，亮度一旦不是 100%
//   （100% 时占空比恒高、不斩波），背光的 boost 电感/陶瓷电容就会被 PWM 激励出
//   很尖的啸叫。25 kHz 越过听觉上限，10 bit 下需要 25k x 1024 = 25.6 MHz 时钟，
//   LEDC_AUTO_CLK 会选 80 MHz APB，够用。
#define BSP_BL_LEDC_TIMER    LEDC_TIMER_0
#define BSP_BL_LEDC_MODE     LEDC_LOW_SPEED_MODE
#define BSP_BL_LEDC_CHANNEL  LEDC_CHANNEL_0
#define BSP_BL_LEDC_RES      LEDC_TIMER_10_BIT
#define BSP_BL_LEDC_FREQ_HZ  25000

// ============================================================================
// 按键:三键共用一个 ADC 引脚,靠分压电阻区分
//
// 电路:3.3V ── 外部上拉 10k ──┬── ADC 节点(GPIO0 / ADC1_CH0)
//                              └── 按键 ── 分压电阻 ── GND
//
//   上   : 0Ω    → 3.3 x 0/10k       =   0 mV
//   下   : 1k    → 3.3 x 1k/11k      ≈ 300 mV
//   确定 : 2.2k  → 3.3 x 2.2k/12.2k  ≈ 595 mV
//   下+确定: 1k||2.2k≈687Ω → 3.3 x 687/10.69k ≈ 212 mV   ← 唯一能区分的组合
//   松开 : 无通路 → 上拉到 3300 mV
//
//   组合键只认 【下+确定】这一对：UP 是 0Ω 直连 GND，它跟任何键同按都还是 0mV，
//   和单按 UP 一模一样，硬件上无从区分。而 212mV 与单按 DOWN 的 300mV 相隔 88mV，
//   把 DOWN 的窗口下沿从 150 抬到 250 就能拆出这一档（两侧各留约 35mV 余量）。
//   代价是 DOWN 的有效窗口收窄了，且 ADC 抖动/电阻公差下有可能误判，故语义由
//   上层再做一次"吞掉松开时补发的单键事件"来兜底。
//
// ⚠ 不能改用【内部上拉】:约 45kΩ 且精度差,会把三档全挤到 0~154mV 并随温漂重叠。
//
// ★ 换了分压/上拉阻值怎么办:进 demo 的 Button 页,它实时显示当前 ADC 电压;
//   逐个按住三个键记下读数,取相邻两档的中点作为窗口边界,改下面的 BSP_BTN_MV 即可。
// ============================================================================
#define BSP_BTN_ADC_UNIT     ADC_UNIT_1
#define BSP_BTN_ADC_CHANNEL  ADC_CHANNEL_0    // GPIO0
// 3 个真按键 + 1 个"下+确定"组合档（不是真按键，见上）。
#define BSP_BTN_COUNT        4

// 每键的电压窗口 {min_mV, max_mV};边界取相邻档中点。
// 确定键上界留宽到 1900,是为了和松开态的 3300mV 拉开距离。
// 顺序对应 bsp_btn_t：UP / DOWN / OK / COMBO(下+确定)。
// DOWN 的下沿由 150 抬到 250,让出的 150~250 就是组合档(实测约 212mV)。
#define BSP_BTN_MV_TABLE  { {0, 150}, {250, 447}, {447, 1900}, {150, 250} }

// ============================================================================
// I2C:ES8311(音频 codec)与 CW2017(电量计)共用一条总线
// ============================================================================
#define BSP_I2C_PORT         I2C_NUM_0
#define BSP_I2C_SDA          10
#define BSP_I2C_SCL          7
#define BSP_I2C_ES8311_ADDR  0x18    // 7 位地址(8 位形式为 0x30)
#define BSP_I2C_CW2017_ADDR  0x63    // 7 位地址

// ============================================================================
// 音频:ES8311,I2S 全双工(同端口一 tx 一 rx,共用 MCLK/BCLK/WS)
// ============================================================================
#define BSP_I2S_PORT         I2S_NUM_0
#define BSP_I2S_MCLK         6
#define BSP_I2S_BCLK         5
#define BSP_I2S_WS           3
#define BSP_I2S_DOUT         2       // 播放:MCU → codec
#define BSP_I2S_DIN          4       // 录音:codec → MCU
// -1 = 功放使能脚未接 MCU(常通)。若你的板子接了,填对应 GPIO。
#define BSP_I2S_PA_CTRL      (-1)
