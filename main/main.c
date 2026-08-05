#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "router_core.h"
#include "web_ui.h"
#include "led_status.h"

static const char *TAG = "MAIN";

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "ESP32 NAT Router Starting...");

    led_init();
    ESP_ERROR_CHECK(router_core_init());
    ESP_ERROR_CHECK(web_ui_start());

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
