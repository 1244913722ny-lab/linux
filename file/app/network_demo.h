#ifndef NETWORK_DEMO_H
#define NETWORK_DEMO_H

#include "lvgl.h"

/* 对外 TCP 连通性测试 demo 的对外接口（与 LVGL 内置 demos 的 open/close 签名一致，
 * 方便 main.c 的注册表直接挂接）。 */
void network_demo_open(void);
void network_demo_close(void);

#endif /* NETWORK_DEMO_H */
