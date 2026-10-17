idf_component_register(SRCS "octop_link.c" "octop_http.c" "octop_stt.c" "octop_ws.c" "octop_wifi.c"
    INCLUDE_DIRS "include"
    REQUIRES companion_credentials nvs_flash esp_wifi esp_netif esp_event esp_http_client
        esp_websocket_client esp_timer heap spi_flash)
