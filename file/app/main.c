/**
 * main.c - LVGL framebuffer demo launcher (v8.3.11)
 *
 * 桌面启动器：主界面是一张深色桌面，上面排列 5 个 LVGL 内置 demo 的应用磁贴
 * （Widgets / Benchmark / Keypad / Music / Stress）。点击磁贴即加载对应 demo。
 * 右上角常驻两个按钮：
 *   🏠 回到桌面 —— demo
 * 在后台继续运行，下次点同一磁贴直接切回（保留运行状态）； ✕ 退出     ——
 * 彻底关闭并销毁 demo 屏，释放内存，下次从磁贴进是全新启动。
 *
 * 运行：lv_init() -> lv_fb_init() 注册显示 ->
 * lv_evdev_init()/lv_keyboard_init() 注册输入 -> build_desktop() -> 主循环
 * lv_tick_inc() + lv_timer_handler()。
 */

#include "lv_demos.h"
#include "lv_fb_drv.h"
#include "lv_freetype.h" /* 运行时加载外部 TTF 中文字体（LV_USE_FREETYPE=1） */
#include "lvgl.h"
#include "camera_demo.h"
#include "network_demo.h"
#include "flip_demo.h"

#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

/* 运行时 FreeType 字体对象的全局指针：init_cjk_font() 就绪后指向 TTF 字体；
 * 在此之前为 NULL，lv_conf.h 的 LV_FONT_DEFAULT 宏据此回退到 Montserrat。
 * 用 void* 是为了能在 lv_conf.h（早于 lv_font_t 定义被解析）里声明该符号。 */
void *g_cjk_font = NULL;

/* ----------------------- demo 注册表 ----------------------- */
typedef struct {
  const char *name;    /* 磁贴下方显示的应用名 */
  const char *symbol;  /* 磁贴中央显示的图标符号 */
  uint32_t color_hex;  /* 磁贴背景色：24bit RGB 值（如 0x3B82F6），运行时
                          lv_color_hex() 转换 */
  void (*open)(void);  /* 启动该 demo */
  void (*close)(void); /* 卸载该 demo */
} demo_entry_t;

/* 注：lv_color_make() 是函数调用，不能用于全局静态初始化（C 要求常量表达式），
 * 故这里只存 24bit RGB 数值，颜色在 build_desktop() 运行时再转换。 */
static const demo_entry_t g_demos[] = {
    {"Widgets", LV_SYMBOL_SETTINGS, 0x3B82F6, lv_demo_widgets,
     lv_demo_widgets_close},
    {"Benchmark", LV_SYMBOL_REFRESH, 0x10B981, lv_demo_benchmark,
     lv_demo_benchmark_close},
    {"Keypad", LV_SYMBOL_KEYBOARD, 0xF59E0B, lv_demo_keypad_encoder,
     lv_demo_keypad_encoder_close},
    {"Music", LV_SYMBOL_AUDIO, 0x8B5CF6, lv_demo_music, lv_demo_music_close},
    {"Stress", LV_SYMBOL_WARNING, 0xEF4444, lv_demo_stress,
     lv_demo_stress_close},
    {"Network", LV_SYMBOL_WIFI, 0x0EA5E9, network_demo_open,
     network_demo_close},
    {"FLIP", LV_SYMBOL_SHUFFLE, 0x22C55E, flip_demo_open, flip_demo_close},
    {"Camera", LV_SYMBOL_VIDEO, 0xEC4899, camera_demo_open,
     camera_demo_close},
};
#define DEMO_COUNT ((int)(sizeof(g_demos) / sizeof(g_demos[0])))

/* ----------------------- 状态 ----------------------- */
static lv_obj_t *g_desktop_scr = NULL; /* 桌面屏 */
static lv_obj_t *g_demo_scr = NULL;    /* 当前 demo 所在屏（保留复用 / 后台） */
static const demo_entry_t *g_active =
    NULL; /* 已加载的 demo（前台或后台均非 NULL；已退出则 NULL） */
static bool g_on_desktop = true; /* 当前活动屏是否为桌面（false=正在看 demo） */
static lv_indev_t *g_kbd = NULL; /* 键盘输入设备 */
static lv_group_t *g_desktop_group = NULL; /* 桌面键盘导航 group */

/* ----------------------- 前向声明 ----------------------- */
static void go_desktop(void);
static void exit_demo(void);
static void launch_demo(const demo_entry_t *d);

/* 磁贴点击事件：启动对应 demo */
static void tile_event_cb(lv_event_t *e) {
  if (e->code != LV_EVENT_CLICKED)
    return;
  const demo_entry_t *d = (const demo_entry_t *)lv_event_get_user_data(e);
  launch_demo(d);
}

/* 🏠 按钮事件：回到桌面（demo 后台保留） */
static void home_btn_event_cb(lv_event_t *e) {
  if (e->code != LV_EVENT_CLICKED)
    return;
  go_desktop();
}

/* ✕ 按钮事件：退出并销毁当前 demo（彻底关掉 app） */
static void exit_btn_event_cb(lv_event_t *e) {
  if (e->code != LV_EVENT_CLICKED)
    return;
  exit_demo();
}

/* 构建桌面：深色背景 + 标题 + 磁贴网格 */
static void build_desktop(void) {
  lv_obj_t *scr = g_desktop_scr;

  /* 深色背景 */
  lv_obj_set_style_bg_color(scr, lv_color_make(0x12, 0x18, 0x2B), 0);

  /* 标题 */
  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "LVGL Demo Launcher");
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

  /* 磁贴容器（flex 自动换行网格） */
  lv_obj_t *cont = lv_obj_create(scr);
  lv_obj_set_size(cont, 720, 540);
  lv_obj_align(cont, LV_ALIGN_CENTER, 0, 20);
  lv_obj_set_style_bg_color(cont, lv_color_make(0x12, 0x18, 0x2B), 0);
  lv_obj_set_style_border_width(cont, 0, 0);
  lv_obj_set_style_pad_left(cont, 30, 0);
  lv_obj_set_style_pad_right(cont, 30, 0);
  lv_obj_set_style_pad_top(cont, 30, 0);
  lv_obj_set_style_pad_bottom(cont, 30, 0);
  lv_obj_set_style_pad_column(cont, 24, 0); /* 横向间距（flex gap） */
  lv_obj_set_style_pad_row(cont, 24, 0);    /* 纵向间距（flex gap） */
  lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  /* 磁贴超过一屏（如 7 个）时允许纵向滚动，避免末行被裁切 */
  lv_obj_set_scroll_dir(cont, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_AUTO);

  /* 键盘导航 group：把磁贴加进去，方向键 + 回车即可在桌面上选择/启动 */
  g_desktop_group = lv_group_create();

  for (int i = 0; i < DEMO_COUNT; i++) {
    lv_obj_t *tile = lv_obj_create(cont);
    lv_obj_set_size(tile, 200, 200);
    lv_obj_set_style_bg_color(tile, lv_color_hex(g_demos[i].color_hex), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_90, 0);
    lv_obj_set_style_radius(tile, 18, 0);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(tile, 14, 0);

    lv_obj_t *icon = lv_label_create(tile);
    lv_label_set_text(icon, g_demos[i].symbol);
    lv_obj_set_style_text_color(icon, lv_color_white(), 0);
    lv_obj_set_style_text_font(icon, LV_FONT_DEFAULT, 0);

    lv_obj_t *label = lv_label_create(tile);
    lv_label_set_text(label, g_demos[i].name);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);

    lv_obj_add_event_cb(tile, tile_event_cb, LV_EVENT_ALL, (void *)&g_demos[i]);
    lv_group_add_obj(g_desktop_group, tile); /* 加入键盘导航 */
  }
}

/* 启动某个 demo。
 * 语义：
 *   - 点同一个“已在后台/前台加载”的 demo -> 直接切回（保留运行状态，不重建）；
 *   - 点另一个 demo -> 先彻底退出旧的，再全新启动新的；
 *   - 尚未启动过 -> 全新启动。
 * 设计要点：demo 屏对象只创建一次（g_demo_scr），之后复用；绝不直接
 * lv_obj_del 活动屏（LVGL 对“删除活动屏”会打 WARN 并把 disp->act_scr 置 NULL，
 * 是已知险路）。 */
static void launch_demo(const demo_entry_t *d) {
  fprintf(stderr, "[launch] enter %s\n", d->name);

  /* 同一 demo 已加载：直接切回（后台保留，不重建） */
  if (g_active == d) {
    fprintf(stderr, "[launch] resume %s (background)\n", d->name);
    lv_scr_load(g_demo_scr);
    g_on_desktop = false;
    fprintf(stderr, "[launch] done\n");
    return;
  }

  /* 有其它 demo 在后台/前台：先彻底退出旧的，再建新的 */
  if (g_active != NULL) {
    fprintf(stderr, "[launch] exit prev %s first\n", g_active->name);
    exit_demo(); /* exit_demo 会切回桌面并清空 g_active / g_demo_scr */
  }

  /* 全新启动 */
  if (g_demo_scr == NULL) {
    g_demo_scr = lv_obj_create(NULL);
    fprintf(stderr, "[launch] created demo screen\n");
  }
  lv_scr_load(g_demo_scr); /* 切到 demo 屏（此时它是活动屏，open 会建在上面） */
  g_on_desktop = false;

  fprintf(stderr, "[launch] open %s\n", d->name);
  d->open(); /* demo 在 lv_scr_act()（即 g_demo_scr）上构建 */
  g_active = d;
  fprintf(stderr, "[launch] done %s\n", d->name);
}

/* 回到桌面：demo 在后台继续运行（不销毁），下次点同一磁贴直接切回。
 * 与 exit_demo() 的区别：这里只切屏、保留 demo 屏内容；exit_demo() 才真正销毁。
 */
static void go_desktop(void) {
  fprintf(stderr, "[desktop] enter\n");

  /* 仅在“正在看 demo”时才需要切回桌面；切屏不 close，保留 demo 屏内容（后台）。
   */
  if (!g_on_desktop) {
    fprintf(stderr, "[desktop] scr_load(desktop)\n");
    lv_scr_load(g_desktop_scr);
    g_on_desktop = true;

    /* 还原桌面键盘导航 group（部分 demo 运行时会改写键盘 indev 的 group） */
    fprintf(stderr, "[desktop] restore group\n");
    if (g_kbd && g_desktop_group) {
      lv_indev_set_group(g_kbd, g_desktop_group);
    }
  }
  fprintf(stderr, "[desktop] exit\n");
}

/* 彻底退出 demo（关掉 app）：清理内容并销毁 demo
 * 屏，释放内存，下次从磁贴进是全新启动。 无论当前在桌面后台还是 demo
 * 前台，都先把 demo 屏设为活动屏再做 close()， 否则 close() 里的
 * lv_obj_clean(lv_scr_act()) 会误清桌面。 */
static void exit_demo(void) {
  if (g_active == NULL)
    return;

  fprintf(stderr, "[exit] enter %s\n", g_active->name);

  /* 确保 demo 屏是活动屏，close() 才能正确清理它（此时若在桌面后台也先切回来）
   */
  if (g_on_desktop) {
    fprintf(stderr, "[exit] load demo scr first\n");
    lv_scr_load(g_demo_scr);
    g_on_desktop = false;
  }

  fprintf(stderr, "[exit] close %s\n", g_active->name);
  g_active->close();

  /* 先切回桌面（demo 屏不再是活动屏），再做 lv_obj_del，
   * 避免“删除活动屏”造成的 disp->act_scr=NULL 险路。 */
  fprintf(stderr, "[exit] back to desktop\n");
  lv_scr_load(g_desktop_scr);
  g_on_desktop = true;

  fprintf(stderr, "[exit] del scr\n");
  lv_obj_del(g_demo_scr);
  g_demo_scr = NULL;
  g_active = NULL;

  /* 还原桌面键盘导航 group */
  fprintf(stderr, "[exit] restore group\n");
  if (g_kbd && g_desktop_group) {
    lv_indev_set_group(g_kbd, g_desktop_group);
  }
  fprintf(stderr, "[exit] done\n");
}

/* 在系统层（layer_sys）上放两个常驻按钮（不随屏幕切换消失）：
 *   🏠 回到桌面 —— demo 后台保留，下次点同一磁贴直接切回；
 *   ✕ 退出     —— 彻底销毁 demo 屏，释放内存。
 * 注意：不能放在 lv_layer_top()！因为部分 demo（stress / keypad_encoder）的
 * close() 会调用 lv_obj_clean(lv_layer_top()) 清理自身临时弹窗，会把放在该层的
 * 按钮一并删除；而删除动作发生在按钮自身的 CLICKED 事件回调里，导致
 * LVGL 事件分发访问已释放对象 -> Segmentation fault。
 * lv_layer_sys() 是系统层（鼠标光标也在这里），没有任何 demo 的 close()
 * 会清理它， 因此按钮放在这里既常驻又安全。 */
static void create_top_buttons(void) {
  /* 🏠 回到桌面（demo 后台保留） */
  lv_obj_t *home = lv_btn_create(lv_layer_sys());
  lv_obj_set_size(home, 56, 56);
  lv_obj_align(home, LV_ALIGN_TOP_RIGHT, -16, 16);
  lv_obj_set_style_radius(home, 28, 0);
  lv_obj_set_style_bg_color(home, lv_color_make(0x25, 0x32, 0x4A), 0);
  lv_obj_set_style_border_width(home, 0, 0);

  lv_obj_t *home_lab = lv_label_create(home);
  lv_label_set_text(home_lab, LV_SYMBOL_HOME);
  lv_obj_set_style_text_color(home_lab, lv_color_white(), 0);

  lv_obj_add_event_cb(home, home_btn_event_cb, LV_EVENT_ALL, NULL);

  /* ✕ 退出（彻底销毁 demo 屏） */
  lv_obj_t *exit_btn = lv_btn_create(lv_layer_sys());
  lv_obj_set_size(exit_btn, 56, 56);
  lv_obj_align(exit_btn, LV_ALIGN_TOP_RIGHT, -80, 16);
  lv_obj_set_style_radius(exit_btn, 28, 0);
  lv_obj_set_style_bg_color(exit_btn, lv_color_make(0xEF, 0x44, 0x44), 0);
  lv_obj_set_style_border_width(exit_btn, 0, 0);

  lv_obj_t *exit_lab = lv_label_create(exit_btn);
  lv_label_set_text(exit_lab, LV_SYMBOL_CLOSE);
  lv_obj_set_style_text_color(exit_lab, lv_color_white(), 0);

  lv_obj_add_event_cb(exit_btn, exit_btn_event_cb, LV_EVENT_ALL, NULL);
}

/* 运行时加载外部 TTF 中文(简体)字体，作为全局默认字体的主字体。
 * v8.3.11 没有 lv_font_add（那是 v9 API）；正确做法是让 FreeType 字体当主字体，
 * 其 fallback 指向 Montserrat（覆盖 TTF 缺的字形，如 ①②✕）。
 * 前置：ARM 版 libfreetype/libbrotli/libpng/libz 已静态链；目标机
 * /mnt/sdcard/zw.ttf 存在。 */
static void init_cjk_font(void) {
  if (!lv_freetype_init(1, 1, 256 * 1024)) {
    fprintf(stderr, "[cjk] FreeType 初始化失败，中文将显示为空白\n");
    return;
  }

  /* static => 零初始化，保证 info.mem == NULL（= 文件加载模式，而非内存字体）*/
  static lv_ft_info_t info;
  info.name = "/mnt/sdcard/zw.ttf"; /* 目标文件系统里的真实路径(ASCII 名) */
  info.weight = 16;                 /* 字号 px（与 Montserrat14 接近，可微调）*/
  info.style = FT_FONT_STYLE_NORMAL;
  /* info.mem 已由 static 零初始化为 NULL，无需再显式赋值 */

  if (!lv_ft_font_init(&info) || info.font == NULL) {
    fprintf(stderr,
            "[cjk] TTF 加载失败：确认 /mnt/sdcard/cjk.ttf 存在且可读\n");
    return;
  }

  /* FreeType 字体做主字体（含中文+拉丁），缺字回退 Montserrat；
   * 再把这个字体对象挂到全局默认字体（lv_conf.h 的 LV_FONT_DEFAULT 宏）。 */
  info.font->fallback = &lv_font_montserrat_14;
  g_cjk_font = (void *)info.font;
  fprintf(stderr, "[cjk] 中文字体就绪\n");
}

int main(void) {
  /* 1) 初始化 LVGL 与 framebuffer 显示 */
  lv_init();

  /* 0) 加载外部中文 TTF 字体并挂到默认字体做回退：
   *     拉丁字符仍走 Montserrat，缺失的汉字自动回退到 TTF 栅格化；
   *     失败仅告警，不影响英文 UI。必须在建界面之前调用。
   *     依赖：ARM 版 libfreetype（-lfreetype，静态链）+ 目标机
   * /mnt/sdcard/cjk.ttf。 */
  init_cjk_font();

  if (lv_fb_init() != 0) {
    fprintf(stderr, "framebuffer 初始化失败，退出。\n");
    return 1;
  }

  /* layer_top / layer_sys 默认是可点击的且铺满全屏，会拦截下层屏幕的点击；
   * 清掉 CLICKABLE
   * 后，空点击会穿透到下层屏幕，而其上的子对象（常驻按钮、鼠标光标） 自身仍带
   * CLICKABLE，照常可达。注意：常驻按钮放在 layer_sys（见
   * create_top_buttons）， 因为 stress/keypad_encoder 的 close() 会
   * lv_obj_clean(lv_layer_top())，不能把常驻 UI 放那层。 */
  lv_obj_clear_flag(lv_layer_top(), LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(lv_layer_sys(), LV_OBJ_FLAG_CLICKABLE);

  /* 2) 可选输入：鼠标（EVDEV 自动探测 event0~event31） */
  lv_indev_t *mouse = lv_evdev_init(NULL);

  /* 3) 可选输入：键盘（PS/2，回车=激活聚焦对象，方向键=切换焦点） */
  g_kbd = lv_keyboard_init(NULL);

  /* 4) 建桌面屏并加载，再填充磁贴 */
  g_desktop_scr = lv_obj_create(NULL);
  lv_scr_load(g_desktop_scr);
  build_desktop();

  /* 桌面键盘导航：把 group 绑定到键盘 indev（group 已在 build_desktop 建好） */
  if (g_kbd && g_desktop_group) {
    lv_indev_set_group(g_kbd, g_desktop_group);
  }

  /* 5) 鼠标可见光标（红点）。挂到顶层 layer，切屏后依然可见 */
  if (mouse) {
    lv_obj_t *cursor = lv_obj_create(lv_layer_top());
    lv_obj_set_size(cursor, 12, 12);
    lv_obj_set_style_bg_color(cursor, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_border_width(cursor, 0, 0);
    lv_indev_set_cursor(mouse, cursor);
  }

  /* 6) 常驻按钮：🏠 回到桌面（后台保留）/ ✕ 退出（彻底销毁） */
  create_top_buttons();

  /* 7) 主循环：喂 tick + 处理定时器/刷新 */
  struct timespec prev;
  clock_gettime(CLOCK_MONOTONIC, &prev);

  while (1) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    long ms = (now.tv_sec - prev.tv_sec) * 1000L +
              (now.tv_nsec - prev.tv_nsec) / 1000000L;
    if (ms > 0) {
      lv_tick_inc((uint32_t)ms);
      prev = now;
    }

    lv_timer_handler();
    usleep(5000); /* ~5ms 节拍 */
  }

  return 0;
}
