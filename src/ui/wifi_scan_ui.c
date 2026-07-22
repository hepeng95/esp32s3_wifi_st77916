#include "wifi_scan_ui.h"
#include "config/lcd_config.h"
#include "wifi/wifi_scan.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "wifi_scan_ui";

/* 翻页函数前向声明 */
static void update_ap_list_page(void);
static void update_remote_id_page(void);

/* ============ 页面状态 ============ */
typedef enum {
    PAGE_AP_LIST = 0,    /* AP 列表页 */
    PAGE_AP_DETAIL,      /* AP 详情页 */
    PAGE_CHANNEL,        /* 信道分析页 */
    PAGE_REMOTE_ID,      /* Remote ID 页 */
    PAGE_COUNT
} ui_page_t;

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_pages[PAGE_COUNT] = {0};
static ui_page_t s_current_page = PAGE_AP_LIST;

/* AP 列表页元素 */
static lv_obj_t *s_ap_list_title = NULL;
static lv_obj_t *s_ap_count_label = NULL;
static lv_obj_t *s_ap_scan_btn = NULL;
static lv_obj_t *s_ap_auto_btn = NULL;    /* 自动刷新按钮 */
static lv_obj_t *s_ap_scan_status = NULL;
static lv_obj_t *s_ap_list = NULL;
static lv_obj_t *s_ap_ch_btn = NULL;       /* 跳转到信道页 */
static lv_obj_t *s_ap_rid_btn = NULL;      /* 跳转到 Remote ID 页 */
static lv_timer_t *s_update_timer = NULL;
static int s_last_ap_count = -1;

/* AP 列表翻页 */
static int s_ap_page = 0;
static int s_ap_total_pages = 1;
static lv_obj_t *s_ap_prev_btn = NULL;
static lv_obj_t *s_ap_next_btn = NULL;
static lv_obj_t *s_ap_page_label = NULL;
#define AP_LIST_PAGE_SIZE  4

/* 自动刷新 */
static bool s_auto_refresh = false;
static lv_timer_t *s_auto_refresh_timer = NULL;
#define AUTO_REFRESH_INTERVAL_MS  10000  /* 10 秒 */

/* AP 详情页元素 */
static int s_detail_ap_index = -1;
static lv_obj_t *s_detail_back_btn = NULL;
static lv_obj_t *s_detail_label = NULL;

/* 信道分析页元素 */
static lv_obj_t *s_ch_back_btn = NULL;
static lv_obj_t *s_ch_chart = NULL;
static lv_obj_t *s_ch_summary = NULL;

/* Remote ID 页元素 */
static lv_obj_t *s_rid_back_btn = NULL;
static lv_obj_t *s_rid_count = NULL;
static lv_obj_t *s_rid_list = NULL;
static int s_last_rid_count = -1;

/* RID 列表翻页 */
static int s_rid_page = 0;
static int s_rid_total_pages = 1;
static lv_obj_t *s_rid_prev_btn = NULL;
static lv_obj_t *s_rid_next_btn = NULL;
static lv_obj_t *s_rid_page_label = NULL;
#define RID_LIST_PAGE_SIZE  4

/* 延迟自动扫描 */
static lv_timer_t *s_delay_timer = NULL;

/* ============ 工具函数 ============ */
static lv_color_t wifi_rssi_color(int8_t rssi)
{
    if (rssi >= -50) return lv_color_hex(0x00E676);
    if (rssi >= -60) return lv_color_hex(0x69F0AE);
    if (rssi >= -70) return lv_color_hex(0xFFEB3B);
    if (rssi >= -80) return lv_color_hex(0xFF9800);
    return lv_color_hex(0xFF5252);
}

static int wifi_rssi_percentage(int8_t rssi)
{
    int val = (rssi + 100) * 100 / 50;
    if (val < 0) val = 0;
    if (val > 100) val = 100;
    return val;
}

static void switch_page(ui_page_t page)
{
    if (page >= PAGE_COUNT) return;
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (s_pages[i]) {
            if (i == (int)page) {
                lv_obj_clear_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    s_current_page = page;
    ESP_LOGI(TAG, "Switched to page %d", (int)page);
}

/* ============ 自动刷新 ============ */
static void auto_refresh_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_auto_refresh) return;

    wifi_scan_result_t result;
    wifi_scan_get_result(&result);
    if (result.scanning) return;  /* 扫描中不重复触发 */

    ESP_LOGI(TAG, "Auto-refresh: triggering WiFi scan");
    wifi_scan_start(NULL);
}

static void auto_btn_cb(lv_event_t *e)
{
    (void)e;
    s_auto_refresh = !s_auto_refresh;

    if (s_auto_refresh) {
        lv_obj_set_style_bg_color(s_ap_auto_btn, lv_color_hex(0x00C853), 0);
        lv_label_set_text(lv_obj_get_child(s_ap_auto_btn, 0), "Auto");
        ESP_LOGI(TAG, "Auto-refresh ON (10s interval)");
        /* 立即触发一次扫描 */
        if (s_auto_refresh_timer == NULL) {
            s_auto_refresh_timer = lv_timer_create(auto_refresh_timer_cb, AUTO_REFRESH_INTERVAL_MS, NULL);
        }
        wifi_scan_start(NULL);
    } else {
        lv_obj_set_style_bg_color(s_ap_auto_btn, lv_color_hex(0x37474F), 0);
        lv_label_set_text(lv_obj_get_child(s_ap_auto_btn, 0), "Auto");
        ESP_LOGI(TAG, "Auto-refresh OFF");
        if (s_auto_refresh_timer) {
            lv_timer_del(s_auto_refresh_timer);
            s_auto_refresh_timer = NULL;
        }
    }
}

/* ============ AP 详情页 ============ */
static void detail_back_cb(lv_event_t *e)
{
    (void)e;
    switch_page(PAGE_AP_LIST);
}

static void create_detail_page(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_set_size(page, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_bg_color(page, lv_color_hex(0x0A0E1A), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    s_pages[PAGE_AP_DETAIL] = page;

    /* 返回按钮 */
    s_detail_back_btn = lv_btn_create(page);
    lv_obj_set_size(s_detail_back_btn, 50, 22);
    lv_obj_align(s_detail_back_btn, LV_ALIGN_TOP_LEFT, 50, 70);
    lv_obj_set_style_bg_color(s_detail_back_btn, lv_color_hex(0x37474F), 0);
    lv_obj_set_style_radius(s_detail_back_btn, 8, 0);
    lv_obj_add_event_cb(s_detail_back_btn, detail_back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_lbl = lv_label_create(s_detail_back_btn);
    lv_label_set_text(back_lbl, "< Back");
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_10, 0);
    lv_obj_center(back_lbl);

    /* 标题 */
    lv_obj_t *title = lv_label_create(page);
    lv_label_set_text(title, "Details");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x4FC3F7), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 70);

    /* 详情内容：可滚动，字体 14 */
    s_detail_label = lv_label_create(page);
    lv_obj_set_style_text_font(s_detail_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_detail_label, lv_color_hex(0xE8EDF5), 0);
    lv_obj_set_style_pad_all(s_detail_label, 4, 0);
    lv_obj_set_style_text_line_space(s_detail_label, 2, LV_PART_MAIN);
    lv_obj_align(s_detail_label, LV_ALIGN_TOP_MID, 0, 100);
    lv_obj_set_width(s_detail_label, 280);
    lv_label_set_long_mode(s_detail_label, LV_LABEL_LONG_WRAP);

    lv_obj_set_scroll_dir(page, LV_DIR_VER);
}

static void update_detail_page(void)
{
    if (s_detail_ap_index < 0) return;

    wifi_scan_result_t result;
    wifi_scan_get_result(&result);

    if (s_detail_ap_index >= result.ap_count) {
        lv_label_set_text(s_detail_label, "AP not found");
        return;
    }

    wifi_ap_info_t *ap = &result.aps[s_detail_ap_index];
    const char *signal_str = "Weak";
    if (ap->rssi >= -50) signal_str = "Excellent";
    else if (ap->rssi >= -60) signal_str = "Good";
    else if (ap->rssi >= -70) signal_str = "Fair";

    char ch_str[32];
    if (ap->second_channel == 1) {
        snprintf(ch_str, sizeof(ch_str), "%d (above)", ap->primary_channel);
    } else if (ap->second_channel == 2) {
        snprintf(ch_str, sizeof(ch_str), "%d (below)", ap->primary_channel);
    } else {
        snprintf(ch_str, sizeof(ch_str), "%d", ap->primary_channel);
    }

    char buf[1600];
    if (ap->is_remote_id) {
        snprintf(buf, sizeof(buf),
            "SSID: %s\n"
            "BSSID: %02X:%02X:%02X:%02X:%02X:%02X\n"
            "RSSI: %d dBm (%s)\n"
            "Signal: %d%%\n"
            "Channel: %s\n"
            "Auth: %s\n"
            "Source: %s\n"
            "--- RID ---\n"
            "RID Code: %s\n"
            "UAS ID: %s\n"
            "Operator: %s\n"
            "Type: %s\n"
            "State: %s\n"
            "Lat: %.6f\n"
            "Lon: %.6f\n"
            "Alt: %ldm\n"
            "GSpd: %.1fm/s\n"
            "VSpd: %.1fm/s\n"
            "Hdg: %.1fdeg\n"
            "Sats: %d\n"
            "Self ID: %s\n"
            "Ver: %d",
            ap->ssid,
            ap->bssid[0], ap->bssid[1], ap->bssid[2],
            ap->bssid[3], ap->bssid[4], ap->bssid[5],
            ap->rssi, signal_str,
            wifi_rssi_percentage(ap->rssi),
            ch_str,
            wifi_auth_mode_str(ap->auth_mode),
            ap->rid_from_beacon ? "Beacon IE" : "SSID",
            ap->rid_code[0] ? ap->rid_code : ap->remote_id,
            ap->uas_id[0] ? ap->uas_id : "-",
            ap->operator_id[0] ? ap->operator_id : "-",
            wifi_rid_device_type_str(ap->device_type),
            wifi_rid_op_state_str(ap->op_state),
            ap->latitude, ap->longitude,
            ap->altitude,
            ap->ground_speed,
            ap->vertical_speed,
            ap->heading,
            ap->satellites,
            ap->self_id_desc[0] ? ap->self_id_desc : "-",
            ap->rid_version);
    } else {
        snprintf(buf, sizeof(buf),
            "SSID: %s\n"
            "BSSID: %02X:%02X:%02X:%02X:%02X:%02X\n"
            "RSSI: %d dBm (%s)\n"
            "Signal: %d%%\n"
            "Channel: %s\n"
            "Auth: %s\n"
            "Hidden: %s",
            ap->ssid,
            ap->bssid[0], ap->bssid[1], ap->bssid[2],
            ap->bssid[3], ap->bssid[4], ap->bssid[5],
            ap->rssi, signal_str,
            wifi_rssi_percentage(ap->rssi),
            ch_str,
            wifi_auth_mode_str(ap->auth_mode),
            ap->is_hidden ? "Yes" : "No");
    }
    lv_label_set_text(s_detail_label, buf);
}

/* ============ 信道分析页 ============ */
static void channel_back_cb(lv_event_t *e)
{
    (void)e;
    switch_page(PAGE_AP_LIST);
}

static void create_channel_page(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_set_size(page, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_bg_color(page, lv_color_hex(0x0A0E1A), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    s_pages[PAGE_CHANNEL] = page;

    /* 返回按钮 */
    s_ch_back_btn = lv_btn_create(page);
    lv_obj_set_size(s_ch_back_btn, 50, 22);
    lv_obj_align(s_ch_back_btn, LV_ALIGN_TOP_LEFT, 50, 70);
    lv_obj_set_style_bg_color(s_ch_back_btn, lv_color_hex(0x37474F), 0);
    lv_obj_set_style_radius(s_ch_back_btn, 8, 0);
    lv_obj_add_event_cb(s_ch_back_btn, channel_back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_lbl = lv_label_create(s_ch_back_btn);
    lv_label_set_text(back_lbl, "< Back");
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_10, 0);
    lv_obj_center(back_lbl);

    /* 标题 */
    lv_obj_t *title = lv_label_create(page);
    lv_label_set_text(title, "Channels");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x4FC3F7), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 70);

    /* 图例标签 */
    lv_obj_t *legend = lv_label_create(page);
    lv_label_set_text(legend, "AP count | CH #");
    lv_obj_set_style_text_font(legend, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(legend, lv_color_hex(0x607D8B), 0);
    lv_obj_align(legend, LV_ALIGN_TOP_MID, 0, 94);

    /* 图表容器 */
    lv_obj_t *chart_cont = lv_obj_create(page);
    lv_obj_set_size(chart_cont, 280, 170);
    lv_obj_align(chart_cont, LV_ALIGN_TOP_MID, 0, 108);
    lv_obj_set_style_bg_color(chart_cont, lv_color_hex(0x121928), 0);
    lv_obj_set_style_bg_opa(chart_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(chart_cont, 10, 0);
    lv_obj_set_style_border_opa(chart_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(chart_cont, 4, 0);
    lv_obj_clear_flag(chart_cont, LV_OBJ_FLAG_SCROLLABLE);

    s_ch_chart = lv_obj_create(chart_cont);
    lv_obj_set_size(s_ch_chart, 270, 150);
    lv_obj_align(s_ch_chart, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_ch_chart, lv_color_hex(0x0D1320), 0);
    lv_obj_set_style_bg_opa(s_ch_chart, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_ch_chart, 6, 0);
    lv_obj_set_style_border_opa(s_ch_chart, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_ch_chart, LV_OBJ_FLAG_SCROLLABLE);

    /* 摘要 */
    s_ch_summary = lv_label_create(page);
    lv_obj_set_style_text_font(s_ch_summary, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_ch_summary, lv_color_hex(0x8892A4), 0);
    lv_obj_align(s_ch_summary, LV_ALIGN_TOP_MID, 0, 288);
    lv_obj_set_width(s_ch_summary, 260);
    lv_label_set_long_mode(s_ch_summary, LV_LABEL_LONG_WRAP);
}

static void update_channel_page(void)
{
    if (!s_ch_chart) return;
    lv_obj_clean(s_ch_chart);

    wifi_scan_result_t result;
    wifi_scan_get_result(&result);

    int total_width = 260;
    int channel_width = total_width / WIFI_CHANNEL_MAX;
    int gap = 1;
    int chart_h = 100;
    int bar_bottom_offset = 22;  /* bar 底部距容器底部的偏移，为通道号留空间 */

    /* 找出最大 AP 数量用于缩放 */
    int max_ap = 1;
    for (int ch = 1; ch <= WIFI_CHANNEL_MAX; ch++) {
        if (result.channel_stats[ch].ap_count > max_ap) {
            max_ap = result.channel_stats[ch].ap_count;
        }
    }

    for (int ch = 1; ch <= WIFI_CHANNEL_MAX; ch++) {
        wifi_channel_stat_t *stat = &result.channel_stats[ch];
        int bar_h = 0;
        if (stat->ap_count > 0) {
            bar_h = (stat->ap_count * chart_h) / max_ap;
            if (bar_h < 4) bar_h = 4;  /* 最小可见高度 */
        }

        lv_obj_t *bar = lv_bar_create(s_ch_chart);
        lv_obj_set_size(bar, channel_width - gap, chart_h);
        /* bar 上移，底部留出空间显示通道号 */
        lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, (ch - 1) * channel_width + 2, -bar_bottom_offset);

        lv_obj_set_style_bg_color(bar, lv_color_hex(0x1E2736), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bar, 2, 0);

        if (stat->ap_count > 0) {
            lv_obj_set_style_bg_color(bar, wifi_rssi_color(stat->avg_rssi), LV_PART_INDICATOR);
            lv_bar_set_value(bar, (bar_h * 100) / chart_h, LV_ANIM_OFF);

            /* AP 数量标签（柱状图顶部上方） */
            lv_obj_t *cnt_lbl = lv_label_create(s_ch_chart);
            lv_label_set_text_fmt(cnt_lbl, "%d", stat->ap_count);
            lv_obj_set_style_text_font(cnt_lbl, &lv_font_montserrat_8, 0);
            lv_obj_set_style_text_color(cnt_lbl, lv_color_hex(0xB0BEC5), 0);
            lv_obj_align(cnt_lbl, LV_ALIGN_BOTTOM_LEFT,
                         (ch - 1) * channel_width + 4, -bar_bottom_offset - chart_h - 4);

            /* 信道号标签（柱状图底部下方） */
            lv_obj_t *ch_lbl = lv_label_create(s_ch_chart);
            lv_label_set_text_fmt(ch_lbl, "%d", ch);
            lv_obj_set_style_text_font(ch_lbl, &lv_font_montserrat_8, 0);
            lv_obj_set_style_text_color(ch_lbl, lv_color_hex(0x607D8B), 0);
            lv_obj_align(ch_lbl, LV_ALIGN_BOTTOM_LEFT,
                         (ch - 1) * channel_width + 4, -bar_bottom_offset + 6);
        } else {
            /* 无 AP 的信道仍显示通道号（底部下方） */
            lv_obj_t *ch_lbl = lv_label_create(s_ch_chart);
            lv_label_set_text_fmt(ch_lbl, "%d", ch);
            lv_obj_set_style_text_font(ch_lbl, &lv_font_montserrat_8, 0);
            lv_obj_set_style_text_color(ch_lbl, lv_color_hex(0x37474F), 0);
            lv_obj_align(ch_lbl, LV_ALIGN_BOTTOM_LEFT,
                         (ch - 1) * channel_width + 4, -bar_bottom_offset + 6);
        }
    }

    /* 更新摘要 */
    int busy_ch = 0;
    int max_ch = 0;
    int max_cnt = 0;
    int total_aps = 0;
    int hidden_cnt = 0;
    for (int ch = 1; ch <= WIFI_CHANNEL_MAX; ch++) {
        int cnt = result.channel_stats[ch].ap_count;
        total_aps += cnt;
        if (cnt > 0) busy_ch++;
        if (cnt > max_cnt) { max_cnt = cnt; max_ch = ch; }
    }
    for (int i = 0; i < result.ap_count; i++) {
        if (result.aps[i].is_hidden) hidden_cnt++;
    }

    char summary[160];
    if (total_aps == 0) {
        snprintf(summary, sizeof(summary), "No APs found. Tap Scan.");
    } else {
        snprintf(summary, sizeof(summary),
            "Total: %d APs  Hidden: %d\n"
            "Active: %d channels  Busiest: CH%d (%d APs)",
            total_aps, hidden_cnt, busy_ch, max_ch, max_cnt);
    }
    lv_label_set_text(s_ch_summary, summary);
}

/* ============ Remote ID 页面 ============ */
static void rid_back_cb(lv_event_t *e)
{
    (void)e;
    switch_page(PAGE_AP_LIST);
}

static void rid_item_clicked_cb(lv_event_t *e)
{
    int rid_idx = (int)(intptr_t)lv_event_get_user_data(e);

    wifi_scan_result_t result;
    wifi_scan_get_result(&result);

    int found = 0;
    int global_idx = 0;
    for (int i = 0; i < result.ap_count; i++) {
        if (result.aps[i].is_remote_id) {
            if (found == rid_idx) {
                global_idx = i;
                break;
            }
            found++;
        }
    }
    if (found != rid_idx) return;

    s_detail_ap_index = global_idx;
    update_detail_page();
    switch_page(PAGE_AP_DETAIL);
}

static void rid_prev_page_cb(lv_event_t *e)
{
    (void)e;
    if (s_rid_page > 0) {
        s_rid_page--;
        s_last_rid_count = -1;
        update_remote_id_page();
    }
}

static void rid_next_page_cb(lv_event_t *e)
{
    (void)e;
    if (s_rid_page < s_rid_total_pages - 1) {
        s_rid_page++;
        s_last_rid_count = -1;
        update_remote_id_page();
    }
}

static void create_remote_id_page(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_set_size(page, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_bg_color(page, lv_color_hex(0x0A0E1A), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    s_pages[PAGE_REMOTE_ID] = page;

    /* 返回按钮 */
    s_rid_back_btn = lv_btn_create(page);
    lv_obj_set_size(s_rid_back_btn, 50, 22);
    lv_obj_align(s_rid_back_btn, LV_ALIGN_TOP_LEFT, 50, 70);
    lv_obj_set_style_bg_color(s_rid_back_btn, lv_color_hex(0x37474F), 0);
    lv_obj_set_style_radius(s_rid_back_btn, 8, 0);
    lv_obj_add_event_cb(s_rid_back_btn, rid_back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_lbl = lv_label_create(s_rid_back_btn);
    lv_label_set_text(back_lbl, "< Back");
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_10, 0);
    lv_obj_center(back_lbl);

    /* 标题 */
    lv_obj_t *title = lv_label_create(page);
    lv_label_set_text(title, "Remote ID");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xCE93D8), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 70);

    /* Remote ID 数量 */
    s_rid_count = lv_label_create(page);
    lv_label_set_text(s_rid_count, "0 devices");
    lv_obj_set_style_text_font(s_rid_count, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_rid_count, lv_color_hex(0xCE93D8), 0);
    lv_obj_align(s_rid_count, LV_ALIGN_TOP_MID, 0, 96);

    s_rid_list = lv_obj_create(page);
    lv_obj_set_size(s_rid_list, 280, 180);
    lv_obj_align(s_rid_list, LV_ALIGN_TOP_MID, 0, 112);
    lv_obj_set_style_bg_color(s_rid_list, lv_color_hex(0x0D1320), 0);
    lv_obj_set_style_bg_opa(s_rid_list, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_rid_list, 10, 0);
    lv_obj_set_style_border_opa(s_rid_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_rid_list, 4, 0);
    lv_obj_clear_flag(s_rid_list, LV_OBJ_FLAG_SCROLLABLE);

    s_rid_prev_btn = lv_btn_create(page);
    lv_obj_set_size(s_rid_prev_btn, 70, 28);
    lv_obj_align(s_rid_prev_btn, LV_ALIGN_TOP_MID, -80, 298);
    lv_obj_set_style_bg_color(s_rid_prev_btn, lv_color_hex(0x4A148C), 0);
    lv_obj_set_style_radius(s_rid_prev_btn, 8, 0);
    lv_obj_add_event_cb(s_rid_prev_btn, rid_prev_page_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rid_prev_lbl = lv_label_create(s_rid_prev_btn);
    lv_label_set_text(rid_prev_lbl, "< Prev");
    lv_obj_set_style_text_font(rid_prev_lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(rid_prev_lbl);

    s_rid_page_label = lv_label_create(page);
    lv_label_set_text(s_rid_page_label, "1/1");
    lv_obj_set_style_text_font(s_rid_page_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_rid_page_label, lv_color_hex(0xCE93D8), 0);
    lv_obj_align(s_rid_page_label, LV_ALIGN_TOP_MID, 0, 302);

    s_rid_next_btn = lv_btn_create(page);
    lv_obj_set_size(s_rid_next_btn, 70, 28);
    lv_obj_align(s_rid_next_btn, LV_ALIGN_TOP_MID, 80, 298);
    lv_obj_set_style_bg_color(s_rid_next_btn, lv_color_hex(0x4A148C), 0);
    lv_obj_set_style_radius(s_rid_next_btn, 8, 0);
    lv_obj_add_event_cb(s_rid_next_btn, rid_next_page_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rid_next_lbl = lv_label_create(s_rid_next_btn);
    lv_label_set_text(rid_next_lbl, "Next >");
    lv_obj_set_style_text_font(rid_next_lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(rid_next_lbl);
}

static void update_remote_id_page(void)
{
    if (!s_rid_list) return;

    wifi_scan_result_t result;
    wifi_scan_get_result(&result);

    int rid_count = 0;
    for (int i = 0; i < result.ap_count; i++) {
        if (result.aps[i].is_remote_id) {
            rid_count++;
            ESP_LOGI(TAG, "RID found [%d]: SSID='%s'", i, result.aps[i].ssid);
        }
    }
    ESP_LOGI(TAG, "Total APs: %d, RID count: %d", result.ap_count, rid_count);

    lv_label_set_text_fmt(s_rid_count, "%d device%s",
                          rid_count, rid_count == 1 ? "" : "s");

    s_rid_total_pages = (rid_count + RID_LIST_PAGE_SIZE - 1) / RID_LIST_PAGE_SIZE;
    if (s_rid_total_pages < 1) s_rid_total_pages = 1;
    if (s_rid_page >= s_rid_total_pages) s_rid_page = s_rid_total_pages - 1;

    lv_label_set_text_fmt(s_rid_page_label, "%d/%d", s_rid_page + 1, s_rid_total_pages);

    if (s_rid_prev_btn) {
        if (s_rid_page == 0) {
            lv_obj_add_flag(s_rid_prev_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_rid_prev_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_rid_next_btn) {
        if (s_rid_page >= s_rid_total_pages - 1) {
            lv_obj_add_flag(s_rid_next_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_rid_next_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (s_last_rid_count == rid_count && rid_count > 0) return;
    s_last_rid_count = rid_count;

    lv_obj_clean(s_rid_list);

    if (rid_count == 0) {
        lv_obj_t *empty_lbl = lv_label_create(s_rid_list);
        lv_label_set_text(empty_lbl, "No Remote ID devices");
        lv_obj_set_style_text_font(empty_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(empty_lbl, lv_color_hex(0x607D8B), 0);
        return;
    }

    int start_idx = s_rid_page * RID_LIST_PAGE_SIZE;
    int end_idx = start_idx + RID_LIST_PAGE_SIZE;
    if (end_idx > rid_count) end_idx = rid_count;

    ESP_LOGI(TAG, "Displaying RID list page %d: items %d-%d", s_rid_page + 1, start_idx, end_idx - 1);

    int rid_idx = 0;
    int display_idx = 0;
    for (int i = 0; i < result.ap_count; i++) {
        wifi_ap_info_t *ap = &result.aps[i];
        if (!ap->is_remote_id) continue;

        if (rid_idx >= start_idx && rid_idx < end_idx) {
            int item_y = (rid_idx - start_idx) * 42 + 2;

            lv_obj_t *list_btn = lv_obj_create(s_rid_list);
            lv_obj_set_size(list_btn, 248, 40);
            lv_obj_set_pos(list_btn, 6, item_y);
            lv_obj_set_style_bg_color(list_btn, lv_color_hex(0x1A0F2E), 0);
            lv_obj_set_style_bg_opa(list_btn, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(list_btn, 6, 0);
            lv_obj_set_style_pad_all(list_btn, 0, 0);
            lv_obj_add_event_cb(list_btn, rid_item_clicked_cb, LV_EVENT_SHORT_CLICKED, (void*)(intptr_t)rid_idx);

            lv_obj_t *row = lv_obj_create(list_btn);
            lv_obj_set_size(row, 238, 34);
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
            lv_obj_set_style_pad_all(row, 0, 0);
            lv_obj_set_style_border_opa(row, LV_OPA_TRANSP, 0);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);
            lv_obj_align(row, LV_ALIGN_CENTER, 0, 0);

            lv_obj_t *name_lbl = lv_label_create(row);
            lv_label_set_text_fmt(name_lbl, "%s", ap->remote_id);
            lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(name_lbl, lv_color_hex(0xCE93D8), 0);
            lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 4, -6);
            lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
            lv_obj_set_width(name_lbl, 170);
            lv_obj_add_flag(name_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);

            lv_obj_t *rssi_lbl = lv_label_create(row);
            lv_label_set_text_fmt(rssi_lbl, "%d dBm", ap->rssi);
            lv_obj_set_style_text_font(rssi_lbl, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(rssi_lbl, wifi_rssi_color(ap->rssi), 0);
            lv_obj_align(rssi_lbl, LV_ALIGN_RIGHT_MID, -4, -6);
            lv_obj_add_flag(rssi_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);

            lv_obj_t *info_lbl = lv_label_create(row);
            lv_label_set_text_fmt(info_lbl, "CH%d | %s | %s",
                ap->channel,
                wifi_rid_device_type_str(ap->device_type),
                wifi_rid_op_state_str(ap->op_state));
            lv_obj_set_style_text_font(info_lbl, &lv_font_montserrat_10, 0);
            lv_obj_set_style_text_color(info_lbl, lv_color_hex(0x8892A4), 0);
            lv_obj_align(info_lbl, LV_ALIGN_LEFT_MID, 4, 10);
            lv_obj_add_flag(info_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);

            display_idx++;
        }
        rid_idx++;
    }
}

/* ============ AP 列表页 ============ */
static void scan_btn_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, ">>> Scan button clicked");
    lv_label_set_text(s_ap_scan_status, "Scanning");
    lv_obj_set_style_text_color(s_ap_scan_status, lv_color_hex(0xFFC107), 0);
    lv_obj_clear_flag(s_ap_scan_btn, LV_OBJ_FLAG_CLICKABLE);
    wifi_scan_start(NULL);
}

static void nav_to_channel_cb(lv_event_t *e)
{
    (void)e;
    switch_page(PAGE_CHANNEL);
}

static void nav_to_rid_cb(lv_event_t *e)
{
    (void)e;
    update_remote_id_page();
    switch_page(PAGE_REMOTE_ID);
}

static void ap_item_clicked_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    s_detail_ap_index = idx;
    ESP_LOGI(TAG, "AP item clicked, index=%d", idx);
    update_detail_page();
    switch_page(PAGE_AP_DETAIL);
}

static void ap_prev_page_cb(lv_event_t *e)
{
    (void)e;
    if (s_ap_page > 0) {
        s_ap_page--;
        s_last_ap_count = -1;
        update_ap_list_page();
    }
}

static void ap_next_page_cb(lv_event_t *e)
{
    (void)e;
    if (s_ap_page < s_ap_total_pages - 1) {
        s_ap_page++;
        s_last_ap_count = -1;
        update_ap_list_page();
    }
}

static void create_ap_list_page(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_set_size(page, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_bg_color(page, lv_color_hex(0x0A0E1A), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    s_pages[PAGE_AP_LIST] = page;

    /* 标题 */
    s_ap_list_title = lv_label_create(page);
    lv_label_set_text(s_ap_list_title, "WiFi Analyzer");
    lv_obj_set_style_text_font(s_ap_list_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_ap_list_title, lv_color_hex(0x4FC3F7), 0);
    lv_obj_align(s_ap_list_title, LV_ALIGN_TOP_MID, 0, 10);

    /* 状态 */
    s_ap_scan_status = lv_label_create(page);
    lv_label_set_text(s_ap_scan_status, "Ready");
    lv_obj_set_style_text_font(s_ap_scan_status, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_ap_scan_status, lv_color_hex(0x00E676), 0);
    lv_obj_align(s_ap_scan_status, LV_ALIGN_TOP_LEFT, 20, 36);

    /* AP 数量 */
    s_ap_count_label = lv_label_create(page);
    lv_label_set_text(s_ap_count_label, "0 APs");
    lv_obj_set_style_text_font(s_ap_count_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_ap_count_label, lv_color_hex(0x8892A4), 0);
    lv_obj_align(s_ap_count_label, LV_ALIGN_TOP_RIGHT, -20, 36);

    /* 按钮布局：RID 在上方，Scan/Auto/CH 在下方 */
    /* 上方：RID 按钮（在 Auto 正上方） */
    s_ap_rid_btn = lv_btn_create(page);
    lv_obj_set_size(s_ap_rid_btn, 76, 32);
    lv_obj_align(s_ap_rid_btn, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_style_bg_color(s_ap_rid_btn, lv_color_hex(0x6A1B9A), 0);
    lv_obj_set_style_radius(s_ap_rid_btn, 8, 0);
    lv_obj_add_event_cb(s_ap_rid_btn, nav_to_rid_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rid_lbl = lv_label_create(s_ap_rid_btn);
    lv_label_set_text(rid_lbl, "RID");
    lv_obj_set_style_text_font(rid_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(rid_lbl);

    /* 下方：Scan + Auto + CH 三个按钮 */
    s_ap_scan_btn = lv_btn_create(page);
    lv_obj_set_size(s_ap_scan_btn, 76, 32);
    lv_obj_align(s_ap_scan_btn, LV_ALIGN_TOP_MID, -80, 90);
    lv_obj_set_style_bg_color(s_ap_scan_btn, lv_color_hex(0x1E88E5), 0);
    lv_obj_set_style_radius(s_ap_scan_btn, 8, 0);
    lv_obj_add_event_cb(s_ap_scan_btn, scan_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *scan_lbl = lv_label_create(s_ap_scan_btn);
    lv_label_set_text(scan_lbl, "Scan");
    lv_obj_set_style_text_font(scan_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(scan_lbl);

    s_ap_auto_btn = lv_btn_create(page);
    lv_obj_set_size(s_ap_auto_btn, 76, 32);
    lv_obj_align(s_ap_auto_btn, LV_ALIGN_TOP_MID, 0, 90);
    lv_obj_set_style_bg_color(s_ap_auto_btn, lv_color_hex(0x37474F), 0);
    lv_obj_set_style_radius(s_ap_auto_btn, 8, 0);
    lv_obj_add_event_cb(s_ap_auto_btn, auto_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *auto_lbl = lv_label_create(s_ap_auto_btn);
    lv_label_set_text(auto_lbl, "Auto");
    lv_obj_set_style_text_font(auto_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(auto_lbl);

    s_ap_ch_btn = lv_btn_create(page);
    lv_obj_set_size(s_ap_ch_btn, 76, 32);
    lv_obj_align(s_ap_ch_btn, LV_ALIGN_TOP_MID, 80, 88);
    lv_obj_set_style_bg_color(s_ap_ch_btn, lv_color_hex(0x37474F), 0);
    lv_obj_set_style_radius(s_ap_ch_btn, 8, 0);
    lv_obj_add_event_cb(s_ap_ch_btn, nav_to_channel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ch_lbl = lv_label_create(s_ap_ch_btn);
    lv_label_set_text(ch_lbl, "CH");
    lv_obj_set_style_text_font(ch_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(ch_lbl);

    s_ap_list = lv_obj_create(page);
    lv_obj_set_size(s_ap_list, 280, 168);
    lv_obj_align(s_ap_list, LV_ALIGN_TOP_MID, 0, 124);
    lv_obj_set_style_bg_color(s_ap_list, lv_color_hex(0x0D1320), 0);
    lv_obj_set_style_bg_opa(s_ap_list, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_ap_list, 10, 0);
    lv_obj_set_style_border_opa(s_ap_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_ap_list, 4, 0);
    lv_obj_clear_flag(s_ap_list, LV_OBJ_FLAG_SCROLLABLE);

    s_ap_prev_btn = lv_btn_create(page);
    lv_obj_set_size(s_ap_prev_btn, 70, 28);
    lv_obj_align(s_ap_prev_btn, LV_ALIGN_TOP_MID, -80, 298);
    lv_obj_set_style_bg_color(s_ap_prev_btn, lv_color_hex(0x37474F), 0);
    lv_obj_set_style_radius(s_ap_prev_btn, 8, 0);
    lv_obj_add_event_cb(s_ap_prev_btn, ap_prev_page_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *prev_lbl = lv_label_create(s_ap_prev_btn);
    lv_label_set_text(prev_lbl, "< Prev");
    lv_obj_set_style_text_font(prev_lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(prev_lbl);

    s_ap_page_label = lv_label_create(page);
    lv_label_set_text(s_ap_page_label, "1/1");
    lv_obj_set_style_text_font(s_ap_page_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_ap_page_label, lv_color_hex(0x8892A4), 0);
    lv_obj_align(s_ap_page_label, LV_ALIGN_TOP_MID, 0, 302);

    s_ap_next_btn = lv_btn_create(page);
    lv_obj_set_size(s_ap_next_btn, 70, 28);
    lv_obj_align(s_ap_next_btn, LV_ALIGN_TOP_MID, 80, 298);
    lv_obj_set_style_bg_color(s_ap_next_btn, lv_color_hex(0x37474F), 0);
    lv_obj_set_style_radius(s_ap_next_btn, 8, 0);
    lv_obj_add_event_cb(s_ap_next_btn, ap_next_page_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *next_lbl = lv_label_create(s_ap_next_btn);
    lv_label_set_text(next_lbl, "Next >");
    lv_obj_set_style_text_font(next_lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(next_lbl);
}

static void update_ap_list_page(void)
{
    if (!s_ap_list) return;

    wifi_scan_result_t result;
    wifi_scan_get_result(&result);

    lv_label_set_text_fmt(s_ap_count_label, "%d APs", result.ap_count);

    s_ap_total_pages = (result.ap_count + AP_LIST_PAGE_SIZE - 1) / AP_LIST_PAGE_SIZE;
    if (s_ap_total_pages < 1) s_ap_total_pages = 1;
    if (s_ap_page >= s_ap_total_pages) s_ap_page = s_ap_total_pages - 1;

    lv_label_set_text_fmt(s_ap_page_label, "%d/%d", s_ap_page + 1, s_ap_total_pages);

    if (s_ap_prev_btn) {
        if (s_ap_page == 0) {
            lv_obj_add_flag(s_ap_prev_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_ap_prev_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_ap_next_btn) {
        if (s_ap_page >= s_ap_total_pages - 1) {
            lv_obj_add_flag(s_ap_next_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_ap_next_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (s_last_ap_count == result.ap_count && result.ap_count > 0) return;
    s_last_ap_count = result.ap_count;

    lv_obj_clean(s_ap_list);

    if (result.ap_count == 0) {
        lv_obj_t *empty_lbl = lv_label_create(s_ap_list);
        lv_label_set_text(empty_lbl, "No APs - tap Scan");
        lv_obj_set_style_text_font(empty_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(empty_lbl, lv_color_hex(0x607D8B), 0);
        return;
    }

    int start_idx = s_ap_page * AP_LIST_PAGE_SIZE;
    int end_idx = start_idx + AP_LIST_PAGE_SIZE;
    if (end_idx > result.ap_count) end_idx = result.ap_count;

    ESP_LOGI(TAG, "Displaying AP list page %d: items %d-%d", s_ap_page + 1, start_idx, end_idx - 1);

    for (int i = start_idx; i < end_idx; i++) {
        wifi_ap_info_t *ap = &result.aps[i];
        int item_y = (i - start_idx) * 40 + 2;

        lv_obj_t *list_btn = lv_obj_create(s_ap_list);
        lv_obj_set_size(list_btn, 256, 38);
        lv_obj_set_pos(list_btn, 6, item_y);
        lv_obj_set_style_bg_color(list_btn, lv_color_hex(0x161D2A), 0);
        lv_obj_set_style_bg_opa(list_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(list_btn, 6, 0);
        lv_obj_set_style_pad_all(list_btn, 0, 0);
        lv_obj_add_event_cb(list_btn, ap_item_clicked_cb, LV_EVENT_SHORT_CLICKED, (void*)(intptr_t)i);

        lv_obj_t *row = lv_obj_create(list_btn);
        lv_obj_set_size(row, 246, 32);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_border_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_align(row, LV_ALIGN_CENTER, 0, 0);

        lv_obj_t *ssid_label = lv_label_create(row);
        lv_color_t ssid_color = lv_color_hex(0xE8EDF5);
        if (ap->is_remote_id) ssid_color = lv_color_hex(0xCE93D8);
        else if (ap->is_hidden) ssid_color = lv_color_hex(0xFF8A65);
        lv_label_set_text_fmt(ssid_label, "%s%s",
                              ap->is_hidden ? "[H] " : "",
                              ap->ssid);
        lv_obj_set_style_text_font(ssid_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(ssid_label, ssid_color, 0);
        lv_obj_align(ssid_label, LV_ALIGN_LEFT_MID, 4, 0);
        lv_label_set_long_mode(ssid_label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(ssid_label, 150);
        lv_obj_add_flag(ssid_label, LV_OBJ_FLAG_EVENT_BUBBLE);

        lv_obj_t *rssi_label = lv_label_create(row);
        lv_label_set_text_fmt(rssi_label, "%d dBm", ap->rssi);
        lv_obj_set_style_text_font(rssi_label, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(rssi_label, wifi_rssi_color(ap->rssi), 0);
        lv_obj_align(rssi_label, LV_ALIGN_RIGHT_MID, -4, 0);
        lv_obj_add_flag(rssi_label, LV_OBJ_FLAG_EVENT_BUBBLE);

        lv_obj_t *rssi_bar = lv_bar_create(row);
        lv_obj_set_size(rssi_bar, 60, 8);
        lv_obj_align(rssi_bar, LV_ALIGN_RIGHT_MID, -70, 0);
        lv_obj_set_style_bg_color(rssi_bar, lv_color_hex(0x1E2736), 0);
        lv_obj_set_style_bg_opa(rssi_bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(rssi_bar, 4, 0);
        lv_obj_set_style_bg_color(rssi_bar, wifi_rssi_color(ap->rssi), LV_PART_INDICATOR);
        lv_obj_set_style_radius(rssi_bar, 4, LV_PART_INDICATOR);
        lv_bar_set_value(rssi_bar, wifi_rssi_percentage(ap->rssi), LV_ANIM_OFF);
        lv_obj_add_flag(rssi_bar, LV_OBJ_FLAG_EVENT_BUBBLE);
    }
}

static void update_status(void)
{
    wifi_scan_result_t result;
    wifi_scan_get_result(&result);

    if (result.scanning) {
        lv_label_set_text(s_ap_scan_status, "Scanning");
        lv_obj_set_style_text_color(s_ap_scan_status, lv_color_hex(0xFFC107), 0);
        lv_obj_clear_flag(s_ap_scan_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_label_set_text(lv_obj_get_child(s_ap_scan_btn, 0), "...");
    } else {
        lv_label_set_text(s_ap_scan_status, "Ready");
        lv_obj_set_style_text_color(s_ap_scan_status, lv_color_hex(0x00E676), 0);
        lv_obj_add_flag(s_ap_scan_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_label_set_text(lv_obj_get_child(s_ap_scan_btn, 0), "Scan");
    }
}

/* ============ 定时器回调 ============ */
static void ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    update_status();
    update_ap_list_page();
    update_channel_page();
    update_remote_id_page();
    if (s_current_page == PAGE_AP_DETAIL) {
        update_detail_page();
    }
}

/* 延迟自动扫描 */
static void auto_scan_cb(lv_timer_t *t)
{
    (void)t;
    ESP_LOGI(TAG, "Auto-starting WiFi scan after delay");
    wifi_scan_start(NULL);
    if (s_delay_timer) {
        lv_timer_del(s_delay_timer);
        s_delay_timer = NULL;
    }
}

/* ============ 公共接口 ============ */
void wifi_scan_ui_create(lv_obj_t *parent)
{
    s_root = parent;

    /* 创建四个页面 */
    create_ap_list_page(parent);
    create_detail_page(parent);
    create_channel_page(parent);
    create_remote_id_page(parent);

    /* 默认显示 AP 列表页 */
    switch_page(PAGE_AP_LIST);

    /* UI 刷新定时器 500ms */
    s_update_timer = lv_timer_create(ui_timer_cb, 500, NULL);

    /* 延迟 2 秒后自动启动扫描 */
    ESP_LOGI(TAG, "WiFi scan UI created, will auto-scan in 2s");
    lv_label_set_text(s_ap_scan_status, "Wait");
    lv_obj_set_style_text_color(s_ap_scan_status, lv_color_hex(0xFFC107), 0);
    s_delay_timer = lv_timer_create(auto_scan_cb, 2000, NULL);
    lv_timer_set_repeat_count(s_delay_timer, 1);
}

void wifi_scan_ui_update(void)
{
    if (s_update_timer) {
        lv_timer_ready(s_update_timer);
    }
}

void wifi_scan_ui_destroy(void)
{
    if (s_update_timer) {
        lv_timer_del(s_update_timer);
        s_update_timer = NULL;
    }
    if (s_auto_refresh_timer) {
        lv_timer_del(s_auto_refresh_timer);
        s_auto_refresh_timer = NULL;
    }
    if (s_delay_timer) {
        lv_timer_del(s_delay_timer);
        s_delay_timer = NULL;
    }
    s_root = NULL;
    for (int i = 0; i < PAGE_COUNT; i++) s_pages[i] = NULL;
    s_ap_list_title = NULL;
    s_ap_count_label = NULL;
    s_ap_scan_btn = NULL;
    s_ap_auto_btn = NULL;
    s_ap_scan_status = NULL;
    s_ap_list = NULL;
    s_ap_ch_btn = NULL;
    s_ap_rid_btn = NULL;
    s_detail_back_btn = NULL;
    s_detail_label = NULL;
    s_ch_back_btn = NULL;
    s_ch_chart = NULL;
    s_ch_summary = NULL;
    s_rid_back_btn = NULL;
    s_rid_count = NULL;
    s_rid_list = NULL;
    s_auto_refresh = false;
    ESP_LOGI(TAG, "WiFi scan UI destroyed");
}
