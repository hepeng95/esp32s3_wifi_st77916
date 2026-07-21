#include "wifi_scan.h"
#include "config/lcd_config.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

static const char *TAG = "wifi_scan";

static wifi_scan_result_t s_scan_result = {0};
static wifi_scan_callback_t s_callback = NULL;

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

    s_scan_result.scanning = false;

    if (s_callback) {
        s_callback(&s_scan_result);
    }

    ESP_LOGI(TAG, ">>> WiFi scan completed, found %d APs", ap_count);

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