/**
 * @file st77916_lcd.c
 * @brief ST77916 LCD 驱动实现 - QSPI 3-wire SPI 模式
 * 依赖：ESP-IDF SPI Master Driver
 * 移植说明：替换 SPI 读写相关的底层实现即可适配其他平台
 */

#include "drivers/st77916_lcd.h"
#include "config/lcd_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "st77916_lcd";

/* SPI 设备句柄 - 仅在本文件内可见 */
static spi_device_handle_t s_spi_dev = NULL;

/* ============================================================
 *  ST77916 初始化命令表
 * ============================================================ */
static const st77916_init_cmd_t s_init_cmds[] = {
    {0xDE, (uint8_t[]){0x00}, 1, 0},
    {0xDF, (uint8_t[]){0x98, 0x55}, 2, 0},
    {0xCE, (uint8_t[]){0x0D, 0x00}, 2, 0},
    {0xD8, (uint8_t[]){0x08, 0x00}, 2, 0},
    {0xB2, (uint8_t[]){0x30}, 1, 0},

    {0xB7, (uint8_t[]){0x01, 0x35, 0x01, 0x5D}, 4, 0},

    {0xBB, (uint8_t[]){0x1B, 0x64, 0xE3, 0x34, 0x3E, 0xF3}, 6, 0},
    {0xBC, (uint8_t[]){0x00, 0x1A, 0xF3, 0xC0}, 4, 0},

    {0xC0, (uint8_t[]){0x22, 0xC1}, 2, 0},

    {0xC3, (uint8_t[]){0x00, 0x01, 0x8D, 0x0B, 0x08, 0x48, 0x07, 0x04, 0x62, 0x30, 0x30}, 11, 0},

    {0xC4, (uint8_t[]){0x40, 0x00, 0xAD, 0x68, 0x37, 0x07, 0x04, 0x16, 0x43, 0x07, 0x04}, 11, 0},

    {0xC8, (uint8_t[]){0x3F, 0x2D, 0x22, 0x1D, 0x1D, 0x1F, 0x1B, 0x1C, 0x1B, 0x1B, 0x17, 0x0D, 0x09, 0x05, 0x01, 0x02}, 16, 0},
    {0xC8, (uint8_t[]){0x3F, 0x2D, 0x22, 0x1D, 0x1D, 0x1F, 0x1B, 0x1C, 0x1B, 0x1B, 0x17, 0x0D, 0x09, 0x05, 0x01, 0x02}, 16, 0},

    {0xD3, (uint8_t[]){0x28, 0x13}, 2, 0},
    {0xD9, (uint8_t[]){0x00, 0x00, 0xFF, 0x00, 0xF0, 0x00}, 6, 0},

    {0xDE, (uint8_t[]){0x01}, 1, 0},

    {0xB7, (uint8_t[]){0x17, 0xA7, 0x64, 0x3B, 0x06, 0x36, 0x18, 0x18}, 8, 0},
    {0xBE, (uint8_t[]){0x00}, 1, 0},
    {0xC1, (uint8_t[]){0x04, 0x40, 0x90, 0x08}, 4, 0},
    {0xC2, (uint8_t[]){0x00, 0x16, 0xDA, 0xE7}, 4, 0},
    {0xC4, (uint8_t[]){0x72, 0x12}, 2, 0},

    {0xC7, (uint8_t[]){0x00, 0x00, 0x02, 0x32, 0x10, 0x32}, 6, 0},
    {0xC8, (uint8_t[]){0x00, 0x00, 0x0B, 0x32, 0x12, 0x2E}, 6, 0},
    {0xC9, (uint8_t[]){0x00, 0x0A, 0x08, 0x06, 0x04}, 5, 0},
    {0xCA, (uint8_t[]){0x1E, 0x1F, 0x10, 0x17, 0x18}, 5, 0},
    {0xCB, (uint8_t[]){0x01, 0x0B, 0x09, 0x07, 0x05}, 5, 0},
    {0xCC, (uint8_t[]){0x1E, 0x1F, 0x11, 0x17, 0x18}, 5, 0},
    {0xCD, (uint8_t[]){0x31, 0x25, 0x27, 0x29, 0x2B}, 5, 0},
    {0xCE, (uint8_t[]){0x3F, 0x3E, 0x21, 0x37, 0x38}, 5, 0},
    {0xCF, (uint8_t[]){0x30, 0x24, 0x26, 0x28, 0x2A}, 5, 0},
    {0xD0, (uint8_t[]){0x3F, 0x3E, 0x20, 0x37, 0x38}, 5, 0},
    {0xD1, (uint8_t[]){0x06, 0x30, 0xA5, 0xDB, 0x30}, 5, 0},
    {0xD3, (uint8_t[]){0x3B, 0x08, 0x00, 0x00, 0x00, 0x00}, 6, 0},
    {0xD4, (uint8_t[]){0x67, 0x00, 0x00, 0x01, 0x00, 0x01}, 6, 0},
    {0xD5, (uint8_t[]){0x10, 0x10, 0x07, 0x07, 0x0F, 0x94, 0x26}, 7, 0},
    {0xD6, (uint8_t[]){0x00, 0x00, 0x40}, 3, 0},
    {0xD7, (uint8_t[]){0x01, 0x84, 0x20}, 3, 0},

    {0xDE, (uint8_t[]){0x02}, 1, 0},
    {0xB6, (uint8_t[]){0x1C}, 1, 0},
    {0xDE, (uint8_t[]){0x00}, 1, 0},

    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0x67}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0x67}, 4, 0},

    {0x35, NULL, 0, 0},

    {0x36, (uint8_t[]){0x00}, 1, 0},

    {0x3A, (uint8_t[]){0x55}, 1, 0},

    {0xDE, (uint8_t[]){0x00}, 1, 0},

    {0x11, NULL, 0, 120},   /* Sleep Out */
    {0x29, NULL, 0, 10},    /* Display On */
};

#define INIT_CMDS_COUNT  (sizeof(s_init_cmds) / sizeof(s_init_cmds[0]))

/* ============================================================
 *  公共接口实现
 * ============================================================ */

esp_err_t st77916_spi_init(void)
{
    ESP_LOGI(TAG, "Initializing SPI for ST77916");

    spi_bus_config_t bus_config = {
        .mosi_io_num = ST77916_PIN_D0,
        .miso_io_num = -1,
        .sclk_io_num = ST77916_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 0,
    };

    esp_err_t ret = spi_bus_initialize(ST77916_SPI_HOST, &bus_config, SPI_DMA_DISABLED);
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "SPI bus already initialized, reinitializing");
        spi_bus_free(ST77916_SPI_HOST);
        ret = spi_bus_initialize(ST77916_SPI_HOST, &bus_config, SPI_DMA_DISABLED);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    spi_device_interface_config_t dev_config = {
        .mode = ST77916_SPI_MODE,
        .clock_speed_hz = ST77916_SPI_CLK_HZ,
        .spics_io_num = ST77916_PIN_CS,
        .queue_size = 10,
    };
    ret = spi_bus_add_device(ST77916_SPI_HOST, &dev_config, &s_spi_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI add device failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "SPI initialized: SCLK=%d, MOSI=%d, CS=%d, Freq=%dHz",
             ST77916_PIN_SCLK, ST77916_PIN_D0, ST77916_PIN_CS, ST77916_SPI_CLK_HZ);
    return ESP_OK;
}

void st77916_hw_reset(void)
{
    ESP_LOGI(TAG, "Hardware reset LCD (RST pin %d)", ST77916_PIN_RST);

    gpio_reset_pin(ST77916_PIN_RST);
    gpio_set_direction(ST77916_PIN_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(ST77916_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(ST77916_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

esp_err_t st77916_sw_reset(void)
{
    ESP_LOGI(TAG, "Software reset LCD");
    esp_err_t ret = st77916_write_cmd(0x01, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
    return ret;
}

esp_err_t st77916_write_cmd(uint8_t lcd_cmd, const uint8_t *param, size_t param_size)
{
    if (s_spi_dev == NULL) return ESP_ERR_INVALID_STATE;

    uint8_t header[4] = {ST77916_OPCODE_WRITE_CMD, 0x00, lcd_cmd, 0x00};
    size_t total_size = 4 + param_size;
    uint8_t *tx_buf = malloc(total_size);
    if (!tx_buf) return ESP_ERR_NO_MEM;

    memcpy(tx_buf, header, 4);
    if (param && param_size > 0) {
        memcpy(tx_buf + 4, param, param_size);
    }

    spi_transaction_t t = {0};
    t.length = total_size * 8;
    t.tx_buffer = tx_buf;

    esp_err_t ret = spi_device_polling_transmit(s_spi_dev, &t);
    free(tx_buf);
    return ret;
}

esp_err_t st77916_write_color(uint8_t lcd_cmd, const uint8_t *data, size_t data_len)
{
    if (s_spi_dev == NULL) return ESP_ERR_INVALID_STATE;

    size_t max_trans_size;
    ESP_ERROR_CHECK(spi_bus_get_max_transaction_len(ST77916_SPI_HOST, &max_trans_size));

    uint8_t header[4] = {ST77916_OPCODE_WRITE_COLOR, 0x00, lcd_cmd, 0x00};

    esp_err_t ret = spi_device_acquire_bus(s_spi_dev, portMAX_DELAY);
    if (ret != ESP_OK) return ret;

    spi_transaction_t t = {0};
    t.length = 4 * 8;
    t.tx_buffer = header;
    t.flags = SPI_TRANS_CS_KEEP_ACTIVE;

    ret = spi_device_polling_transmit(s_spi_dev, &t);
    if (ret != ESP_OK) {
        spi_device_release_bus(s_spi_dev);
        return ret;
    }

    if (!data || data_len == 0) {
        t.flags = 0;
        t.length = 0;
        ret = spi_device_polling_transmit(s_spi_dev, &t);
        spi_device_release_bus(s_spi_dev);
        return ret;
    }

    const uint8_t *ptr = data;
    size_t remaining = data_len;

    while (remaining > 0) {
        size_t chunk_size = (remaining > max_trans_size) ? max_trans_size : remaining;

        memset(&t, 0, sizeof(t));
        t.length = chunk_size * 8;
        t.tx_buffer = ptr;

        if (remaining > chunk_size) {
            t.flags = SPI_TRANS_CS_KEEP_ACTIVE;
        }

        ret = spi_device_polling_transmit(s_spi_dev, &t);
        if (ret != ESP_OK) break;

        ptr += chunk_size;
        remaining -= chunk_size;
    }

    spi_device_release_bus(s_spi_dev);
    return ret;
}

esp_err_t st77916_set_window(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    /* Set column address (0x2A) */
    uint8_t xs[] = {
        (x1 >> 8) & 0xFF, x1 & 0xFF,
        (x2 >> 8) & 0xFF, x2 & 0xFF
    };
    esp_err_t ret = st77916_write_cmd(0x2A, xs, 4);
    if (ret != ESP_OK) return ret;

    /* Set row address (0x2B) */
    uint8_t ys[] = {
        (y1 >> 8) & 0xFF, y1 & 0xFF,
        (y2 >> 8) & 0xFF, y2 & 0xFF
    };
    return st77916_write_cmd(0x2B, ys, 4);
}

esp_err_t st77916_lcd_init(void)
{
    ESP_LOGI(TAG, "Initializing ST77916 LCD");

    for (size_t i = 0; i < INIT_CMDS_COUNT; i++) {
        const st77916_init_cmd_t *cmd = &s_init_cmds[i];
        esp_err_t ret = st77916_write_cmd(cmd->cmd, cmd->data, cmd->data_len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "LCD init cmd 0x%02X failed: %s", cmd->cmd, esp_err_to_name(ret));
            return ret;
        }
        if (cmd->delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(cmd->delay_ms));
        }
    }

    ESP_LOGI(TAG, "ST77916 LCD initialized successfully");
    return ESP_OK;
}

const st77916_init_cmd_t *st77916_get_init_cmds(size_t *count)
{
    if (count) *count = INIT_CMDS_COUNT;
    return s_init_cmds;
}
