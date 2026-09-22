# ★ 根证书总是编进固件（约 3.5 KB .rodata），不再跟着 CONFIG_CW_TLS 走：
#   用不用 TLS 现在是运行时的事 —— 服务器地址填 "wss://host" 才启用，填域名/IP 走明文
#   （见 cw_net.c 的 parse_server_scheme）。编译期判断不了用户开机后会填哪个地址，
#   省这几 KB 的代价是"换到 wss 模式就因为没有根证书而连不上"，不值得。

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
             # WebSocket 客户端：服务器地址填 wss:// 时，键控、MQTT 都改走它（为了过 Cloudflare）
             espressif__esp_websocket_client
             # OTA：HTTP 拉镜像（esp_http_client）+ 写另一个 app 槽（app_update）
             esp_http_client app_update
    # 服务器证书的信任根：main/root_ca.pem，CW_ROOT_CA_PEM 会从这里拿到字符串。
    # 换自己的 CA / 自签服务器时直接覆盖这个文件，代码不用动。
    # 关键字是 EMBED_TXTFILES（IDF 里有下划线）：它会在数据末尾补 '\0'，
    # 于是 root_ca.pem 能直接当 C 字符串交给 mbedtls。
    EMBED_TXTFILES "root_ca.pem"
)
