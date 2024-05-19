
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

void OLED_app_main(void)
{
    SSD1306_t dev;
    char lineChar[20];
    int i = 0;
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

    while (1)
    {
        ssd1306_clear_screen(&dev, false);
        ssd1306_contrast(&dev, 0xff);
        if (i % 2 == 0)
        {
            sprintf(&lineChar[0], "SoC:%d.", i);
            ssd1306_display_text_x2(&dev, 0, lineChar, strlen(lineChar), false);
        }
        else
        {
            sprintf(&lineChar[0], "SoC:%d .", i);
            ssd1306_display_text_x2(&dev, 0, lineChar, strlen(lineChar), false);
        }
        i += 1;
        if (i == 1000)
            i = 0;
        vTaskDelay(3000 / portTICK_PERIOD_MS);
    }
}
