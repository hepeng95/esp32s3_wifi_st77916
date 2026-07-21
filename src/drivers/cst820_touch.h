/**
 * @file cst820_touch.h
 * @brief CST820 电容触摸屏驱动接口 - 提供 I2C 初始化、芯片复位、坐标读取
 * 移植说明：底层使用 ESP-IDF I2C Master，若更换平台需替换 I2C 读写实现
 */

#ifndef CST820_TOUCH_H
#define CST820_TOUCH_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 触摸点坐标结构
 */
typedef struct {
    uint16_t x;
    uint16_t y;
} cst820_point_t;

/**
 * @brief 初始化 I2C 总线（用于 CST820 通信）
 * @return ESP_OK 成功，其他失败
 */
esp_err_t cst820_i2c_init(void);

/**
 * @brief 硬件复位 CST820 触摸芯片
 * @return ESP_OK 成功，其他失败
 */
esp_err_t cst820_hw_reset(void);

/**
 * @brief 初始化 CST820 触摸芯片（复位 + 读取 ChipID）
 * @return ESP_OK 成功，其他失败
 */
esp_err_t cst820_init(void);

/**
 * @brief 读取触摸点坐标
 * @param point 输出参数，存储读取到的坐标
 * @return true 检测到触摸，false 无触摸
 */
bool cst820_read_touch(cst820_point_t *point);

/**
 * @brief 读取 CST820 多个连续寄存器
 * @param reg 起始寄存器地址
 * @param buf 数据缓冲区
 * @param len 读取字节数
 * @return ESP_OK 成功，其他失败
 */
esp_err_t cst820_read_regs(uint8_t reg, uint8_t *buf, uint8_t len);

/**
 * @brief 写入 CST820 单个寄存器
 * @param reg 寄存器地址
 * @param value 写入值
 * @return ESP_OK 成功，其他失败
 */
esp_err_t cst820_write_reg(uint8_t reg, uint8_t value);

#ifdef __cplusplus
}
#endif

#endif /* CST820_TOUCH_H */
