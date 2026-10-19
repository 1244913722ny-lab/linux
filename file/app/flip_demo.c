/**
 * flip_demo.c - FLIP 动效 demo（First-Last-Invert-Play）
 *
 * 演示 FLIP 重排动画：一组彩色卡片，点击「洗牌」随机打乱顺序，
 * 每张卡片从「当前所在位置」平滑滑动到「新位置」，而不是瞬间跳变。
 *
 * FLIP 算法的四步：
 *   First   记录元素当前的真实几何位置（这里用 lv_obj_get_x/y 取实际坐标）
 *   Last    改变顺序，计算元素的目标位置
 *   Invert  （DOM/布局场景需要把元素先瞬间挪回 First 再动画；本 demo 用绝对
 *            坐标管理卡片，没有布局回流，直接「从 First 动画到 Last」即等价）
 *   Play    用动画把元素从 First 移到 Last
 *
 * 实现要点：
 *   - 卡片用绝对坐标（x/y）摆放，容器不启用 flex，避免布局覆盖我们的动画坐标；
 *   - 每次洗牌读取卡片「当前真实坐标」作为 First，因此即使上一轮动画还没播完
 *     就再次洗牌，也不会跳变（取代旧动画，从当前位置继续）；
 *   - 本 demo 用纯 x/y 位移驱动动画，不叠加 transform 缩放：LVGL v8 里任何带
 *     transform_zoom(≠256)/angle 的对象会被归入 LV_LAYER_TYPE_TRANSFORM 离屏图层
 *     渲染路径，在「逐帧移动 + fbdev 局部刷新」组合下中间帧不被提交到屏幕，看起来
 *     像瞬移；纯位移走普通对象的局部增量刷新，动画平滑可见。
 */

#include "flip_demo.h"
#include "lvgl.h"

#include <stdint.h>
#include <stdlib.h>
#include <time.h>

/* ----------------------- 布局常量 ----------------------- */
#define FLIP_COLS 4
#define FLIP_ROWS 2
#define FLIP_N (FLIP_COLS * FLIP_ROWS) /* 8 张卡片 */

#define CARD_W 110
#define CARD_H 90
#define CARD_GAP 24

/* 棋盘容器尺寸：列*(宽+间距)-间距，行同理 */
#define FLIP_BOARD_W (FLIP_COLS * (CARD_W + CARD_GAP) - CARD_GAP) /* 512 */
#define FLIP_BOARD_H (FLIP_ROWS * (CARD_H + CARD_GAP) - CARD_GAP) /* 204 */

/* 槽位 s 的坐标（容器内绝对坐标，pad=0 故从 0 起算） */
#define SLOT_X(s) ((lv_coord_t)((s) % FLIP_COLS) * (CARD_W + CARD_GAP))
#define SLOT_Y(s) ((lv_coord_t)((s) / FLIP_COLS) * (CARD_H + CARD_GAP))

/* ----------------------- 静态资源 ----------------------- */
static const uint32_t g_palette[FLIP_N] = {
    0xEF4444, 0xF59E0B, 0x10B981, 0x3B82F6,
    0x8B5CF6, 0xEC4899, 0x14B8A6, 0xF97316};
static const char *g_labels[FLIP_N] = {"A", "B", "C", "D",
                                       "E", "F", "G", "H"};

/* ----------------------- 运行状态 ----------------------- */
static lv_obj_t *g_card[FLIP_N];        /* 每张卡片的 UI 对象 */
static int g_slot_of_card[FLIP_N];      /* card -> 当前所在槽位 */
static int32_t g_old_x[FLIP_N], g_old_y[FLIP_N]; /* FLIP: First 坐标 */
static int32_t g_new_x[FLIP_N], g_new_y[FLIP_N]; /* FLIP: Last 坐标 */

static bool g_auto = false;             /* 是否循环自动洗牌 */
static lv_timer_t *g_auto_timer = NULL; /* 自动洗牌定时器 */
static lv_obj_t *g_auto_label = NULL;   /* 「自动/停止」按钮上的文字 */

/* ----------------------- FLIP 动画 exec 回调 ----------------------- */
/* v: 0..1024 进度。按线性插值计算 x/y（纯位移，LVGL 最稳的动画路径）。 */
static void flip_exec(void *var, int32_t v) {
  lv_obj_t *card = (lv_obj_t *)var;
  int idx = (int)(intptr_t)lv_obj_get_user_data(card);

  /* user_data 被踩的防御：idx 越界说明 user_data 不可信，直接返回 */
  if (idx < 0 || idx >= FLIP_N)
    return;

  int32_t x = g_old_x[idx] + (((int32_t)g_new_x[idx] - g_old_x[idx]) * v) / 1024;
  int32_t y = g_old_y[idx] + (((int32_t)g_new_y[idx] - g_old_y[idx]) * v) / 1024;
  lv_obj_set_x(card, x);
  lv_obj_set_y(card, y);
}

/* ----------------------- 洗牌（核心 FLIP 逻辑） ----------------------- */
static void flip_shuffle(void) {
  int slots[FLIP_N];
  for (int i = 0; i < FLIP_N; i++)
    slots[i] = i;

  /* Fisher-Yates 随机排列：slots[card] = 该卡片的新槽位 */
  for (int i = FLIP_N - 1; i > 0; i--) {
    int j = rand() % (i + 1);
    int tmp = slots[i];
    slots[i] = slots[j];
    slots[j] = tmp;
  }

  for (int c = 0; c < FLIP_N; c++) {
    int new_slot = slots[c];

    /* First：读取卡片当前真实坐标（即使上轮动画未完成也安全） */
    g_old_x[c] = lv_obj_get_x(g_card[c]);
    g_old_y[c] = lv_obj_get_y(g_card[c]);

    /* Last：目标坐标 + 记录新顺序 */
    g_new_x[c] = SLOT_X(new_slot);
    g_new_y[c] = SLOT_Y(new_slot);
    g_slot_of_card[c] = new_slot;

    /* Play：从 First 动画到 Last（lv_anim_start 会自动顶掉同一对象的旧动画） */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, g_card[c]);
    lv_anim_set_exec_cb(&a, flip_exec);
    lv_anim_set_values(&a, 0, 1024);
    lv_anim_set_time(&a, 450);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
  }
}

/* ----------------------- 按钮回调 ----------------------- */
static void flip_shuffle_cb(lv_event_t *e) {
  if (e->code != LV_EVENT_CLICKED)
    return;
  flip_shuffle();
}

static void flip_auto_toggle_cb(lv_event_t *e) {
  if (e->code != LV_EVENT_CLICKED)
    return;
  g_auto = !g_auto;
  lv_label_set_text(g_auto_label, g_auto ? "停止" : "自动");
}

/* 自动洗牌定时器：开启时定期触发一次洗牌 */
static void flip_auto_cb(lv_timer_t *t) {
  (void)t;
  if (g_auto)
    flip_shuffle();
}

/* ----------------------- open / close ----------------------- */
void flip_demo_open(void) {
  lv_obj_t *scr = lv_scr_act();
  srand((unsigned)time(NULL));

  /* 标题 + 说明 */
  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "FLIP 动效 Demo（First-Last-Invert-Play）");
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

  lv_obj_t *hint = lv_label_create(scr);
  lv_label_set_text(hint, "点击「洗牌」触发重排动画；「自动」循环洗牌");
  lv_obj_set_style_text_color(hint, lv_color_make(0xAA, 0xBB, 0xCC), 0);
  lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 44);

  /* 棋盘容器：默认 flow=NONE，子对象用绝对坐标摆放，便于 FLIP 自由移动 */
  lv_obj_t *board = lv_obj_create(scr);
  lv_obj_set_size(board, FLIP_BOARD_W, FLIP_BOARD_H);
  lv_obj_set_style_bg_color(board, lv_color_make(0x0E, 0x14, 0x24), 0);
  lv_obj_set_style_bg_opa(board, LV_OPA_COVER, 0); /* 之前漏了 bg_opa，容器透明不可见 */
  lv_obj_set_style_border_width(board, 2, 0);
  lv_obj_set_style_border_color(board, lv_color_make(0x33, 0x44, 0x66), 0);
  lv_obj_set_style_pad_all(board, 0, 0);
  lv_obj_set_style_radius(board, 12, 0);
  lv_obj_center(board);

  /* 初始顺序：card c 放在槽位 c */
  for (int c = 0; c < FLIP_N; c++) {
    g_slot_of_card[c] = c;

    lv_obj_t *card = lv_obj_create(board);
    lv_obj_set_size(card, CARD_W, CARD_H);
    lv_obj_set_style_bg_color(card, lv_color_hex(g_palette[c]), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_x(card, SLOT_X(c));
    lv_obj_set_y(card, SLOT_Y(c));
    lv_obj_set_user_data(card, (void *)(intptr_t)c);

    lv_obj_t *lab = lv_label_create(card);
    lv_label_set_text(lab, g_labels[c]);
    lv_obj_set_style_text_color(lab, lv_color_white(), 0);
    lv_obj_set_style_text_font(lab, LV_FONT_DEFAULT, 0);
    lv_obj_center(lab);

    g_card[c] = card;
  }

  /* 控制按钮：洗牌 / 自动 */
  lv_obj_t *shuf = lv_btn_create(scr);
  lv_obj_set_size(shuf, 140, 48);
  lv_obj_align(shuf, LV_ALIGN_BOTTOM_LEFT, 24, -24);
  lv_obj_set_style_radius(shuf, 10, 0);
  lv_obj_t *shuf_lab = lv_label_create(shuf);
  lv_label_set_text(shuf_lab, "洗牌");
  lv_obj_set_style_text_font(shuf_lab, LV_FONT_DEFAULT, 0);
  lv_obj_add_event_cb(shuf, flip_shuffle_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *auto_btn = lv_btn_create(scr);
  lv_obj_set_size(auto_btn, 140, 48);
  lv_obj_align(auto_btn, LV_ALIGN_BOTTOM_LEFT, 180, -24);
  lv_obj_set_style_radius(auto_btn, 10, 0);
  g_auto_label = lv_label_create(auto_btn);
  lv_label_set_text(g_auto_label, "自动");
  lv_obj_set_style_text_font(g_auto_label, LV_FONT_DEFAULT, 0);
  lv_obj_add_event_cb(auto_btn, flip_auto_toggle_cb, LV_EVENT_CLICKED, NULL);

  /* 初始化自动洗牌状态 + 定时器（关闭时删除） */
  g_auto = false;
  g_auto_timer = lv_timer_create(flip_auto_cb, 1200, NULL);
}

void flip_demo_close(void) {
  if (g_auto_timer) {
    lv_timer_del(g_auto_timer);
    g_auto_timer = NULL;
  }
  g_auto = false;
  /* open 在 lv_scr_act() 上构建；close 只清当前屏（与 network_demo 一致） */
  lv_obj_clean(lv_scr_act());
}
