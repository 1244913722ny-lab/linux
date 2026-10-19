/**
 * lv_fb_drv.c - LVGL Linux framebuffer (/dev/fb0) 显示驱动 + 可选 EVDEV 输入
 *
 * 适用：ARM Linux 开发板（或带 /dev/fb0 的 Linux PC），LVGL v8.3.11。
 *
 * 原理：
 *   - 打开 /dev/fb0，ioctl 取分辨率/像素格式，mmap 映射到用户空间；
 *   - LVGL 画完一块区域后调用 flush_cb，我们把绘制缓冲按行拷进 fb 显存，
 *     并按 fb 实际像素格式（16bit RGB565 或 32bit XRGB8888）做色彩转换；
 *   - （可选）用 EVDEV 读 /dev/input/eventX，喂给 LVGL 的 pointer 输入设备。
 */

#include "lv_fb_drv.h"

#include <fcntl.h>
#include <linux/fb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "lvgl.h"

/* ----------------------- framebuffer 全局状态 ----------------------- */
static int fb_fd = -1;
static void *fb_base = NULL; /* mmap 后的显存基地址 */
static uint32_t fb_xres = 0;
static uint32_t fb_yres = 0;
static uint32_t fb_bpp = 0; /* 每像素字节数: 2 或 4 */
static uint32_t fb_line_len =
    0; /* 每行字节数(含对齐填充), 来自 fix.line_length */

/* ----------------------- flush: LVGL 缓冲 -> 显存 ----------------------- */
static void fb_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
                        lv_color_t *color_p) {
  int32_t x1 = area->x1, y1 = area->y1, x2 = area->x2, y2 = area->y2;
  int32_t w = x2 - x1 + 1;
  int32_t h = y2 - y1 + 1;

  uint8_t *dst_row =
      (uint8_t *)fb_base + (size_t)y1 * fb_line_len + (size_t)x1 * fb_bpp;

  for (int32_t y = 0; y < h; y++) {
    if (fb_bpp == 4) {
      /* fb 是 32bit XRGB8888；LVGL 内部 32bit 内存布局为 B,G,R,A(小端)，转成
       * R,G,B,X */
      uint32_t *dst = (uint32_t *)(dst_row + (size_t)y * fb_line_len);
      for (int32_t x = 0; x < w; x++) {
        uint32_t c = color_p[x].full;
        uint8_t b = (uint8_t)(c & 0xFF);
        uint8_t g = (uint8_t)((c >> 8) & 0xFF);
        uint8_t r = (uint8_t)((c >> 16) & 0xFF);
        dst[x] = (0xFFu << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
      }
    } else {
      /* fb 是 16bit RGB565；LVGL 32bit -> RGB565 */
      uint16_t *dst = (uint16_t *)(dst_row + (size_t)y * fb_line_len);
      for (int32_t x = 0; x < w; x++) {
        uint32_t c = color_p[x].full;
        uint8_t b = (uint8_t)(c & 0xFF);
        uint8_t g = (uint8_t)((c >> 8) & 0xFF);
        uint8_t r = (uint8_t)((c >> 16) & 0xFF);
        dst[x] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
      }
    }
    color_p += w; /* 下一行（每行宽 = w 像素） */
  }

  lv_disp_flush_ready(drv);
}

static const char *fb_probe_device() {
  static char path[32];
  struct fb_var_screeninfo v;
  struct fb_fix_screeninfo f;
  for (int i = 0; i < 4; i++) {
    snprintf(path, sizeof(path), "/dev/fb%d", i);
    int fd = open(path, O_RDWR);
    if (fd < 0)
      continue;

    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) == 0 &&
        ioctl(fd, FBIOGET_FSCREENINFO, &f) == 0) {
      close(fd);
      return path;
    }
  }
  return NULL;
}

/* ----------------------- 初始化 framebuffer 显示 ----------------------- */
int lv_fb_init(void) {

  const char *path = fb_probe_device();
  if (!path) {
    fprintf(stderr, "[lv_fb] 未找到可用的 framebuffer 设备\n");
    return -1;
  }

  fb_fd = open(path, O_RDWR);
  if (fb_fd < 0) {
    perror("open framebuffer");
    return -1;
  }

  struct fb_var_screeninfo v;
  struct fb_fix_screeninfo f;
  if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &v) < 0) {
    perror("ioctl var");
    return -1;
  }
  if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &f) < 0) {
    perror("ioctl fix");
    return -1;
  }

  fb_xres = v.xres;
  fb_yres = v.yres;
  fb_bpp = v.bits_per_pixel / 8;
  fb_line_len = f.line_length ? f.line_length : fb_xres * fb_bpp;

  if (fb_bpp != 2 && fb_bpp != 4) {
    fprintf(stderr, "[lv_fb] 不支持的 fb 位深: %u bpp (仅支持 16/32)\n",
            v.bits_per_pixel);
    return -1;
  }

  fb_base =
      mmap(NULL, f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
  if (fb_base == MAP_FAILED) {
    perror("mmap");
    return -1;
  }

  printf("[lv_fb] %ux%u  bpp=%u  stride=%u\n", fb_xres, fb_yres,
         v.bits_per_pixel, fb_line_len);

  /* 部分刷新缓冲：约 40 行，省内存（LVGL 会分多次回调把整屏刷完） */
  static lv_color_t *draw_buf = NULL;
  uint32_t buf_px = fb_xres * 40;
  draw_buf = (lv_color_t *)malloc((size_t)buf_px * sizeof(lv_color_t));
  if (!draw_buf) {
    fprintf(stderr, "[lv_fb] 分配绘制缓冲失败\n");
    return -1;
  }

  static lv_disp_draw_buf_t disp_buf;
  lv_disp_draw_buf_init(&disp_buf, draw_buf, NULL, buf_px);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = fb_xres;
  disp_drv.ver_res = fb_yres;
  disp_drv.flush_cb = fb_flush_cb;
  disp_drv.draw_buf = &disp_buf;
  disp_drv.full_refresh = 0;
  lv_disp_drv_register(&disp_drv);

  return 0;
}

/* ====================== 可选：EVDEV 输入（触摸/鼠标） ======================
 */
#if defined(__linux__)
#include <linux/input.h>

/* 用户空间没有 test_bit，自己按 unsigned long 位宽判断 */
#define EV_BITS_PER_LONG (8 * (int)sizeof(unsigned long))
#define EV_BIT_ARR_SIZE(n) (((n) + EV_BITS_PER_LONG - 1) / EV_BITS_PER_LONG)
#define EV_BIT_ISSET(b, a)                                                     \
  (((a)[(b) / EV_BITS_PER_LONG] >> ((b) % EV_BITS_PER_LONG)) & 1UL)

static int evdev_fd = -1;
static int32_t evdev_x = 0;
static int32_t evdev_y = 0;
static bool evdev_pressed = false;
static bool evdev_is_rel =
    false; /* true=鼠标(相对坐标), false=触摸屏(绝对坐标) */

static void evdev_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  (void)drv;
  struct input_event e;

  /* 一次性把当前事件队列读完（非阻塞） */
  while (read(evdev_fd, &e, sizeof(e)) == (ssize_t)sizeof(e)) {
    if (e.type == EV_ABS) {
      /* 触摸屏：直接拿到绝对坐标 */
      if (e.code == ABS_X)
        evdev_x = e.value;
      else if (e.code == ABS_Y)
        evdev_y = e.value;
    } else if (e.type == EV_REL) {
      /* USB 鼠标：相对位移，累加到当前坐标 */
      evdev_is_rel = true;
      if (e.code == REL_X) {
        evdev_x += e.value;
        if (evdev_x < 0)
          evdev_x = 0;
        if (evdev_x >= (int32_t)fb_xres)
          evdev_x = (int32_t)fb_xres - 1;
      } else if (e.code == REL_Y) {
        evdev_y += e.value;
        if (evdev_y < 0)
          evdev_y = 0;
        if (evdev_y >= (int32_t)fb_yres)
          evdev_y = (int32_t)fb_yres - 1;
      }
    } else if (e.type == EV_KEY) {
      if (e.code == BTN_TOUCH || e.code == BTN_LEFT)
        evdev_pressed = (e.value != 0);
    }
  }

  data->point.x = (lv_coord_t)evdev_x;
  data->point.y = (lv_coord_t)evdev_y;
  data->state =
      evdev_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* 探测设备支持的事件类型：1=找到可用的 pointer 设备, 0=不是 */
static int evdev_probe_device(const char *path) {
  int fd = open(path, O_RDONLY | O_NONBLOCK);
  if (fd < 0)
    return 0;

  char name[64] = {0};
  ioctl(fd, EVIOCGNAME(sizeof(name)), name);

  unsigned long evbit[EV_BIT_ARR_SIZE(EV_MAX)] = {0};
  if (ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit) < 0) {
    close(fd);
    return 0;
  }

  bool has_abs = EV_BIT_ISSET(EV_ABS, evbit);
  bool has_rel = EV_BIT_ISSET(EV_REL, evbit);
  bool has_key = EV_BIT_ISSET(EV_KEY, evbit);

  /* 打印设备名 + 能力，方便调试（区分鼠标/触摸/键盘） */
  printf("[lv_fb] probe %s (\"%s\"): ABS=%d REL=%d KEY=%d\n", path, name,
         has_abs, has_rel, has_key);

  close(fd);

  /* pointer 设备：触摸屏(ABS+KEY) 或 鼠标(REL+KEY) */
  if ((has_abs && has_key) || (has_rel && has_key))
    return 1;
  return 0;
}

/* 自动寻找第一个可用的 /dev/input/eventX */
static const char *evdev_find_first(void) {
  static char path[32];
  for (int i = 0; i < 32; i++) {
    snprintf(path, sizeof(path), "/dev/input/event%d", i);
    if (evdev_probe_device(path))
      return path;
  }
  return NULL;
}

struct _lv_indev_t *lv_evdev_init(const char *dev_path) {
  const char *path = dev_path;

  /* 如果调用者没指定，或指定的打不开，自动探测 */
  if (!path || !path[0] || access(path, R_OK) != 0) {
    const char *auto_path = evdev_find_first();
    if (auto_path) {
      if (path && path[0])
        printf("[lv_fb] %s 不可用，自动切换到 %s\n", path, auto_path);
      path = auto_path;
    } else if (!path || !path[0]) {
      fprintf(stderr, "[lv_fb] 未找到可用的输入设备（不影响显示）\n");
      return NULL;
    }
    /* 否则继续尝试用户指定的路径，让它自己失败报 open 错 */
  }

  evdev_fd = open(path, O_RDONLY | O_NONBLOCK);
  if (evdev_fd < 0) {
    perror("open evdev");
    fprintf(stderr, "[lv_fb] 跳过输入设备 %s（没有触摸/鼠标也能正常显示）\n",
            path);
    return NULL;
  }

  /* 判断设备类型，初始化坐标到屏幕中心（鼠标相对坐标时） */
  unsigned long evbit[EV_BIT_ARR_SIZE(EV_MAX)] = {0};
  if (ioctl(evdev_fd, EVIOCGBIT(0, sizeof(evbit)), evbit) >= 0) {
    evdev_is_rel = EV_BIT_ISSET(EV_REL, evbit);
  }
  if (evdev_is_rel) {
    evdev_x = (int32_t)fb_xres / 2;
    evdev_y = (int32_t)fb_yres / 2;
  }

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = evdev_read_cb;
  lv_indev_t *indev = lv_indev_drv_register(&indev_drv);

  printf("[lv_fb] EVDEV 输入已启用: %s (%s)\n", path,
         evdev_is_rel ? "mouse/rel" : "touch/abs");
  return indev;
}

/* ====================== 键盘输入（回车=点击，最可靠） ======================
 */
static int kbd_fd = -1;
static uint32_t kbd_key = 0;
static bool kbd_pressed = false;

static void kbd_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  (void)drv;
  struct input_event e;

  while (read(kbd_fd, &e, sizeof(e)) == (ssize_t)sizeof(e)) {
    if (e.type == EV_KEY) {
      uint32_t lv_key = 0;
      switch (e.code) {
      case KEY_ENTER:
      case KEY_KPENTER:
        lv_key = LV_KEY_ENTER;
        break;
      case KEY_UP:
        lv_key = LV_KEY_UP;
        break;
      case KEY_DOWN:
        lv_key = LV_KEY_DOWN;
        break;
      case KEY_LEFT:
        lv_key = LV_KEY_LEFT;
        break;
      case KEY_RIGHT:
        lv_key = LV_KEY_RIGHT;
        break;
      case KEY_ESC:
        lv_key = LV_KEY_ESC;
        break;
      default:
        break;
      }
      if (lv_key) {
        if (e.value) {
          kbd_key = lv_key;
          kbd_pressed = true;
        } else if (kbd_key == lv_key) {
          kbd_pressed = false; /* 释放时不清零 key，LVGL 据此触发 CLICKED */
        }
      }
    }
  }

  data->key = kbd_key;
  data->state = kbd_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* 探测键盘：支持 EV_KEY 且包含 KEY_ENTER，且不是 pointer 设备 */
static int kbd_probe_device(const char *path) {
  int fd = open(path, O_RDONLY | O_NONBLOCK);
  if (fd < 0)
    return 0;

  char name[64] = {0};
  ioctl(fd, EVIOCGNAME(sizeof(name)), name);

  unsigned long evbit[EV_BIT_ARR_SIZE(EV_MAX)] = {0};
  if (ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit) < 0) {
    close(fd);
    return 0;
  }
  if (!EV_BIT_ISSET(EV_KEY, evbit)) {
    printf("[lv_fb] kbdprobe %s (\"%s\"): 无 EV_KEY，跳过\n", path, name);
    close(fd);
    return 0;
  }

  unsigned long keybit[EV_BIT_ARR_SIZE(KEY_MAX)] = {0};
  if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit) < 0) {
    close(fd);
    return 0;
  }

  bool has_enter = EV_BIT_ISSET(KEY_ENTER, keybit);
  bool has_rel = EV_BIT_ISSET(EV_REL, evbit);
  bool has_abs = EV_BIT_ISSET(EV_ABS, evbit);
  printf("[lv_fb] kbdprobe %s (\"%s\"): ENTER=%d REL=%d ABS=%d\n", path, name,
         has_enter, has_rel, has_abs);
  close(fd);

  /* 键盘 = 有 ENTER，且不是 pointer(鼠标/触摸) 设备 */
  if (has_enter && !(has_rel || has_abs))
    return 1;
  return 0;
}

static const char *kbd_find_first(void) {
  static char path[32];
  for (int i = 0; i < 32; i++) {
    snprintf(path, sizeof(path), "/dev/input/event%d", i);
    if (kbd_probe_device(path))
      return path;
  }
  return NULL;
}

struct _lv_indev_t *lv_keyboard_init(const char *dev_path) {
  const char *path = dev_path;
  if (!path || !path[0] || access(path, R_OK) != 0) {
    const char *auto_path = kbd_find_first();
    if (auto_path) {
      if (path && path[0])
        printf("[lv_fb] %s 不可用，自动切换到 %s\n", path, auto_path);
      path = auto_path;
    } else if (!path || !path[0]) {
      fprintf(stderr, "[lv_fb] 未找到可用的键盘设备（不影响显示）\n");
      return NULL;
    }
  }

  kbd_fd = open(path, O_RDONLY | O_NONBLOCK);
  if (kbd_fd < 0) {
    perror("open kbd");
    fprintf(stderr, "[lv_fb] 跳过键盘设备 %s（不影响显示）\n", path);
    return NULL;
  }

  static lv_indev_drv_t kbd_drv;
  lv_indev_drv_init(&kbd_drv);
  kbd_drv.type = LV_INDEV_TYPE_KEYPAD;
  kbd_drv.read_cb = kbd_read_cb;
  lv_indev_t *indev = lv_indev_drv_register(&kbd_drv);

  printf("[lv_fb] KEYPAD 输入已启用: %s\n", path);
  return indev;
}
#else
int lv_evdev_init(const char *dev_path) {
  (void)dev_path;
  return -1;
}
lv_indev_t *lv_keyboard_init(const char *dev_path) {
  (void)dev_path;
  return NULL;
}
#endif
