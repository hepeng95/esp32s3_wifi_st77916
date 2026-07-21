# ESP32-S3 WiFi Analyzer (ST77916)

基于 ESP32-S3 的便携式 WiFi 分析仪，配合 360×360 圆形 ST77916 LCD 与 CST820 电容触摸屏，支持 WiFi 扫描、信道分析、Remote ID 识别（GB 46750-2025 国标解析）等功能，UI 使用 LVGL 9 实现。

## 硬件平台

| 组件 | 规格 |
|------|------|
| 主控 | 4D Systems gen4-ESP32 R8N16（ESP32-S3，8MB Flash，16MB PSRAM） |
| 屏幕 | ST77916 QSPI LCD，360×360 圆形，RGB565 16bit |
| 触摸 | CST820 电容触摸（I2C，单点） |
| 通信 | WiFi 2.4GHz，1-13 信道（国家码 CN） |

### 引脚配置

**ST77916 LCD（3-wire SPI，无 DC 引脚）**
| 信号 | GPIO |
|------|------|
| SCLK | 7 |
| D0 / MOSI | 5 |
| CS | 8 |
| RST | 11 |
| SPI 时钟 | 80 MHz，SPI2_HOST，Mode 3 |

**CST820 触摸（I2C）**
| 信号 | GPIO |
|------|------|
| SDA | 10 |
| SCL | 9 |
| RST | 13 |
| I2C 地址 | 0x15 |
| I2C 频率 | 400 kHz |

## 目录结构

```
esp32s3_wifi_st77916/
├── src/
│   ├── main.c                      # 应用入口，硬件初始化与 LVGL 任务
│   ├── lvgl_port.c/.h              # LVGL 显示/输入适配层
│   ├── config/
│   │   └── lcd_config.h            # 分辨率、引脚、SPI/I2C、WiFi 参数
│   ├── drivers/
│   │   ├── st77916_lcd.c/.h        # ST77916 LCD + SPI DMA 驱动
│   │   └── cst820_touch.c/.h       # CST820 触摸驱动
│   ├── wifi/
│   │   ├── wifi_scan.c/.h          # WiFi 扫描与 GB 46750-2025 解析
│   └── ui/
│       └── wifi_scan_ui.c/.h       # LVGL UI 实现（4 个页面）
├── partitions_singleapp_large.csv  # 大分区表（适配 8MB Flash）
├── lv_conf.h                       # LVGL 配置
├── sdkconfig.defaults              # ESP-IDF 默认配置
├── sdkconfig.4d_systems_esp32s3_gen4_r8n16  # 板级 sdkconfig
├── scripts/extra_script.py         # PlatformIO 预处理脚本
└── platformio.ini                  # PlatformIO 工程配置
```

## 功能特性

### 1. AP 列表页（主页）
- 实时显示扫描到的 WiFi 热点（最多 32 个）
- 每条显示 SSID、RSSI、信道、信号强度条与配色
- `Scan` 手动触发扫描，`Auto` 自动刷新（10 秒间隔）
- `CH` 跳转信道分析页，`RID` 跳转 Remote ID 页
- 列表可滚动，12px 触摸滚动条，圆角 6px

### 2. AP 详情页
- 点击 AP 项进入，显示 SSID、BSSID、RSSI、信号百分比、信道（含 above/below）、认证方式
- 隐藏 WiFi 显示为 `(Hidden XXXX)`
- Remote ID 设备额外显示 RID Code、设备类型、运行状态、经纬度、高度、速度
- 内容超出屏幕时可垂直滚动

### 3. 信道分析页
- 1-13 信道柱状图，柱高按 AP 数量归一化
- 数量标签位于柱状图顶部，信道号位于底部
- 摘要显示最拥挤信道与最空闲信道
- RSSI 颜色映射：≥-50 绿、≥-60 浅绿、≥-70 黄、≥-80 橙、<-80 红

### 4. Remote ID 页（GB 46750-2025）
- 自动识别 SSID 含 `RID-` / `DRONE-RID-` / `UAV-RID-` / `DJI-RID-` / `Remote-` / `ID-` / 冒号（:）的设备
- 解析字段：
  - `rid_version`：RID 版本号
  - `rid_code`：16 位十六进制设备识别编码
  - `device_type`：旋翼/固定翼/直升机/多旋翼/混合/其他
  - `op_state`：空闲/飞行/降落/紧急
  - `latitude/longitude`：经纬度
  - `altitude/speed`：高度（米）、速度（米/秒）
  - `timestamp`：UTC 时间戳
- 紫色主题列表（#CE93D8），点击进入详情页

### 5. LVGL 优化
- SPI DMA 加速（`SPI_DMA_CH_AUTO`，`max_transfer_sz=4096`）
- 同步 `spi_device_polling_transmit` 避免 `StoreProhibited` 崩溃
- DMA 缓冲区使用 `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL` 内部内存
- LVGL 缓冲区分块传输（40 行/块）

## 编译与烧录

### 环境要求
- [PlatformIO Core](https://platformio.org/) 或 VSCode PlatformIO 插件
- ESP-IDF 工具链（PlatformIO 自动安装）
- USB 数据线（用于烧录与串口监控）

### 步骤
```bash
# 克隆仓库
git clone https://github.com/hepeng95/esp32s3_wifi_st77916.git
cd esp32s3_wifi_st77916

# 编译
pio run

# 烧录并打开串口监控（115200 bps）
pio run -t upload
pio device monitor
```

### 关键配置
- 板级：`4d_systems_esp32s3_gen4_r8n16`
- 框架：`espidf`
- Flash 分区：`partitions_singleapp_large.csv`（适配 8MB Flash）
- LVGL 字体：Montserrat 12/14/16/20/24
- LVGL 颜色深度：16bit RGB565

## UI 交互说明

| 操作 | 行为 |
|------|------|
| 点击 AP 项 | 进入 AP 详情页（移动距离 <16px 才视为点击） |
| 滑动列表 | 滚动列表内容 |
| 拖动滚动条 | 快速定位（12px 宽，可触摸） |
| `Scan` 按钮 | 手动触发一次 WiFi 扫描 |
| `Auto` 按钮 | 切换自动刷新（绿色=开，灰色=关） |
| `CH` 按钮 | 跳转信道分析页 |
| `RID` 按钮 | 跳转 Remote ID 页 |
| `< Back` 按钮 | 返回 AP 列表页 |

## 工程经验

- **SPI DMA**：避免使用 `SPI_TRANS_USE_TXDATA`（仅适用于 ≤32 位传输），大数据使用 `tx_buffer` 指针
- **DMA 内存**：SPI 缓冲区必须分配在 `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`，PSRAM 会触发缓存错误
- **LVGL 9 API**：字体宏为 `LV_FONT_MONTSERRAT_*`，样式 part 用 `LV_PART_INDICATOR`/`LV_PART_SCROLLBAR`
- **WiFi 扫描**：任务栈 ≥8KB，国家码设为 `CN` 启用 1-13 信道，扫描延迟 UI 初始化 2 秒避免竞争
- **滚动条**：移除外层容器嵌套，直接在 `page` 上创建 `lv_list`，避免触摸事件被拦截
- **大数组**：任务内大数组声明为 `static` 防止栈溢出

## 内存占用参考

```
RAM:  ~34%
Flash: ~70% (8MB 分区表)
```

## 许可

本项目仅供学习与技术研究使用。

## 仓库

GitHub: https://github.com/hepeng95/esp32s3_wifi_st77916
