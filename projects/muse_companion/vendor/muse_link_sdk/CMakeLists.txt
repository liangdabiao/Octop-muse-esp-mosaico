set(sdk_sources app.c ble_server.c wifi_mgr.c wifi_known.c identity.c
    config_store.c vm_api.c diagnostic_log.c stack_monitor.c bug_report.c
    link_pairing.c pairing_signer_policy.c pairing_transcript.c factory_test.c
    noise_control.cpp noise_tunnel.cpp net_discovery.c
    muse_chat_link.c muse_chat_text.c muse_text.c)
list(TRANSFORM sdk_sources PREPEND "upstream/")
idf_component_register(SRCS ${sdk_sources} "mosaico_hooks.c" "tunnel_disabled.c"
    INCLUDE_DIRS "upstream" "."
    REQUIRES nvs_flash esp_wifi esp_event esp_netif esp_http_client app_update
        esp_app_format esp-tls noise_core product_info bt driver esp_driver_gpio
        esp_driver_i2c esp_driver_spi esp_driver_uart esp_driver_ledc esp_lcd lwip
        mbedtls efuse esp_timer esp-mosaico-bsp companion_credentials)
target_include_directories(${COMPONENT_LIB} PRIVATE "${PROJECT_DIR}/main")
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
