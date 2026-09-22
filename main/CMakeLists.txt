idf_component_register(
    SRCS "main.c"
         "cw_morse.c"
         "cw_proto.c"
         "cw_radio.c"
         "cw_net.c"
         "cw_prov.c"
         "cw_ota.c"
    INCLUDE_DIRS "."
    REQUIRES bsp bt esp_event esp_hw_support esp_http_server esp_netif esp_timer esp_wifi
             nvs_flash esp_common
             # OTA：HTTP 拉镜像（esp_http_client）+ 写另一个 app 槽（app_update）
             esp_http_client app_update
)
