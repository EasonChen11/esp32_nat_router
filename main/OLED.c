
#include "OLED.h"
/*
 You have to set this config value with menuconfig
 CONFIG_INTERFACE

 for i2c
 CONFIG_MODEL
 CONFIG_SDA_GPIO
 CONFIG_SCL_GPIO
 CONFIG_RESET_GPIO

 for SPI
 CONFIG_CS_GPIO
 CONFIG_DC_GPIO
 CONFIG_RESET_GPIO
*/

#define tag "SSD1306"
char *OLED_text = NULL;
SemaphoreHandle_t OLED_xSemaphore = NULL;

void OLED_app_main(void)
{
    SSD1306_t dev;
    // char lineChar[20];
    // int i = 0;
    // OLED_text = (char *)malloc(20);
#if CONFIG_I2C_INTERFACE
    ESP_LOGI(tag, "INTERFACE is i2c");
    ESP_LOGI(tag, "CONFIG_SDA_GPIO=%d", CONFIG_SDA_GPIO);
    ESP_LOGI(tag, "CONFIG_SCL_GPIO=%d", CONFIG_SCL_GPIO);
    ESP_LOGI(tag, "CONFIG_RESET_GPIO=%d", CONFIG_RESET_GPIO);
    i2c_master_init(&dev, CONFIG_SDA_GPIO, CONFIG_SCL_GPIO, CONFIG_RESET_GPIO);
#endif // CONFIG_I2C_INTERFACE

#if CONFIG_SPI_INTERFACE
    ESP_LOGI(tag, "INTERFACE is SPI");
    ESP_LOGI(tag, "CONFIG_MOSI_GPIO=%d", CONFIG_MOSI_GPIO);
    ESP_LOGI(tag, "CONFIG_SCLK_GPIO=%d", CONFIG_SCLK_GPIO);
    ESP_LOGI(tag, "CONFIG_CS_GPIO=%d", CONFIG_CS_GPIO);
    ESP_LOGI(tag, "CONFIG_DC_GPIO=%d", CONFIG_DC_GPIO);
    ESP_LOGI(tag, "CONFIG_RESET_GPIO=%d", CONFIG_RESET_GPIO);
    spi_master_init(&dev, CONFIG_MOSI_GPIO, CONFIG_SCLK_GPIO, CONFIG_CS_GPIO, CONFIG_DC_GPIO, CONFIG_RESET_GPIO);
#endif // CONFIG_SPI_INTERFACE

#if CONFIG_FLIP
    dev._flip = true;
    ESP_LOGW(tag, "Flip upside down");
#endif
#if CONFIG_SSD1306_128x64
    ESP_LOGI(tag, "Panel is 128x64");
    ssd1306_init(&dev, 128, 64);
#endif // CONFIG_SSD1306_128x64
#if CONFIG_SSD1306_128x32
    ESP_LOGI(tag, "Panel is 128x32");
    ssd1306_init(&dev, 128, 32);
#endif // CONFIG_SSD1306_128x32

    // while (1)
    // {
    //     ssd1306_clear_screen(&dev, false);
    //     ssd1306_contrast(&dev, 0xff);
    //     ssd1306_display_text_x2(&dev, 0, OLED_text, strlen(OLED_text), false);
    //     // ssd1306_hardware_scroll(&dev, SCROLL_LEFT);
    //     vTaskDelay(3000 / portTICK_PERIOD_MS);
    // }
    int pos = 0; // 文本的起始位置
    int length;
    int display_length = 8;     // 每次顯示的字符數量
    int direction = 1;          // 滾動方向，1 為向右，-1 為向左
    char last_display[9] = {0}; // 用於保存上一次顯示的文字，以便比較
    while (1)
    {
        xSemaphoreTake(OLED_xSemaphore, portMAX_DELAY);
        length = strlen(OLED_text);
        char display_text[9] = {0}; // 用於保存每次顯示的文字
        if (length <= display_length)
        {
            strncpy(display_text, OLED_text, display_length);
            xSemaphoreGive(OLED_xSemaphore);
            display_text[length] = '\0';
            if (strcmp(last_display, display_text) != 0)
            {
                ssd1306_clear_screen(&dev, false); // clear screen
                ssd1306_display_text_x2(&dev, 0, display_text, display_length, false);
                strcpy(last_display, display_text);
            }
            pos = 0;
            vTaskDelay(3000 / portTICK_PERIOD_MS);
            continue;
        }

        strncpy(display_text, &OLED_text[pos], display_length);
        xSemaphoreGive(OLED_xSemaphore);
        display_text[display_length] = '\0';
        if (strcmp(last_display, display_text) != 0)
        {
            ssd1306_display_text_x2(&dev, 0, display_text, display_length, false);
            strcpy(last_display, display_text);
        }

        // 更新位置
        pos += direction;
        if (pos == length - display_length + 1)
        {
            vTaskDelay(500 / portTICK_PERIOD_MS); // 暫停一秒在最右邊
            pos = 0;                              // 回到開始，重新滾動
            strcpy(last_display, "");             // 確保重新顯示開始的文本
        }

        vTaskDelay(300 / portTICK_PERIOD_MS);
    }
}
// oled 單頁顯示 8個字
