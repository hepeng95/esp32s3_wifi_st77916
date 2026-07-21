#ifndef WIFI_SCAN_UI_H
#define WIFI_SCAN_UI_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void wifi_scan_ui_create(lv_obj_t *parent);
void wifi_scan_ui_update(void);
void wifi_scan_ui_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_SCAN_UI_H */
