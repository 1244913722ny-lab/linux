#ifndef FLIP_DEMO_H
#define FLIP_DEMO_H

/* FLIP 动效 demo：用 First-Last-Invert-Play 算法实现卡片重排的平滑位移动画。
 * 接口与 network_demo 一致：open 在 lv_scr_act() 上构建 UI，close 清理当前屏。 */
void flip_demo_open(void);
void flip_demo_close(void);

#endif /* FLIP_DEMO_H */
