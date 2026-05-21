#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "board_pins.h"
#include "wifi_manager.h"

static const char *TAG = "APP";

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-S3 gestart");

    // NVS nodig voor WiFi
    ESP_ERROR_CHECK(nvs_flash_init());

    // LED init
    gpio_reset_pin(PIN_LED_STATUS);
    gpio_set_direction(PIN_LED_STATUS, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LED_STATUS, 0);

    ESP_LOGI(TAG, "Start firmware");

    // WiFi connect
    wifi_init_sta();

    ESP_LOGI(TAG, "WiFi verbonden");

    while (1) {
        // LED = OK status (traag knipperen)
        gpio_set_level(PIN_LED_STATUS, 1);
        vTaskDelay(pdMS_TO_TICKS(800));

        gpio_set_level(PIN_LED_STATUS, 0);
        vTaskDelay(pdMS_TO_TICKS(800));
    }
}