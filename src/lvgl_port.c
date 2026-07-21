#include "lvgl_port.h"
#include "config/lcd_config.h"
#include "drivers/st77916_lcd.h"
#include "drivers/cst820_touch.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/spi_master.h"
#include <string.h>

static const char *TAG = "lvgl_port";

static uint8_t *s_disp_buf1 = NULL;
static uint8_t *s_disp_buf2 = NULL;
static lv_display_t *s_disp = NULL;
static lv_indev_t *s_indev = NULL;
static esp_timer_handle_t s_lvgl_timer = NULL;

static uint32_t s_flush_count = 0;

static void lvgl_flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map)
{
    (void)display;
    if (s_flush_count < 3) {
        ESP_LOGI(TAG, "flush #%d: area=(%d,%d)-(%d,%d) pixels=%u",
                 s_flush_count, area->x1, area->y1, area->x2, area->y2,
                 (area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1));
    }
    s_flush_count++;

    st77916_set_window(area->x1, area->y1, area->x2, area->y2);

    uint32_t pixel_count = (area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1);

    for (uint32_t i = 0; i < pixel_count; i++) {
        uint8_t tmp = px_map[i * 2];
        px_map[i * 2] = px_map[i * 2 + 1];
        px_map[i * 2 + 1] = tmp;
    }

    st77916_write_color(0x2C, px_map, pixel_count * 2);

    lv_display_flush_ready(display);
}

static void lvgl_touch_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    static lv_point_t last_point = {0};
    static uint32_t log_counter = 0;

    cst820_point_t point;
    if (cst820_read_touch(&point)) {
        /* 每 10 次触摸事件记录一次，避免日志刷屏 */
        if (log_counter % 10 == 0) {
            ESP_LOGI(TAG, "touch pressed: (%d, %d)", point.x, point.y);
        }
        log_counter++;
        last_point.x = point.x;
        last_point.y = point.y;
        data->point = last_point;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->point = last_point;
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void lvgl_tick_callback(void *arg)
{
    (void)arg;
    lv_tick_inc(1);
}

lv_display_t *lvgl_port_init(void)
{
    ESP_LOGI(TAG, "Initializing LVGL port");

    lv_init();

    size_t buf_size = LCD_H_RES * LVGL_BUF_LINES * 2;

    s_disp_buf1 = heap_caps_malloc(buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (s_disp_buf1 == NULL) {
        ESP_LOGE(TAG, "Failed to allocate display buffer 1");
        return NULL;
    }
    memset(s_disp_buf1, 0, buf_size);

    s_disp_buf2 = heap_caps_malloc(buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (s_disp_buf2 == NULL) {
        ESP_LOGW(TAG, "Failed to allocate display buffer 2, using single buffer");
    } else {
        memset(s_disp_buf2, 0, buf_size);
    }

    s_disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    if (s_disp == NULL) {
        ESP_LOGE(TAG, "Failed to create LVGL display");
        free(s_disp_buf1);
        free(s_disp_buf2);
        return NULL;
    }

    lv_display_set_flush_cb(s_disp, lvgl_flush_cb);
    lv_display_set_buffers(s_disp, s_disp_buf1, s_disp_buf2, buf_size,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);

    s_indev = lv_indev_create();
    if (s_indev) {
        lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_indev, lvgl_touch_cb);
        lv_indev_set_display(s_indev, s_disp);
    }

    cst820_init();
    ESP_LOGI(TAG, "CST820 touch initialized, indev=%p", (void*)s_indev);

    const esp_timer_create_args_t timer_args = {
        .callback = &lvgl_tick_callback,
        .name = "lvgl_tick"
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_lvgl_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_lvgl_timer, 1000));

    ESP_LOGI(TAG, "LVGL port initialized (%dx%d, buf_lines=%d)",
             LCD_H_RES, LCD_V_RES, LVGL_BUF_LINES);
    return s_disp;
}

bool lvgl_port_get_touch_point(lv_point_t *point)
{
    if (point == NULL) return false;

    cst820_point_t p;
    if (cst820_read_touch(&p)) {
        point->x = p.x;
        point->y = p.y;
        return true;
    }
    return false;
}