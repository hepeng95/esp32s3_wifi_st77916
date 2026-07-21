#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "config/lcd_config.h"
#include "drivers/st77916_lcd.h"
#include "drivers/cst820_touch.h"
#include "lvgl_port.h"
#include "wifi/wifi_scan.h"
#include "ui/wifi_scan_ui.h"

static const char *TAG = "main";

static void lvgl_task(void *pvParameter)
{
    (void)pvParameter;

    ESP_LOGI(TAG, "Initializing LVGL");
    lv_display_t *disp = lvgl_port_init();
    if (disp == NULL) {
        ESP_LOGE(TAG, "Failed to initialize LVGL port");
        vTaskDelete(NULL);
        return;
    }

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0A0E1A), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clean(scr);

    ESP_LOGI(TAG, "Starting WiFi Scan UI");
    wifi_scan_ui_create(scr);

    ESP_LOGI(TAG, "WiFi Analyzer running, touch screen to interact");

    while (1) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-S3 WiFi Analyzer - Start");

    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGI(TAG, "Initializing SPI for ST77916");
    ESP_ERROR_CHECK(st77916_spi_init());

    ESP_LOGI(TAG, "Software Reset LCD");
    ESP_ERROR_CHECK(st77916_sw_reset());

    ESP_LOGI(TAG, "Initialize LCD");
    ESP_ERROR_CHECK(st77916_lcd_init());
    ESP_LOGI(TAG, "LCD initialized");

    ESP_LOGI(TAG, "Initializing I2C for CST820");
    ESP_ERROR_CHECK(cst820_i2c_init());

    ESP_LOGI(TAG, "Initializing WiFi scan module");
    ESP_ERROR_CHECK(wifi_scan_init());

    ESP_LOGI(TAG, "Creating LVGL task");
    xTaskCreate(lvgl_task, "lvgl_task", 16384, NULL, 5, NULL);

    ESP_LOGI(TAG, "Main task exiting");
    vTaskDelete(NULL);
}