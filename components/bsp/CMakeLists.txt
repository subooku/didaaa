idf_component_register(
    SRCS "src/bsp_i2c.c"
         "src/bsp_display.c"
         "src/bsp_display_lvgl.c"
         "src/bsp_display_rounding.c"
         "src/bsp_button.c"
         "src/bsp_audio.c"
         "src/bsp_es8311_sleep_check.c"
         "src/bsp_battery.c"
    INCLUDE_DIRS "include"
    # esp_timer:按键判决要用 esp_timer_get_time() 做采样节流(bsp_button.c)
    REQUIRES driver esp_driver_i2c esp_driver_i2s esp_lcd esp_lvgl_port button esp_adc esp_codec_dev esp_timer
)
