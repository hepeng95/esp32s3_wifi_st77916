#ifndef LVGL_PORT_H
#define LVGL_PORT_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_display_t *lvgl_port_init(void);
bool lvgl_port_get_touch_point(lv_point_t *point);

#ifdef __cplusplus
}
#endif

#endif /* LVGL_PORT_H */