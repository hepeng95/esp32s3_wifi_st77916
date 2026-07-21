/**
 * @file cst820_touch.c
 * @brief CST820 电容触摸屏驱动实现 - I2C 接口
 * 依赖：ESP-IDF I2C Master Driver
 * 移植说明：替换 I2C 读写相关的底层实现即可适配其他平台
 */

#include "drivers/cst820_touch.h"
#include "config/lcd_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include <string.h>

static const char *TAG = "cst820_touch";

esp_err_t cst820_i2c_init(void)
{
    ESP_LOGI(TAG, "Initializing I2C for CST820");

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = CST820_I2C_SDA,
        .scl_io_num = CST820_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = CST820_I2C_FREQ_HZ,
    };

    esp_err_t ret = i2c_param_config(CST820_I2C_HOST, &conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C param config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2c_driver_install(CST820_I2C_HOST, I2C_MODE_MASTER, 0, 0, 0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "I2C driver install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "I2C initialized: SDA=%d, SCL=%d, Freq=%dHz",
             CST820_I2C_SDA, CST820_I2C_SCL, CST820_I2C_FREQ_HZ);
    return ESP_OK;
}

esp_err_t cst820_hw_reset(void)
{
    ESP_LOGI(TAG, "Hardware reset CST820 (RST pin %d)", CST820_PIN_RST);

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << CST820_PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&io_conf);

    gpio_set_level(CST820_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(CST820_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(200));

    return ESP_OK;
}

esp_err_t cst820_read_regs(uint8_t reg, uint8_t *buf, uint8_t len)
{
    if (buf == NULL || len == 0) return ESP_ERR_INVALID_ARG;

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (CST820_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (CST820_I2C_ADDR << 1) | I2C_MASTER_READ, true);
    if (len > 1) {
        i2c_master_read(cmd, buf, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, &buf[len - 1], I2C_MASTER_NACK);
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin(CST820_I2C_HOST, cmd, pdMS_TO_TICKS(20));
    i2c_cmd_link_delete(cmd);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2C read failed (reg=0x%02X): %s", reg, esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t cst820_write_reg(uint8_t reg, uint8_t value)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (CST820_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, value, true);
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin(CST820_I2C_HOST, cmd, pdMS_TO_TICKS(10));
    i2c_cmd_link_delete(cmd);
    return ret;
}

esp_err_t cst820_init(void)
{
    ESP_LOGI(TAG, "Initializing CST820 touch chip");

    /* 硬件复位 */
    cst820_hw_reset();

    /* 配置 INT 引脚为输入（低电平有效，有触摸时拉低） */
    if (CST820_PIN_INT >= 0) {
        gpio_config_t int_conf = {
            .pin_bit_mask = (1ULL << CST820_PIN_INT),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&int_conf);
        ESP_LOGI(TAG, "INT pin configured on GPIO%d", CST820_PIN_INT);
    }

    /* 读取 Chip ID 验证通信 */
    uint8_t chip_id = 0;
    esp_err_t ret = cst820_read_regs(CST820_REG_CHIP_ID, &chip_id, 1);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "CST820 Chip ID: 0x%02X", chip_id);
    } else {
        ESP_LOGE(TAG, "Failed to read CST820 chip ID: %s", esp_err_to_name(ret));
    }

    return ret;
}

bool cst820_read_touch(cst820_point_t *point)
{
    if (point == NULL) return false;

    /* INT 引脚预检测：高电平表示无触摸，跳过 I2C 读取 */
    if (CST820_PIN_INT >= 0) {
        if (gpio_get_level(CST820_PIN_INT) == 1) {
            return false;
        }
    }

    /* CST820 寄存器布局:
     * 0x01 TD_STATUS  (低4位: 触摸点数)
     * 0x02 XH  (高4位)
     * 0x03 XL  (低8位)
     * 0x04 YH  (高4位)
     * 0x05 YL  (低8位)
     */
    uint8_t buf[6] = {0};
    esp_err_t ret = cst820_read_regs(CST820_REG_TD_STATUS, buf, 6);

    if (ret == ESP_OK && (buf[0] & 0x0F) > 0) {
        point->x = ((uint16_t)(buf[2] & 0x0F) << 8) | buf[3];
        point->y = ((uint16_t)(buf[4] & 0x0F) << 8) | buf[5];
        return true;
    }

    return false;
}
