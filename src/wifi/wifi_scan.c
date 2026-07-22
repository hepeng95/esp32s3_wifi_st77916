#include "wifi_scan.h"
#include "config/lcd_config.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>

static const char *TAG = "wifi_scan";

static wifi_scan_result_t s_scan_result = {0};
static wifi_scan_callback_t s_callback = NULL;

/* ============ OpenDroneID 协议解析 ============ */

/* OpenDroneID 消息类型 */
typedef enum {
    ODID_MSG_BASIC_ID      = 0,
    ODID_MSG_LOCATION      = 1,
    ODID_MSG_AUTH          = 2,
    ODID_MSG_SELF_ID       = 3,
    ODID_MSG_SYSTEM        = 4,
    ODID_MSG_OPERATOR_ID   = 5,
} odid_msg_type_t;

/* 识别 OpenDroneID 的 OUI 列表（常见厂商） */
static const uint8_t ODID_OUI_LIST[][3] = {
    {0xFA, 0x0B, 0xBC},  /* OpenDroneID 官方 */
    {0x90, 0x3A, 0xE6},  /* 部分无人机厂商 */
    {0x6C, 0x9C, 0xEC},  /* DIY 设备常用 */
};

#define ODID_OUI_COUNT  (sizeof(ODID_OUI_LIST) / 3)

/* 判断 OUI 是否为 OpenDroneID */
static bool odid_match_oui(const uint8_t *oui)
{
    for (size_t i = 0; i < ODID_OUI_COUNT; i++) {
        if (memcmp(oui, ODID_OUI_LIST[i], 3) == 0) return true;
    }
    return false;
}

/* 识别 Vendor Specific Content 中是否含 OpenDroneID 标识
 * ASTM F3411 规范：Vendor Specific Content 起始字节为 0x0D (Odd) 的 Message Type
 * 也可通过特定 OUI+Payload 标识判断
 */
static bool odid_is_valid_payload(const uint8_t *data, int len)
{
    if (len < 2) return false;
    /* ODID 消息第一字节: 高4位=消息类型(0-5), 低4位=协议版本(通常为1) */
    uint8_t msg_type = data[0] >> 4;
    uint8_t proto_ver = data[0] & 0x0F;
    if (msg_type > 5) return false;
    if (proto_ver == 0 || proto_ver > 3) return false;
    return true;
}

/* 解析 Basic ID 消息 (Message Type 0, 25字节) */
static void odid_parse_basic_id(const uint8_t *data, int len, wifi_ap_info_t *ap)
{
    if (len < 25) return;
    /* 数据结构:
     * [0]   类型+版本
     * [1]   ID Type (高4) + UAS Type (低4)
     * [2-19] UAS ID (16字节)
     */
    uint8_t uas_type = data[1] & 0x0F;

    /* UAS ID 为 16 字节 ASCII */
    char uas_id[17] = {0};
    memcpy(uas_id, &data[2], 16);
    /* 去除尾部空格 */
    for (int i = 15; i >= 0 && uas_id[i] == ' '; i--) uas_id[i] = '\0';

    snprintf(ap->uas_id, sizeof(ap->uas_id), "%s", uas_id);
    if (ap->rid_code[0] == '\0') {
        snprintf(ap->rid_code, sizeof(ap->rid_code), "%s", uas_id);
    }

    /* 根据 UAS Type 映射设备类型 */
    switch (uas_type) {
        case 1: ap->device_type = RID_DEVICE_TYPE_ROTOR; break;
        case 2: ap->device_type = RID_DEVICE_TYPE_FIXED_WING; break;
        case 3: ap->device_type = RID_DEVICE_TYPE_HELIX; break;
        case 4: ap->device_type = RID_DEVICE_TYPE_MULTI_ROTOR; break;
        case 5: ap->device_type = RID_DEVICE_TYPE_HYBRID; break;
        default: ap->device_type = RID_DEVICE_TYPE_OTHER; break;
    }
}

/* 解析 Location 消息 (Message Type 1, 25字节) */
static void odid_parse_location(const uint8_t *data, int len, wifi_ap_info_t *ap)
{
    if (len < 25) return;
    /* 数据结构（ASTM F3411）:
     * [0]   类型+版本
     * [1]   Status (高4) + Reserved (低4)
     * [2]   Direction (高8位, 实际 10 位中的高8位)
     * [3]   Direction 低2位 (高2) + Speed 高6位
     * [4]   Speed 低4位 (高4) + Vertical Speed 高4位
     * [5]   Vertical Speed 低8位
     * [6-9]   Latitude (4字节, 压缩整数)
     * [10-13] Longitude (4字节, 压缩整数)
     * [14]    Altitude Pressure (高8位, 16位中的高8位)
     * [15]    Altitude Pressure 低8位
     * [16]    Altitude Geodetic (高8位)
     * [17]    Altitude Geodetic 低8位
     * [18]    Height (高8位)
     * [19]    Height 低8位
     * [20]    Horiz Accuracy (高4) + Vert Accuracy (低4)
     * [21]    Baro Accuracy (高4) + Speed Accuracy (低4)
     * [22]    Time Accuracy (高4) + Reserved (低4)
     * [23]    Timestamp
     * [24]    Reserved
     */
    uint8_t status = data[1] >> 4;

    /* 方向 (0-359.9 度, 1/10 度精度) */
    uint16_t direction_raw = (data[2] << 2) | (data[3] >> 6);
    ap->heading = direction_raw * 0.1f;
    if (ap->heading >= 360.0f) ap->heading -= 360.0f;

    /* 水平速度 (0-254.25 m/s, 0.25 m/s 精度) */
    uint16_t speed_raw = ((data[3] & 0x3F) << 4) | (data[4] >> 4);
    ap->ground_speed = speed_raw * 0.25f;
    ap->speed = ap->ground_speed;

    /* 垂直速度 (±62 m/s, 0.5 m/s 精度) */
    int16_t vspeed_raw = ((data[4] & 0x0F) << 8) | data[5];
    if (vspeed_raw & 0x0800) vspeed_raw |= 0xF000;  /* 符号扩展 */
    ap->vertical_speed = vspeed_raw * 0.5f;

    /* 纬度 (±90度, 1e-7 度精度) */
    int32_t lat_raw = ((int32_t)data[6] << 24) | ((int32_t)data[7] << 16) |
                      ((int32_t)data[8] << 8) | data[9];
    ap->latitude = lat_raw / 1e7f;

    /* 经度 (±180度, 1e-7 度精度) */
    int32_t lon_raw = ((int32_t)data[10] << 24) | ((int32_t)data[11] << 16) |
                      ((int32_t)data[12] << 8) | data[13];
    ap->longitude = lon_raw / 1e7f;

    /* 气压高度 (相对海平面, 米, 0.5米精度, -1000起算) */
    uint16_t alt_p_raw = (data[14] << 8) | data[15];
    int32_t alt_pressure = alt_p_raw * 5 - 1000;

    /* 几何高度 (相对起飞点, 米, 0.5米精度, -1000起算) */
    uint16_t alt_g_raw = (data[16] << 8) | data[17];
    ap->altitude = alt_g_raw * 5 - 1000;

    /* 时间戳 (0.1秒精度) */
    ap->timestamp = data[23];

    /* 运行状态映射 */
    switch (status) {
        case 1: ap->op_state = RID_OP_STATE_IDLE; break;
        case 2: ap->op_state = RID_OP_STATE_FLYING; break;
        case 3: ap->op_state = RID_OP_STATE_LANDING; break;
        case 4: ap->op_state = RID_OP_STATE_EMERGENCY; break;
        default: ap->op_state = RID_OP_STATE_UNKNOWN; break;
    }
    (void)alt_pressure;
}

/* 解析 System 消息 (Message Type 4, 25字节) */
static void odid_parse_system(const uint8_t *data, int len, wifi_ap_info_t *ap)
{
    if (len < 25) return;
    /* [0]  类型+版本
     * [1]  Reserved
     * [2]  Operator Location Type (高2) + Classification Type (2位) + Reserved
     * [3]  Operator Latitude (高8位)
     * [4-6] Operator Latitude 低24位
     * [7]  Operator Longitude (高8位)
     * [8-10] Operator Longitude 低24位
     * ...
     * [16] Area Count (高8)
     * [17] Area Count 低8 + Area Radius (高4)
     * [18] Area Radius 低8 + Area Ceiling (高8位)
     * [19] Area Ceiling 低8
     * [20] Area Floor (高8)
     * [21] Area Floor 低8
     * [22] Category (高4) + Class (低4)
     * [23] Operator Altitude (高8)
     * [24] Operator Altitude 低8
     */
    /* 卫星数（部分实现放在 [1] 字节） */
    ap->satellites = data[1];

    /* 无人驾驶航空器分类 */
    uint8_t category = data[22] >> 4;
    if (category >= 1 && category <= 5) {
        ap->device_type = (rid_device_type_t)category;
    }
}

/* 解析 Operator ID 消息 (Message Type 5, 25字节) */
static void odid_parse_operator_id(const uint8_t *data, int len, wifi_ap_info_t *ap)
{
    if (len < 25) return;
    /* [0]  类型+版本
     * [1]  Operator ID Type
     * [2-21] Operator ID (20字节 ASCII)
     */
    char op_id[21] = {0};
    memcpy(op_id, &data[2], 20);
    for (int i = 19; i >= 0 && op_id[i] == ' '; i--) op_id[i] = '\0';
    snprintf(ap->operator_id, sizeof(ap->operator_id), "%s", op_id);
}

/* 解析 Self ID 消息 (Message Type 3, 25字节) */
static void odid_parse_self_id(const uint8_t *data, int len, wifi_ap_info_t *ap)
{
    if (len < 25) return;
    /* [0]  类型+版本
     * [1]  Self ID Type
     * [2-23] Self ID 描述 (22字节 ASCII)
     */
    char desc[23] = {0};
    memcpy(desc, &data[2], 22);
    for (int i = 21; i >= 0 && desc[i] == ' '; i--) desc[i] = '\0';
    snprintf(ap->self_id_desc, sizeof(ap->self_id_desc), "%s", desc);
}

/* 解析一条 ODID 消息 */
static void odid_parse_message(const uint8_t *data, int len, wifi_ap_info_t *ap)
{
    if (len < 1) return;
    uint8_t msg_type = data[0] >> 4;
    uint8_t proto_ver = data[0] & 0x0F;
    ESP_LOGI(TAG, "ODID msg: type=%d, ver=%d, len=%d", msg_type, proto_ver, len);

    ap->rid_version = proto_ver;
    ap->rid_from_beacon = true;

    switch (msg_type) {
        case ODID_MSG_BASIC_ID:
            odid_parse_basic_id(data, len, ap);
            break;
        case ODID_MSG_LOCATION:
            odid_parse_location(data, len, ap);
            break;
        case ODID_MSG_SELF_ID:
            odid_parse_self_id(data, len, ap);
            break;
        case ODID_MSG_SYSTEM:
            odid_parse_system(data, len, ap);
            break;
        case ODID_MSG_OPERATOR_ID:
            odid_parse_operator_id(data, len, ap);
            break;
        default:
            break;
    }
}

/* ============ Promiscuous 模式回调 ============ */

/* 临时存储待合并的 RID 设备 (来自 Beacon 解析) */
#define MAX_RID_DEVICES  16
static wifi_ap_info_t s_rid_devices[MAX_RID_DEVICES];
static volatile int s_rid_device_count = 0;

/* 查找已记录的 RID 设备 (按 BSSID 匹配) */
static wifi_ap_info_t *rid_find_device(const uint8_t *bssid)
{
    for (int i = 0; i < s_rid_device_count; i++) {
        if (memcmp(s_rid_devices[i].bssid, bssid, 6) == 0) {
            return &s_rid_devices[i];
        }
    }
    return NULL;
}

/* 添加或更新 RID 设备 */
static wifi_ap_info_t *rid_add_device(const uint8_t *bssid, int8_t rssi, uint8_t channel)
{
    wifi_ap_info_t *dev = rid_find_device(bssid);
    if (dev) {
        if (rssi > dev->rssi) dev->rssi = rssi;
        return dev;
    }
    if (s_rid_device_count >= MAX_RID_DEVICES) return NULL;

    int idx = s_rid_device_count++;
    dev = &s_rid_devices[idx];
    memset(dev, 0, sizeof(wifi_ap_info_t));
    memcpy(dev->bssid, bssid, 6);
    dev->rssi = rssi;
    dev->channel = channel;
    dev->primary_channel = channel;
    dev->is_remote_id = true;
    dev->rid_from_beacon = true;
    dev->auth_mode = WIFI_AUTH_OPEN;
    snprintf(dev->ssid, sizeof(dev->ssid), "RID-%02X%02X%02X",
             bssid[3], bssid[4], bssid[5]);
    snprintf(dev->remote_id, sizeof(dev->remote_id), "ODID-%02X%02X%02X",
             bssid[3], bssid[4], bssid[5]);
    return dev;
}

/* 解析 802.11 管理帧中的 Vendor Specific IE */
static void parse_vendor_ie(const uint8_t *payload, int len, const uint8_t *bssid,
                             int8_t rssi, uint8_t channel)
{
    /* 跳过 Fixed Fields (24字节 MAC头 + 12字节 Beacon Fixed = 36 字节) */
    if (len < 36) return;

    int offset = 36;
    while (offset + 2 < len) {
        uint8_t elem_id  = payload[offset];
        uint8_t elem_len = payload[offset + 1];

        if (offset + 2 + elem_len > len) break;

        if (elem_id == 221 && elem_len >= 5) {
            /* Vendor Specific IE: [OUI 3字节][OUI Type 1字节][Payload] */
            const uint8_t *oui = &payload[offset + 2];
            const uint8_t *vendor_payload = &payload[offset + 5];
            int vendor_len = elem_len - 3;

            /* 匹配 OUI 或检测 ODID 有效载荷 */
            if (odid_match_oui(oui) || odid_is_valid_payload(vendor_payload, vendor_len)) {
                /* 注册或查找设备 */
                wifi_ap_info_t *dev = rid_add_device(bssid, rssi, channel);
                if (dev) {
                    ESP_LOGI(TAG, "ODID Vendor IE found, BSSID=%02X:%02X:%02X:%02X:%02X:%02X, len=%d",
                             bssid[0],bssid[1],bssid[2],bssid[3],bssid[4],bssid[5], vendor_len);
                    /* ODID 消息为 25 字节 */
                    if (vendor_len >= 25) {
                        odid_parse_message(vendor_payload, 25, dev);
                    } else {
                        odid_parse_message(vendor_payload, vendor_len, dev);
                    }
                }
            }
        }
        offset += 2 + elem_len;
    }
}

/* Promiscuous 模式回调 - 在中断上下文执行，不能使用 semaphore */
static void wifi_promiscuous_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT) return;

    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *payload = pkt->payload;
    int len = pkt->rx_ctrl.sig_len;

    if (len < 24) return;

    uint8_t frame_type = payload[0] & 0x0C;
    if (frame_type != 0x00) return;

    uint8_t subtype = (payload[0] >> 4) & 0x0F;
    if (subtype != 0x08 && subtype != 0x05) return;

    const uint8_t *bssid = &payload[16];
    int8_t rssi = pkt->rx_ctrl.rssi;
    uint8_t channel = pkt->rx_ctrl.channel;

    parse_vendor_ie(payload, len, bssid, rssi, channel);
}

/* 启动 Promiscuous 监听 (持续 duration_ms 毫秒) */
static void run_promiscuous_scan(uint32_t duration_ms)
{
    ESP_LOGI(TAG, "Starting promiscuous RID scan for %lu ms", (unsigned long)duration_ms);

    s_rid_device_count = 0;
    memset(s_rid_devices, 0, sizeof(s_rid_devices));

    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT
    };
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(wifi_promiscuous_cb);
    esp_wifi_set_promiscuous(true);

    vTaskDelay(pdMS_TO_TICKS(duration_ms));

    esp_wifi_set_promiscuous(false);

    ESP_LOGI(TAG, "Promiscuous scan done, captured %d RID devices", s_rid_device_count);
}

/* 将 Promiscuous 模式捕获的 RID 设备合并到扫描结果 */
/* 注意：调用前必须先关闭 promiscuous 模式，确保无中断写入 */
static void merge_rid_devices(void)
{
    for (int i = 0; i < s_rid_device_count; i++) {
        wifi_ap_info_t *rid_dev = &s_rid_devices[i];

        bool found = false;
        for (int j = 0; j < s_scan_result.ap_count; j++) {
            if (memcmp(s_scan_result.aps[j].bssid, rid_dev->bssid, 6) == 0) {
                wifi_ap_info_t *ap = &s_scan_result.aps[j];
                ap->is_remote_id = true;
                ap->rid_from_beacon = true;
                if (rid_dev->uas_id[0]) memcpy(ap->uas_id, rid_dev->uas_id, sizeof(ap->uas_id));
                if (rid_dev->operator_id[0]) memcpy(ap->operator_id, rid_dev->operator_id, sizeof(ap->operator_id));
                if (rid_dev->rid_code[0]) memcpy(ap->rid_code, rid_dev->rid_code, sizeof(ap->rid_code));
                if (rid_dev->rid_version) ap->rid_version = rid_dev->rid_version;
                if (rid_dev->latitude != 0) ap->latitude = rid_dev->latitude;
                if (rid_dev->longitude != 0) ap->longitude = rid_dev->longitude;
                if (rid_dev->altitude != -1000) ap->altitude = rid_dev->altitude;
                if (rid_dev->speed != 0) ap->speed = rid_dev->speed;
                if (rid_dev->ground_speed != 0) ap->ground_speed = rid_dev->ground_speed;
                if (rid_dev->vertical_speed != 0) ap->vertical_speed = rid_dev->vertical_speed;
                if (rid_dev->heading != 0) ap->heading = rid_dev->heading;
                if (rid_dev->op_state != RID_OP_STATE_UNKNOWN) ap->op_state = rid_dev->op_state;
                if (rid_dev->device_type != RID_DEVICE_TYPE_UNKNOWN) ap->device_type = rid_dev->device_type;
                if (rid_dev->satellites) ap->satellites = rid_dev->satellites;
                if (rid_dev->self_id_desc[0]) memcpy(ap->self_id_desc, rid_dev->self_id_desc, sizeof(ap->self_id_desc));
                if (rid_dev->timestamp) ap->timestamp = rid_dev->timestamp;

                ESP_LOGI(TAG, "Merged ODID into existing AP[%d]: %s", j, ap->ssid);
                found = true;
                break;
            }
        }

        if (!found && s_scan_result.ap_count < WIFI_MAX_SCAN_RESULTS) {
            wifi_ap_info_t *ap = &s_scan_result.aps[s_scan_result.ap_count++];
            memcpy(ap, rid_dev, sizeof(wifi_ap_info_t));
            ESP_LOGI(TAG, "Added new ODID-only device[%d]: %s", s_scan_result.ap_count - 1, ap->ssid);
        }
    }
}


static const char *s_auth_mode_names[] = {
    "OPEN",
    "WEP",
    "WPA PSK",
    "WPA2 PSK",
    "WPA/WPA2 PSK",
    "WPA3 PSK",
    "WPA2/WPA3 PSK"
};

static const char *s_rid_device_type_names[] = {
    "Unknown",
    "Rotor",
    "Fixed Wing",
    "Helix",
    "Multi-rotor",
    "Hybrid",
    "Other"
};

static const char *s_rid_op_state_names[] = {
    "Unknown",
    "Idle",
    "Flying",
    "Landing",
    "Emergency"
};

static bool wifi_is_remote_id(const char *ssid)
{
    if (!ssid || strlen(ssid) < 4) return false;
    
    if (strncmp(ssid, "RID-", 4) == 0) return true;
    if (strncmp(ssid, "DRONE-RID-", 10) == 0) return true;
    if (strncmp(ssid, "UAV-RID-", 8) == 0) return true;
    if (strncmp(ssid, "Remote-", 7) == 0) return true;
    if (strncmp(ssid, "ID-", 3) == 0) return true;
    if (strncmp(ssid, "DJI-RID-", 7) == 0) return true;
    if (strncmp(ssid, "DJI-", 4) == 0) return true;
    
    for (int i = 0; ssid[i] != '\0'; i++) {
        if (ssid[i] == ':') return true;
    }
    
    return false;
}

static bool is_hex_char(char c)
{
    return (c >= '0' && c <= '9') || 
           (c >= 'a' && c <= 'f') || 
           (c >= 'A' && c <= 'F');
}

static void wifi_parse_gb46750_rid(const char *ssid, wifi_ap_info_t *ap)
{
    if (!ssid || !ap) return;
    
    memset(ap->rid_code, 0, sizeof(ap->rid_code));
    ap->rid_version = 0;
    ap->device_type = RID_DEVICE_TYPE_UNKNOWN;
    ap->op_state = RID_OP_STATE_UNKNOWN;
    ap->latitude = 0.0f;
    ap->longitude = 0.0f;
    ap->altitude = 0;
    ap->speed = 0.0f;
    ap->timestamp = 0;
    
    const char *p = ssid;
    
    if (strncmp(p, "RID-V", 5) == 0) {
        ap->rid_version = p[5] - '0';
        p += 7;
    } else if (strncmp(p, "DRONE-RID-", 10) == 0) {
        p += 10;
    } else if (strncmp(p, "UAV-RID-", 8) == 0) {
        p += 8;
    } else if (strncmp(p, "RID-", 4) == 0) {
        p += 4;
    } else if (strncmp(p, "DJI-RID-", 7) == 0) {
        p += 7;
    } else if (strncmp(p, "DJI-", 4) == 0) {
        p += 4;
    } else if (strncmp(p, "Remote-", 7) == 0) {
        p += 7;
        snprintf(ap->rid_code, sizeof(ap->rid_code), "%s", p);
        ap->device_type = RID_DEVICE_TYPE_MULTI_ROTOR;
        ap->op_state = RID_OP_STATE_FLYING;
        return;
    } else if (strncmp(p, "ID-", 3) == 0) {
        p += 3;
        snprintf(ap->rid_code, sizeof(ap->rid_code), "%s", p);
        return;
    } else {
        snprintf(ap->rid_code, sizeof(ap->rid_code), "%s", ssid);
        return;
    }
    
    int token_idx = 0;
    
    while (*p != '\0' && token_idx < 9) {
        char c = *p;
        if (c == '-' || c == '_' || c == ' ') {
            token_idx++;
            p++;
            continue;
        }
        
        if (token_idx == 0) {
            int code_len = 0;
            while (*p && code_len < 32 && (is_hex_char(*p) || *p == '-')) {
                ap->rid_code[code_len++] = *p++;
            }
            token_idx++;
        } else if (token_idx == 1) {
            if (strncmp(p, "MR", 2) == 0 || strncmp(p, "multi", 5) == 0) {
                ap->device_type = RID_DEVICE_TYPE_MULTI_ROTOR;
            } else if (strncmp(p, "FW", 2) == 0 || strncmp(p, "fixed", 5) == 0) {
                ap->device_type = RID_DEVICE_TYPE_FIXED_WING;
            } else if (strncmp(p, "RO", 2) == 0 || strncmp(p, "rotor", 5) == 0) {
                ap->device_type = RID_DEVICE_TYPE_ROTOR;
            } else if (strncmp(p, "HE", 2) == 0 || strncmp(p, "helix", 5) == 0) {
                ap->device_type = RID_DEVICE_TYPE_HELIX;
            } else if (strncmp(p, "HY", 2) == 0 || strncmp(p, "hybrid", 6) == 0) {
                ap->device_type = RID_DEVICE_TYPE_HYBRID;
            }
            while (*p && *p != '-' && *p != '_') p++;
        } else if (token_idx == 2) {
            if (strncmp(p, "FL", 2) == 0 || strncmp(p, "flying", 6) == 0) {
                ap->op_state = RID_OP_STATE_FLYING;
            } else if (strncmp(p, "ID", 2) == 0 || strncmp(p, "idle", 4) == 0) {
                ap->op_state = RID_OP_STATE_IDLE;
            } else if (strncmp(p, "LD", 2) == 0 || strncmp(p, "landing", 7) == 0) {
                ap->op_state = RID_OP_STATE_LANDING;
            } else if (strncmp(p, "EM", 2) == 0 || strncmp(p, "emergency", 9) == 0) {
                ap->op_state = RID_OP_STATE_EMERGENCY;
            }
            while (*p && *p != '-' && *p != '_') p++;
        } else if (token_idx == 3) {
            ap->latitude = strtof(p, (char**)&p);
        } else if (token_idx == 4) {
            ap->longitude = strtof(p, (char**)&p);
        } else if (token_idx == 5) {
            ap->altitude = (int32_t)strtof(p, (char**)&p);
        } else if (token_idx == 6) {
            ap->speed = strtof(p, (char**)&p);
        } else if (token_idx == 7) {
            ap->timestamp = (uint32_t)strtoul(p, (char**)&p, 10);
        }
        
        if (*p) p++;
    }
    
    if (ap->rid_code[0] == '\0') {
        snprintf(ap->rid_code, sizeof(ap->rid_code), "%s", ssid);
    }
}

static void wifi_extract_remote_id(const char *ssid, char *remote_id, size_t max_len)
{
    if (!ssid || !remote_id) return;
    
    if (strncmp(ssid, "RID-", 4) == 0) {
        snprintf(remote_id, max_len, "%s", ssid + 4);
    } else if (strncmp(ssid, "DRONE-RID-", 10) == 0) {
        snprintf(remote_id, max_len, "%s", ssid + 10);
    } else if (strncmp(ssid, "UAV-RID-", 8) == 0) {
        snprintf(remote_id, max_len, "%s", ssid + 8);
    } else if (strncmp(ssid, "Remote-", 7) == 0) {
        snprintf(remote_id, max_len, "%s", ssid + 7);
    } else if (strncmp(ssid, "ID-", 3) == 0) {
        snprintf(remote_id, max_len, "%s", ssid + 3);
    } else if (strncmp(ssid, "DJI-RID-", 7) == 0) {
        snprintf(remote_id, max_len, "%s", ssid + 7);
    } else if (strncmp(ssid, "DJI-", 4) == 0) {
        snprintf(remote_id, max_len, "%s", ssid + 4);
    } else {
        snprintf(remote_id, max_len, "%s", ssid);
    }
}

const char *wifi_rid_device_type_str(rid_device_type_t type)
{
    if (type >= sizeof(s_rid_device_type_names) / sizeof(s_rid_device_type_names[0])) {
        return "Unknown";
    }
    return s_rid_device_type_names[type];
}

const char *wifi_rid_op_state_str(rid_operation_state_t state)
{
    if (state >= sizeof(s_rid_op_state_names) / sizeof(s_rid_op_state_names[0])) {
        return "Unknown";
    }
    return s_rid_op_state_names[state];
}

static void wifi_scan_analyze_channels(void)
{
    memset(s_scan_result.channel_stats, 0, sizeof(s_scan_result.channel_stats));
    
    for (int i = 0; i < WIFI_CHANNEL_MAX + 1; i++) {
        s_scan_result.channel_stats[i].channel = i;
        s_scan_result.channel_stats[i].max_rssi = -128;
        s_scan_result.channel_stats[i].min_rssi = 0;
    }
    
    for (int i = 0; i < s_scan_result.ap_count; i++) {
        uint8_t channel = s_scan_result.aps[i].channel;
        if (channel > WIFI_CHANNEL_MAX) continue;
        
        wifi_channel_stat_t *stat = &s_scan_result.channel_stats[channel];
        stat->ap_count++;
        
        if (s_scan_result.aps[i].rssi > stat->max_rssi) {
            stat->max_rssi = s_scan_result.aps[i].rssi;
        }
        if (s_scan_result.aps[i].rssi < stat->min_rssi) {
            stat->min_rssi = s_scan_result.aps[i].rssi;
        }
        stat->avg_rssi += s_scan_result.aps[i].rssi;
    }
    
    for (int i = 1; i <= WIFI_CHANNEL_MAX; i++) {
        if (s_scan_result.channel_stats[i].ap_count > 0) {
            s_scan_result.channel_stats[i].avg_rssi /= s_scan_result.channel_stats[i].ap_count;
        }
    }
}

static void wifi_scan_task(void *arg)
{
    ESP_LOGI(TAG, ">>> WiFi scan task started");

    s_scan_result.scanning = true;

    /* 先清空之前的结果 */
    s_scan_result.ap_count = 0;
    memset(s_scan_result.aps, 0, sizeof(s_scan_result.aps));

    ESP_LOGI(TAG, "Calling esp_wifi_scan_start(blocking=true)...");
    esp_err_t ret = esp_wifi_scan_start(NULL, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi scan start failed: %s", esp_err_to_name(ret));
        s_scan_result.scanning = false;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "esp_wifi_scan_start OK");

    /* 获取 AP 数量 */
    uint16_t ap_total = 0;
    ret = esp_wifi_scan_get_ap_num(&ap_total);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_get_ap_num failed: %s", esp_err_to_name(ret));
        s_scan_result.scanning = false;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Found %u APs total", ap_total);

    uint16_t ap_count = (ap_total < WIFI_MAX_SCAN_RESULTS) ? ap_total : WIFI_MAX_SCAN_RESULTS;
    /* 使用静态数组避免栈溢出（wifi_ap_record_t 较大，32 个约 2.5KB） */
    static wifi_ap_record_t ap_records[WIFI_MAX_SCAN_RESULTS];
    memset(ap_records, 0, sizeof(ap_records));

    ret = esp_wifi_scan_get_ap_records(&ap_count, ap_records);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi scan get results failed: %s", esp_err_to_name(ret));
        s_scan_result.scanning = false;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Retrieved %u AP records", ap_count);

    s_scan_result.ap_count = ap_count;
    
    for (int i = 0; i < ap_count; i++) {
        memset(&s_scan_result.aps[i], 0, sizeof(wifi_ap_info_t));
        memcpy(s_scan_result.aps[i].ssid, ap_records[i].ssid, sizeof(ap_records[i].ssid));
        memcpy(s_scan_result.aps[i].bssid, ap_records[i].bssid, 6);
        s_scan_result.aps[i].rssi = ap_records[i].rssi;
        s_scan_result.aps[i].channel = ap_records[i].primary;
        s_scan_result.aps[i].primary_channel = ap_records[i].primary;
        s_scan_result.aps[i].second_channel = ap_records[i].second;
        s_scan_result.aps[i].auth_mode = ap_records[i].authmode;

        /* 识别隐藏 WiFi：SSID 为空字符串 */
        s_scan_result.aps[i].is_hidden = (ap_records[i].ssid[0] == '\0');
        /* 隐藏网络的 SSID 显示为 "(Hidden)" */
        if (s_scan_result.aps[i].is_hidden) {
            snprintf(s_scan_result.aps[i].ssid, sizeof(s_scan_result.aps[i].ssid),
                     "(Hidden %02X%02X)",
                     ap_records[i].bssid[4], ap_records[i].bssid[5]);
        }

        s_scan_result.aps[i].is_remote_id = wifi_is_remote_id((const char *)ap_records[i].ssid);
        if (s_scan_result.aps[i].is_remote_id) {
            wifi_extract_remote_id((const char *)ap_records[i].ssid,
                                  s_scan_result.aps[i].remote_id,
                                  sizeof(s_scan_result.aps[i].remote_id));
            wifi_parse_gb46750_rid((const char *)ap_records[i].ssid,
                                  &s_scan_result.aps[i]);
        }

        ESP_LOGI(TAG, "AP[%d]: %s, BSSID: %02X:%02X:%02X:%02X:%02X:%02X, RSSI: %d, CH: %d, Auth: %s, RID: %s, Hidden: %s",
                 i,
                 s_scan_result.aps[i].ssid,
                 s_scan_result.aps[i].bssid[0], s_scan_result.aps[i].bssid[1],
                 s_scan_result.aps[i].bssid[2], s_scan_result.aps[i].bssid[3],
                 s_scan_result.aps[i].bssid[4], s_scan_result.aps[i].bssid[5],
                 s_scan_result.aps[i].rssi,
                 s_scan_result.aps[i].channel,
                 wifi_auth_mode_str(s_scan_result.aps[i].auth_mode),
                 s_scan_result.aps[i].is_remote_id ? "Yes" : "No",
                 s_scan_result.aps[i].is_hidden ? "Yes" : "No");
    }
    
    wifi_scan_analyze_channels();

    /* === 第二阶段：Promiscuous 模式监听 RID Beacon (3秒) === */
    ESP_LOGI(TAG, "Starting promiscuous RID scan stage...");
    run_promiscuous_scan(3000);

    /* 合并 Promiscuous 阶段捕获的 RID 设备 */
    merge_rid_devices();

    /* 重新统计信道（RID 设备可能新增） */
    wifi_scan_analyze_channels();

    s_scan_result.scanning = false;

    if (s_callback) {
        s_callback(&s_scan_result);
    }

    ESP_LOGI(TAG, ">>> WiFi scan completed, found %d APs (incl. RID devices)",
             s_scan_result.ap_count);

    vTaskDelete(NULL);
}

esp_err_t wifi_scan_init(void)
{
    ESP_LOGI(TAG, "Initializing WiFi scan module");
    
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* 设置国家码为中国（CN），支持 1-13 信道 */
    wifi_country_t country = {
        .cc = "CN",
        .schan = 1,
        .nchan = 13,
        .policy = WIFI_COUNTRY_POLICY_AUTO,
    };
    ESP_ERROR_CHECK(esp_wifi_set_country(&country));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* STA 模式启动后立即断开，确保只扫描不连接 */
    esp_wifi_disconnect();

    ESP_LOGI(TAG, "WiFi scan module initialized (mode=STA, country=CN 1-13)");
    return ESP_OK;
}

esp_err_t wifi_scan_start(wifi_scan_callback_t callback)
{
    if (s_scan_result.scanning) {
        ESP_LOGW(TAG, "Scan already in progress");
        return ESP_ERR_INVALID_STATE;
    }
    
    s_callback = callback;

    /* 增大栈到 8192，容纳 WiFi 内部调用和大数组 */
    xTaskCreate(wifi_scan_task, "wifi_scan", 8192, NULL, 5, NULL);
    
    return ESP_OK;
}

void wifi_scan_get_result(wifi_scan_result_t *result)
{
    if (result == NULL) return;
    memcpy(result, &s_scan_result, sizeof(wifi_scan_result_t));
}

const char *wifi_auth_mode_str(wifi_auth_mode_t mode)
{
    if (mode >= sizeof(s_auth_mode_names) / sizeof(s_auth_mode_names[0])) {
        return "UNKNOWN";
    }
    return s_auth_mode_names[mode];
}

int8_t wifi_get_channel_noise(uint8_t channel)
{
    if (channel >= 1 && channel <= 13) {
        return -95;
    }
    return -100;
}