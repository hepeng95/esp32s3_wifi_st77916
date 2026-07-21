#ifndef LCD_CONFIG_H
#define LCD_CONFIG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 *  LCD 分辨率配置
 * ============================================================ */
#define LCD_H_RES                   (360)
#define LCD_V_RES                   (360)
#define LCD_BITS_PER_PIXEL          (16)

/* ============================================================
 *  ST77916 LCD SPI 引脚配置 (内置PSRAM，引脚无需避冲突)
 *  使用 3-wire SPI (9-bit SPI)，无 DC 引脚
 * ============================================================ */
#define ST77916_PIN_SCLK            (7)
#define ST77916_PIN_D0              (5)     /* D0 数据线（MOSI） */
#define ST77916_PIN_MOSI            ST77916_PIN_D0  /* 兼容别名 */
#define ST77916_PIN_CS              (8)
#define ST77916_PIN_RST             (11)

/* SPI 总线参数 */
#define ST77916_SPI_HOST            SPI2_HOST
#define ST77916_SPI_MODE            (3)
#define ST77916_SPI_CLK_HZ          (80 * 1000 * 1000)

/* SPI 操作码（写命令/写颜色） */
#define ST77916_OPCODE_WRITE_CMD    (0x02)
#define ST77916_OPCODE_WRITE_COLOR  (0x02)

/* ============================================================
 *  CST820 触摸屏 I2C 配置
 * ============================================================ */
#define CST820_I2C_HOST             I2C_NUM_0
#define CST820_I2C_SDA              (10)
#define CST820_I2C_SCL              (9)
#define CST820_I2C_FREQ_HZ          (400 * 1000)
#define CST820_I2C_ADDR             (0x15)   /* 7-bit 地址 */

/* CST820 引脚 */
#define CST820_PIN_INT              (-1)    /* 无INT引脚，设为-1禁用检测 */
#define CST820_PIN_RST              (13)

/* CST820 寄存器地址 */
#define CST820_REG_TD_STATUS        (0x01)
#define CST820_REG_XH               (0x02)
#define CST820_REG_CHIP_ID          (0xA3)

/* ============================================================
 *  LVGL 与 WiFi 配置
 * ============================================================ */
#define LVGL_BUF_LINES              (40)

#define WIFI_MAX_SCAN_RESULTS       (32)
#define WIFI_SCAN_INTERVAL_MS       (5000)
#define WIFI_CHANNEL_MAX            (14)

#ifdef __cplusplus
}
#endif

#endif /* LCD_CONFIG_H */
