/**
 * @file st77916_lcd.h
 * @brief ST77916 LCD 驱动接口 - 提供初始化、命令发送、像素数据写入
 * 移植说明：底层使用 ESP-IDF SPI Master，若更换平台需替换 SPI 读写实现
 */

#ifndef ST77916_LCD_H
#define ST77916_LCD_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief LCD 初始化命令结构
 */
typedef struct {
    uint8_t cmd;
    const uint8_t *data;
    uint8_t data_len;
    uint16_t delay_ms;
} st77916_init_cmd_t;

/**
 * @brief 初始化 SPI 总线并添加 ST77916 设备
 * @return ESP_OK 成功，其他失败
 */
esp_err_t st77916_spi_init(void);

/**
 * @brief 硬件复位 LCD（通过 RST 引脚）
 */
void st77916_hw_reset(void);

/**
 * @brief 软件复位 LCD（通过 SPI 发送 0x01 命令）
 * @return ESP_OK 成功，其他失败
 */
esp_err_t st77916_sw_reset(void);

/**
 * @brief 发送 LCD 命令及参数（QSPI 3-wire 格式）
 * @param lcd_cmd 命令字节
 * @param param 参数数据指针，可为 NULL
 * @param param_size 参数字节数
 * @return ESP_OK 成功，其他失败
 */
esp_err_t st77916_write_cmd(uint8_t lcd_cmd, const uint8_t *param, size_t param_size);

/**
 * @brief 发送像素颜色数据到 LCD（QSPI 3-wire 格式，支持分块传输）
 * @param lcd_cmd 命令字节（通常为 0x2C 写内存）
 * @param data 像素数据指针（RGB565，字节顺序需与 LCD 匹配）
 * @param data_len 数据字节数
 * @return ESP_OK 成功，其他失败
 */
esp_err_t st77916_write_color(uint8_t lcd_cmd, const uint8_t *data, size_t data_len);

/**
 * @brief 设置显示窗口（列地址 + 行地址）
 * @param x1 起始列
 * @param y1 起始行
 * @param x2 结束列
 * @param y2 结束行
 * @return ESP_OK 成功，其他失败
 */
esp_err_t st77916_set_window(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);

/**
 * @brief 初始化 LCD（发送初始化命令序列）
 * @return ESP_OK 成功，其他失败
 */
esp_err_t st77916_lcd_init(void);

/**
 * @brief 获取初始化命令表（用于自定义修改）
 * @return 命令表指针
 */
const st77916_init_cmd_t *st77916_get_init_cmds(size_t *count);

#ifdef __cplusplus
}
#endif

#endif /* ST77916_LCD_H */
