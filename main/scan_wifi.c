/* WiFi station Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"
// #include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "SCANWIFI.h"
int scan_wifi_num = 0;
char **scan_wifi_list = NULL;

static const char *TAG = "WiFiScan";
// 执行WiFi扫描
void wifi_scan()
{
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true};
    ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_config, true)); // true 表示阻塞模式
    ESP_LOGI(TAG, "WiFi scan started");
}
// 初始化WiFi
void scan_wifi_init()
{
    esp_netif_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    // 开始扫描
    wifi_scan();
}

void print_scan_results()
{
    uint16_t number = 20; // 最大结果数
    wifi_ap_record_t ap_records[20];
    uint16_t ap_count = 0;
    memset(ap_records, 0, sizeof(ap_records));
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&number, ap_records));
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));

    ESP_LOGI(TAG, "Total APs scanned = %u", ap_count);
    scan_wifi_num = 0;
    scan_wifi_list = (char **)malloc(ap_count * sizeof(char *));
    for (int i = 0; i < ap_count; i++)
    {
        // ESP_LOGI(TAG, "SSID %s, RSSI %d", ap_records[i].ssid, ap_records[i].rssi);
        if (strlen((char *)ap_records[i].ssid) != 0)
        {
            scan_wifi_list[scan_wifi_num] = (char *)malloc(sizeof(ap_records[i].ssid));
            snprintf(scan_wifi_list[scan_wifi_num], sizeof(ap_records[i].ssid), "%s", ap_records[i].ssid);
            ESP_LOGI(TAG, "SSID %s, RSSI %d", scan_wifi_list[scan_wifi_num], ap_records[i].rssi);
            scan_wifi_num++;
        }

        // if (strcmp((char *)ap_records[i].ssid, CONFIG_ESP_WIFI_SSID) == 0)
        // {
        //     ESP_LOGI(TAG, "Found AP with SSID: %s", ap_records[i].ssid);
        // }
    }
}

void ScanStop()
{
    ESP_ERROR_CHECK(esp_wifi_stop());
    ESP_ERROR_CHECK(esp_wifi_deinit());
    ESP_ERROR_CHECK(esp_event_loop_delete_default());
    esp_netif_deinit();
}

void ScanWifi()
{
    // 初始化NVS —— 必须先初始化才能初始化WiFi
    ESP_ERROR_CHECK(nvs_flash_init());
    // 初始化WiFi
    scan_wifi_init();
    // 打印扫描结果
    print_scan_results();

    // close wifi
    ScanStop();
}
