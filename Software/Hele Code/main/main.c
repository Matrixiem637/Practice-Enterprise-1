#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "wifi_manager.h"

#define LED_PIN GPIO_NUM_2

static const char *TAG = "APP";

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-S3 gestart");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_PIN, 0);

    ESP_LOGI(TAG, "Start firmware");

    wifi_init_sta();

    while (1) {
        gpio_set_level(LED_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(800));

        gpio_set_level(LED_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(800));
    }
}