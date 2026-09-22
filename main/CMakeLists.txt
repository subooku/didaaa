idf_component_register(
    SRCS "main.c"
         "cw_morse.c"
         "cw_proto.c"
         "cw_radio.c"
         "cw_net.c"
         "cw_prov.c"
         "cw_ota.c"
    INCLUDE_DIRS "."
    # mqtt：信令通道。presence（含 LWT 遗嘱）、台站名单、频道占用表、短报文都走它。
    # ★ 以前漏了这一项照样能编译 —— ESP-IDF 传统构建会把所有组件的静态库都链进去。
    #   那是侥幸，依赖没声明就是没声明；补上才是正确做法。
    REQUIRES bsp bt esp_event esp_hw_support esp_http_server esp_netif esp_timer esp_wifi
             nvs_flash esp_common mqtt
             # OTA：HTTP 拉镜像（esp_http_client）+ 写另一个 app 槽（app_update）
             esp_http_client app_update
)
