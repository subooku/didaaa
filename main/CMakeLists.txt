# 只有 CW_TLS 打开才把根证书编进固件：对它的符号引用是包在 #if 里的，
# 关着的时候编进来纯粹白占 2 KB。（列表为空时 EMBEDTXTFILES 什么都不做。）
if(CONFIG_CW_TLS)
    set(CW_CA_FILE "root_ca.pem")
else()
    set(CW_CA_FILE "")
endif()

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
    # 服务器证书的信任根：main/root_ca.pem，CW_ROOT_CA_PEM 会从这里拿到字符串。
    # 只在开 TLS 时才编进来 —— 明文部署没必要白占 2 KB 的 .rodata。
    # 换自己的 CA / 自签服务器时直接覆盖这个文件，代码不用动。
    # 关键字是 EMBED_TXTFILES（IDF 里有下划线）：它会在数据末尾补 '\0'，
    # 于是 root_ca.pem 能直接当 C 字符串交给 mbedtls。
    EMBED_TXTFILES "${CW_CA_FILE}"
)
