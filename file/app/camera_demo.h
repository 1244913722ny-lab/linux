#ifndef CAMERA_DEMO_H
#define CAMERA_DEMO_H

#include "lvgl.h"

/* 相机 demo 的对外接口（与 LVGL 内置 demos 的 open/close 签名一致，
 * 方便 main.c 的注册表直接挂接）。 */
void camera_demo_open(void);
void camera_demo_close(void);

#endif /* CAMERA_DEMO_H */
