#include "esp_log.h"
#include "screen_driver.h"
#include "web_server.h"
#include "wifi_ap.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "Eink E6 4.2\" 300x400 6-color (SSD2693)");

    /* 先开热点，避免首屏刷新十多秒期间完全搜不到 */
    ESP_ERROR_CHECK(wifi_ap_start());
    ESP_ERROR_CHECK(web_server_start());

    ESP_ERROR_CHECK(screen_driver_init());
    ESP_ERROR_CHECK(screen_driver_fill(SCREEN_COLOR_WHITE));

    ESP_LOGI(TAG, "join Wi-Fi \"%s\", open http://192.168.4.1", WIFI_AP_SSID);
}
