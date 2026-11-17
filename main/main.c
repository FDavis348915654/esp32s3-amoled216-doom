// main.c —— ESP32-S3-Touch-AMOLED-2.16 上的独立 Doom 固件入口。
// 原工程（2.06 手表）走 ESP32Watch-Launcher 的 app 机制，这里改成单固件直接起 Doom。

#include "doom_app.h"

#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "Doom216";

void app_main(void)
{
    ESP_LOGI(TAG, "Starting Doom firmware (AMOLED 2.16)");

    esp_err_t err = doom_app_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Doom app: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Doom app task created");
}
