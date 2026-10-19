/**
 * lv_conf.h - 最小可用配置 (LVGL v8.3.11)
 *
 * 本文件通过 Makefile 里的 -DLV_CONF_INCLUDE_SIMPLE 被 LVGL 自动包含
 * （lv_conf_internal.h 内部用 __has_include("lv_conf.h") 找到它）。
 * 这里只覆盖关键项，其余未列出的宏由 lv_conf_internal.h 的默认值补齐。
 */
#if !defined(LV_CONF_H)
#define LV_CONF_H

/*==================== COLOR SETTINGS ====================*/
#define LV_COLOR_DEPTH                                                         \
  32 /* LVGL 内部色彩深度；驱动再把 32bit 转成 fb 实际格式 */
#define LV_COLOR_16_SWAP                                                       \
  0 /* 16bit RGB565 是否字节交换（ARM 同端序，不需要）          \
     */
#define LV_COLOR_SCREEN_TRANSP 0

/*==================== MEMORY SETTINGS ====================*/
#define LV_MEM_SIZE                                                                     \
  (32 * 1024 * 1024)    /* LVGL 内部堆大小。Widgets/Music/Benchmark 等           \
                        demo 对象极多，1MB 会在运行/返回时耗尽导致 NULL \
                        解引用 -> 段错误；QEMU 里 Linux 内存充裕，给 32MB。 */
#define LV_MEM_CUSTOM 0 /* 用 LVGL 自带内存池 */
#define LV_MEM_ADDR 0

/*==================== HAL SETTINGS ====================*/
#define LV_TICK_CUSTOM                                                         \
  0 /* 关闭内置 tick（我们在 main 里手动调用 lv_tick_inc） */
#define LV_DISP_DEF_REFR_PERIOD (1000 / 120)  /* 屏幕刷新周期(ms) */
#define LV_INDEV_DEF_READ_PERIOD (1000 / 480) /* 输入设备轮询周期(ms) */

/*==================== MISCELLANEOUS ====================*/
#define LV_DPI_DEF 130
#define LV_USE_LOG 0
#define LV_USE_USER_DATA 1
#define LV_USE_ASSERT_NULL 0
#define LV_USE_ASSERT_MEM 0
#define LV_USE_ASSERT_OBJ 0
#define LV_USE_OS 0

/*==================== DRAW / GPU（framebuffer 纯软件渲染，全部关闭）
 * ====================*/
#define LV_USE_DRAW_MASKS 1
#define LV_USE_GPU_STM32_DMA2D 0
#define LV_USE_GPU_NXP_PXP 0
#define LV_USE_GPU_NXP_VGLITE 0
#define LV_USE_GPU_SWM341_DMA2D 0
#define LV_USE_GPU_ARM2D 0
#define LV_USE_VG_LITE 0

/*==================== THEME ====================*/
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_TRANSITION_TIME 80

/*==================== FONT ====================*/
/* 默认字体 */
#define LV_FONT_MONTSERRAT_14 1
/* 默认字体：FreeType 运行时 TTF 字体就绪前回退 Montserrat；就绪后由 g_cjk_font 接管
 * （主字体为 TTF 中文，其 fallback 指向 Montserrat 覆盖缺字，如 ①②✕）。
 * g_cjk_font 在 main.c 中定义；用 void* 声明以便本文件（早于 lv_font_t 定义被解析）引用。 */
extern void * g_cjk_font;
#define LV_FONT_DEFAULT ((lv_font_t *)(g_cjk_font ? g_cjk_font : (void *)&lv_font_montserrat_14))
/* 内置 demos 用到的字体（不开会链接报未定义符号） */
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_22 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_32 1

/*==================== SJPG（Tiny JPEG Decompressor，相机 demo 解码 JPEG 帧）
 * ====================*/
/* 打开后 src/extra/libs/sjpg/ 下的 tjpgd.c 才会参与编译，tjpgd.h 的
 * jd_prepare/jd_decomp 声明才可见（整个目录被 #if LV_USE_SJPG 包裹）。
 * 附带效果：lv_sjpg.c 向 LVGL 注册 sjpg 图片解码器（无副作用）。 */
#define LV_USE_SJPG 1

/*==================== KEYBOARD（屏幕虚拟键盘）
 * ====================*/
/* QEMU 里没有实体键盘输入设备，相机 demo 的服务器 IP / 端口只能靠屏幕
 * 虚拟键盘输入（lv_keyboard 在 src/extra/widgets/keyboard/，整体被
 * #if LV_USE_KEYBOARD 包裹，不打开则 lv_keyboard_create 等接口不存在）。
 * lv_keyboard 基于 btnmatrix 实现，所以 BTNMATRIX 一并显式打开。
 * 数字模式（LV_KEYBOARD_MODE_NUMBER）的按键含 0-9、点、退格、OK、
 * 左右移动光标，正好覆盖 IP 与端口输入。 */
#define LV_USE_KEYBOARD 1
#define LV_USE_BTNMATRIX 1

/*==================== FREETYPE（运行时加载外部 TTF 中文(简体)字体)
 * ====================*/
#define LV_USE_FREETYPE 1
#if LV_USE_FREETYPE
    /* 字体位图/轮廓缓存字节数；必须 >= 0 才会进入缓存分支
     * （lv_freetype.c 用 #if LV_FREETYPE_CACHE_SIZE >= 0 选择 cache 实现）。
     * 这里给 256KB，全字 CJK 按需栅格化，内存占用随用随清。 */
    #define LV_FREETYPE_CACHE_SIZE (256 * 1024)
    /* 用 SBit 缓存（直接缓存小位图，最省事）；关掉则走 ImageCache（矢量轮廓再栅格）。 */
    #define LV_FREETYPE_SBIT_CACHE 1
#endif

/*==================== WIDGETS（demo 用到的） ====================*/
#define LV_USE_BTN 1
#define LV_USE_LABEL 1

/*==================== DEMO / EXAMPLES（桌面启动器要加载内置 demo）
 * ====================*/
#define LV_BUILD_EXAMPLES 0
#define LV_USE_DEMO_WIDGETS 1
#define LV_USE_DEMO_KEYPAD_AND_ENCODER 1
#define LV_USE_DEMO_BENCHMARK 1
#define LV_USE_DEMO_STRESS 1
#define LV_USE_DEMO_MUSIC 1

/* 调试浮层 */
/*1: Show CPU usage and FPS count*/
#define LV_USE_PERF_MONITOR 1
#if LV_USE_PERF_MONITOR
#define LV_USE_PERF_MONITOR_POS LV_ALIGN_BOTTOM_RIGHT
#endif

/*1: Show the used memory and the memory fragmentation
 * Requires LV_MEM_CUSTOM = 0*/
#define LV_USE_MEM_MONITOR 1
#if LV_USE_MEM_MONITOR
#define LV_USE_MEM_MONITOR_POS LV_ALIGN_BOTTOM_LEFT
#endif

#endif /* LV_CONF_H */
