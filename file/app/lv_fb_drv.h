/**
 * lv_fb_drv.h - LVGL Linux framebuffer 显示驱动 + 可选 EVDEV 输入的对外接口
 *
 * 配合 LVGL v8.3.11 使用。
 */
#ifndef LV_FB_DRV_H
#define LV_FB_DRV_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化 /dev/fb0 显示驱动：
 *   - 探测分辨率与像素格式，mmap 显存；
 *   - 分配 LVGL 绘制缓冲并注册显示设备。
 * 返回 0 成功，-1 失败。
 */
int lv_fb_init(void);

/**
 * 可选：初始化一个 EVDEV 输入设备（触摸屏 / 鼠标），节点如 "/dev/input/event0"。
 * 传 NULL 则自动探测第一个可用的 pointer 设备。
 * 打开失败会跳过（不影响显示）。
 * 返回注册的 LVGL 输入设备指针，失败返回 NULL。
 */
struct _lv_indev_t;
struct _lv_indev_t *lv_evdev_init(const char *dev_path);

/**
 * 可选：初始化键盘输入（keypad 模式），回车键映射到 LV_KEY_ENTER，
 * 配合 lv_group 使用即可用键盘"点击"按钮。传 NULL 自动探测键盘设备。
 * 返回注册的 LVGL 输入设备指针，失败返回 NULL。
 */
struct _lv_indev_t *lv_keyboard_init(const char *dev_path);

#ifdef __cplusplus
}
#endif

#endif /* LV_FB_DRV_H */
