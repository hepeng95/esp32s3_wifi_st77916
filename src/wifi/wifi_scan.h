#ifndef WIFI_SCAN_H
#define WIFI_SCAN_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_wifi_types.h"
#include "config/lcd_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RID_DEVICE_TYPE_UNKNOWN = 0,
    RID_DEVICE_TYPE_ROTOR = 1,
    RID_DEVICE_TYPE_FIXED_WING = 2,
    RID_DEVICE_TYPE_HELIX = 3,
    RID_DEVICE_TYPE_MULTI_ROTOR = 4,
    RID_DEVICE_TYPE_HYBRID = 5,
    RID_DEVICE_TYPE_OTHER = 255
} rid_device_type_t;

typedef enum {
    RID_OP_STATE_UNKNOWN = 0,
    RID_OP_STATE_IDLE = 1,
    RID_OP_STATE_FLYING = 2,
    RID_OP_STATE_LANDING = 3,
    RID_OP_STATE_EMERGENCY = 4
} rid_operation_state_t;

typedef struct {
    char ssid[33];
    uint8_t bssid[6];           /* MAC 地址 */
    int8_t rssi;
    uint8_t channel;
    uint8_t primary_channel;    /* 主信道 */
    uint8_t second_channel;     /* 辅信道: 0=none, 1=above, 2=below */
    wifi_auth_mode_t auth_mode;
    bool is_hidden;             /* 隐藏 WiFi（SSID 为空） */

    bool is_remote_id;          /* 是否 Remote ID 设备 */
    char remote_id[64];         /* 提取的 ID 字符串 */

    /* GB 46750-2025 / OpenDroneID 字段 */
    uint8_t rid_version;        /* RID 版本号 */
    char rid_code[33];          /* 设备识别编码 */
    rid_device_type_t device_type;   /* 设备类型 */
    rid_operation_state_t op_state;  /* 运行状态 */
    float latitude;             /* 纬度（度） */
    float longitude;            /* 经度（度） */
    int32_t altitude;           /* 高度（米） */
    float speed;                /* 速度（米/秒） */
    uint32_t timestamp;         /* 时间戳（UTC） */

    /* OpenDroneID 协议字段 */
    bool rid_from_beacon;       /* RID 来自 Beacon Vendor IE (true) 或 SSID 推断 (false) */
    char uas_id[21];            /* UAS ID (无人机系统识别码, 最长20字节) */
    char operator_id[21];       /* 操作者 ID */
    float ground_speed;         /* 地速 (m/s) */
    float vertical_speed;       /* 垂直速度 (m/s) */
    float heading;              /* 航向 (度) */
    uint8_t satellites;         /* 卫星数 */
    char self_id_desc[24];      /* Self ID 描述 */
} wifi_ap_info_t;

typedef struct {
    uint8_t channel;
    uint8_t ap_count;
    int8_t max_rssi;
    int8_t min_rssi;
    int8_t avg_rssi;
} wifi_channel_stat_t;

typedef struct {
    wifi_ap_info_t aps[WIFI_MAX_SCAN_RESULTS];
    int ap_count;
    wifi_channel_stat_t channel_stats[WIFI_CHANNEL_MAX + 1];
    uint32_t scan_time_ms;
    bool scanning;
} wifi_scan_result_t;

typedef void (*wifi_scan_callback_t)(wifi_scan_result_t *result);

esp_err_t wifi_scan_init(void);
esp_err_t wifi_scan_start(wifi_scan_callback_t callback);
void wifi_scan_get_result(wifi_scan_result_t *result);
const char *wifi_auth_mode_str(wifi_auth_mode_t mode);
const char *wifi_rid_device_type_str(rid_device_type_t type);
const char *wifi_rid_op_state_str(rid_operation_state_t state);
int8_t wifi_get_channel_noise(uint8_t channel);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_SCAN_H */