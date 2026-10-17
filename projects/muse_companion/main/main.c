// SPDX-License-Identifier: Apache-2.0

#include "board_display.h"
#include "bsp/esp_mosaico.h"
#include "esp_gsp_esp_lcd.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "companion_ui.h"
#include "companion_runtime.h"
#include "companion_credentials.h"
#include "companion_setup.h"
#include "iris_screen_mirror.h"
#include "iris_ota_support.h"
#include "nvs_flash.h"
#include "ui_bundle.h"

static const char *TAG = "muse_companion";
enum { COMPANION_FONT_RENDER_STACK_BYTES = 64 * 1024 };
static companion_ui_handle_t s_companion;
extern const unsigned char font_start[] asm("_binary_NotoSansSC_Regular_ttf_start");
extern const unsigned char font_end[] asm("_binary_NotoSansSC_Regular_ttf_end");

void app_main(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_boot_probe());
    ESP_ERROR_CHECK(nvs_flash_init());

    /* Keep Recovery reachable even when the external UI image is absent or
     * invalid. */
    iris_ota_support_start();
    ESP_ERROR_CHECK(companion_credentials_init());
    ESP_ERROR_CHECK(companion_setup_start());

    esp_gsp_config_t app_config;
    const esp_err_t bundle_err = ui_bundle_open(&app_config);
    if (bundle_err != ESP_OK) {
        ESP_LOGE(TAG,
                 "GSP UI unavailable (0x%x); ESP-Iris Recovery RPC remains active",
                 bundle_err);
        return;
    }

    app_config.ttf=font_start;
    app_config.ttf_size=(size_t)(font_end-font_start);
    esp_display_present_target_config_t display;
    ESP_ERROR_CHECK(board_display_init(&display));

    esp_lcd_touch_handle_t touch = NULL;
    ESP_ERROR_CHECK(board_touch_init(&touch));

    esp_gsp_esp_lcd_config_t lcd = ESP_GSP_ESP_LCD_CONFIG_INIT();
    lcd.display = display;
    lcd.touch = touch;
    /* Render callbacks only enqueue preference writes; the internal-stack
     * voice worker owns NVS. The renderer can therefore use PSRAM safely. */
    lcd.task_stack_psram = true;
    /* FreeType's gray rasterizer alone uses a 16 KiB local pool. Chinese
     * replies exceed the default 24 KiB render stack once the GSP callback
     * and glyph resolution frames are included (archived device core). */
    lcd.task_stack_size_freetype = COMPANION_FONT_RENDER_STACK_BYTES;

    ESP_ERROR_CHECK(iris_screen_mirror_init());

    esp_gsp_handle_t ui;
    ESP_ERROR_CHECK(esp_gsp_esp_lcd_start(&app_config, &lcd, &ui));
    ESP_ERROR_CHECK(iris_screen_mirror_attach(ui));
    /* Every platform hook tolerates a null runtime, so a failed start degrades
     * to an offline companion on screen instead of aborting app_main and
     * letting the bootloader fall back to Recovery. */
    if(!companion_setup_active(NULL,NULL)){
        const esp_err_t runtime_err=companion_runtime_start();
        if(runtime_err!=ESP_OK)
            ESP_LOGE(TAG,"companion runtime unavailable: %s",esp_err_to_name(runtime_err));
    }
    ESP_ERROR_CHECK(companion_ui_create(ui, &s_companion));

    ESP_LOGI(TAG, "companion ready at %dx%d RGB565; AI hold-to-talk, display only",
             BSP_LCD_H_RES, BSP_LCD_V_RES);
}
