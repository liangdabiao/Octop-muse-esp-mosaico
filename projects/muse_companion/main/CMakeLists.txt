idf_component_register(
    SRCS "main.c" "companion_ui.c" "companion_model.c" "companion_runtime.c" "companion_setup.c" "companion_generation.c" "board_display.c"
    INCLUDE_DIRS "."
    REQUIRES esp_iris esp-mosaico-bsp esp_mosaico_app_recovery
        esp_mosaico_gsp_bundle esp_mosaico_gsp_iris freertos nvs_flash octop_link companion_credentials
    PRIV_REQUIRES esp_timer esp-gsp esp_display_present esp_lcd esp_http_server esp_wifi esp_netif esp_event lwip lwip
    EMBED_TXTFILES "setup.html"
    EMBED_FILES "../ui/fonts/NotoSansSC-Regular.ttf")
include("${MOSAICO_UTILS_ROOT}/mosaico-tools/cmake/gsp_bundle.cmake")
mosaico_gsp_add_ui_bundle(${COMPONENT_LIB} "../ui/main.json;../ui/settings.json;../ui/read.json")
include("${CMAKE_CURRENT_LIST_DIR}/../managed_components/espressif__esp-gsp/cmake/gsp_bundle.cmake" OPTIONAL)
if(NOT COMMAND gsp_enable_freetype)
    include("${PROJECT_DIR}/managed_components/espressif__esp-gsp/cmake/gsp_bundle.cmake")
endif()
gsp_enable_freetype()
