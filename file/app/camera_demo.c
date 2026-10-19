/**
 * camera_demo.c - 相机画面接收 demo（TCP + JPEG 解码 + LVGL 显示）
 *
 * 功能：
 *   作为 TCP 客户端连接「服务器」（手机 App / PC 上的推流服务），
 *   持续接收服务器发来的 JPEG 帧，用 tjpgd 解码成 ARGB32 位图，
 *   贴到 LVGL 的 lv_img 上显示，实现“看手机上传的相机画面”。
 *
 * 帧协议（服务器端按此格式发送，循环往复）：
 *   +--------+------------------+
 *   | 4 字节  | N 字节            |
 *   | 大端长度 | JPEG 数据         |
 *   +--------+------------------+
 *   长度 N 不包含自身 4 字节；N 必须满足 0 < N <= CAM_JPEG_MAX。
 *
 * 界面流程（main.c 的启动器语义：open() 在 demo 屏上构建，close() 清理）：
 *   1) 打开 demo -> 先显示「服务器设置」面板：
 *        服务器 IP（单行输入框，限制只接受数字和点）
 *        端口（单行输入框，默认 8888，只接受数字）
 *        [连接] 按钮
 *      此时并不会建连；点「连接」才在后台线程里发起 TCP 连接。
 *   2) 连接成功 -> 自动切到「相机视图」：
 *        居中的图像区（黑底，收到第一帧后显示画面）
 *        左上角：服务器地址 + 当前帧分辨率
 *        左下角：实时 fps
 *        右下角：[重新设置] 按钮（断开，回到设置面板）
 *   3) 连接失败/中途断开 -> 自动回到设置面板，状态标签显示原因。
 *
 * 线程模型（与 network_demo 一致的标准嵌入式 GUI 网络模式）：
 *   - 后台线程：getaddrinfo -> socket -> 非阻塞 connect(3s 超时)
 *     -> 收流循环 -> tjpgd 解码 -> 写入双缓冲之一的“后台帧槽”；
 *   - 主线程：LVGL 定时器（33ms）轮询共享状态，做所有 lv_* 调用
 *     （LVGL 不是线程安全的，后台线程绝不直接调 lv_*）；
 *   - 共享数据全部由 g_mtx 保护；停止标志 g_stop 用 __atomic。
 *
 * 双缓冲策略：
 *   后台线程每解码完一帧，g_write_seq++（发布 g_frame_seq）；
 *   主线程只认 g_frame_seq 对应的那块缓冲（lv_img_set_src 指向它）。
 *   后台若发现主线程已落后超过 1 帧未消费，则 usleep 限速等主线程，
 *   保证“后台写的缓冲 != 主线程正在显示的缓冲”，不会撕裂/踩烂画面。
 *
 * 画面尺寸：
 *   tjpgd 只能缩（1/1~1/8）不能放，所以手机推的 320x240 若直接显示只占
 *   屏幕一小块。做法是每帧缓冲分两段（显示区 dw*dh + 源区 w*h）：
 *   tjpgd 先 1:1 输出到源区，再由 scale_up_nearest() 整图等比放大到显示区，
 *   保持宽高比不变形，倍率上限 CAM_MAX_UPSCALE。
 *   注意：绝不能在 tjpgd 的输出回调里同时做坐标映射——它是按 MCU 分块
 *   回调的，rect->left 是块内起点而非整幅图列号，映射一错就是花屏。
 *   推流分辨率越高细节越好但 QEMU 软解越慢，建议手机端 480x360；
 *   想要 640x480 的真实细节就把 CAM_MAX_UPSCALE 设 1（只缩小不放大）。
 *
 * 依赖：
 *   - lv_conf.h 必须定义 LV_USE_SJPG 1（tjpgd 解码器整体被该宏包裹）；
 *   - 编译需要 -I../lvgl/src/extra/libs/sjpg（tjpgd.h）；
 *   - QEMU 已接网卡、rootfs 有 udhcpc（连本机地址 127.0.0.1 永远可用）。
 */

#include "camera_demo.h"

#include "lvgl.h"
#include "tjpgd.h" /* TJpgDec：内存 JPEG -> RGB888，支持 1/1~1/8 缩放解码 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* ----------------------- 可调参数 ----------------------- */
#define CAM_JPEG_MAX (512 * 1024) /* 单帧 JPEG 上限 512KB（防恶意/错帧撑爆内存） */
#define CAM_MAX_W 1920            /* 解码前允许的最大原图宽 */
#define CAM_MAX_H 1080            /* 解码前允许的最大原图高 */
#define CAM_WORK_BUF (8 * 1024)   /* tjpgd 工作池（官方推荐 4096，取 8KB 余量） */
#define CAM_CONNECT_TIMEOUT_MS 3000 /* connect  select 超时 */
#define CAM_REFRESH_MS 33            /* 主线程 UI 刷新周期（~30fps） */
#define CAM_DEFAULT_PORT "8888"      /* 端口输入框默认值 */
/* 画面放大倍数上限（面积倍数）：手机推 320x240 时，屏上撑到显示区约 1.6 倍；
 * 设 1 = 关闭放大（保持原尺寸显示）。别设太大，否则小图被拉成糊图还白吃 CPU。 */
#ifndef CAM_MAX_UPSCALE
#define CAM_MAX_UPSCALE 4
#endif

#define CAM_LOG(fmt, ...) fprintf(stderr, "[cam] " fmt, ##__VA_ARGS__)

/* ----------------------- 状态机 ----------------------- */
typedef enum {
  CAM_ST_IDLE = 0,    /* 刚打开/已重置，等待用户输入 */
  CAM_ST_CONNECTING,  /* 后台线程正在 connect */
  CAM_ST_STREAMING,   /* 已连接，正在收流解码 */
  CAM_ST_ERROR        /* 连接失败/断开（原因见 g_status） */
} cam_state_t;

/* ----------------------- 运行状态 ----------------------- */

/* 后台线程与主线程之间的共享状态，全部由 g_mtx 保护 */
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static cam_state_t g_state = CAM_ST_IDLE; /* 当前状态机状态 */
static char g_status[192];                /* 状态文字（ERROR 时是失败原因） */
static uint32_t g_frame_seq;              /* 后台已解码发布的帧序号（从 0 起） */
static uint32_t g_shown_seq;              /* 主线程已显示的帧序号 */
static uint32_t g_write_seq;              /* 后台最近写入的帧序号 */
static uint32_t g_frames_total;           /* 累计成功解码帧数 */
static uint32_t g_drop_total;             /* 累计丢弃帧数（解码失败/坏帧） */
static float g_fps;                       /* 主线程每秒刷新的帧率 */
static uint16_t g_img_w, g_img_h;         /* 当前帧解码后尺寸 */

/* 双缓冲：g_rgb[0]/g_rgb[1] 是两块 ARGB32 帧缓冲（lv_color_t* 即 uint32_t*，
 * LV_COLOR_DEPTH=32 时 lv_img 的 LV_IMG_CF_TRUE_COLOR 就是 4 字节/像素）。
 * g_dsc[i] 是与 g_rgb[i] 配对的 lv_img_dsc_t（LVGL 只认 dsc）。 */
static uint32_t *g_rgb[2] = {NULL, NULL};
static uint32_t g_rgb_cap[2] = {0, 0}; /* 各缓冲当前容量（字节） */
static lv_img_dsc_t g_dsc[2];          /* static => 零初始化 */

/* 后台线程专用：收帧缓冲（整帧 JPEG）与 tjpgd 工作池 */
static uint8_t *g_jpeg = NULL;
static uint8_t g_work[CAM_WORK_BUF];

/* 解码输出回调要用的上下文（仅后台线程访问，无需锁） */
static uint32_t *g_dec_dst = NULL; /* 当前解码目标缓冲（本帧的“源区”） */
static uint16_t g_dec_w, g_dec_h;  /* tjpgd 1:1 输出的宽/高（含 scale 缩小） */

/* 显示区限制（open() 时由主线程算好，后台线程只读） */
static lv_coord_t g_disp_max_w = 720;
static lv_coord_t g_disp_max_h = 400;

/* 上一次刷新时看到的状态（供 refresh 检测“状态切换”，open 时复位） */
static cam_state_t g_last_st = CAM_ST_IDLE;

/* 连接与线程 */
static int g_sock = -1;          /* 当前连接 socket（-1=无） */
static volatile int g_stop = 0;  /* 停止标志（__atomic 访问） */
static pthread_t g_thread;       /* 后台线程句柄 */
static int g_thread_running = 0; /* 线程是否在跑（需 join） */
static char g_conn_ip[64];       /* 连接参数回显（信息栏用） */
static char g_conn_port[16];

/* UI 对象（全部只在主线程访问） */
static lv_obj_t *g_setup = NULL;    /* 设置面板 */
static lv_obj_t *g_ip_ta = NULL;    /* IP 输入框 */
static lv_obj_t *g_port_ta = NULL;  /* 端口输入框 */
static lv_obj_t *g_kb = NULL;       /* 屏幕虚拟键盘（QEMU 无实体键盘，只能用它输入） */
static lv_obj_t *g_status_lab = NULL; /* 设置面板上的状态标签 */
static lv_obj_t *g_view = NULL;     /* 相机视图 */
static lv_obj_t *g_img = NULL;      /* 图像对象 */
static lv_obj_t *g_info_lab = NULL; /* 左上：地址+分辨率 */
static lv_obj_t *g_fps_lab = NULL;  /* 右上：fps */
static lv_obj_t *g_wait_lab = NULL; /* 中央：等待首帧提示 */
static lv_timer_t *g_timer = NULL;  /* 主线程刷新定时器 */

/* ----------------------- 小工具 ----------------------- */

/* 置错误状态（后台线程调用；带 mutex） */
static void cam_set_error(const char *fmt, ...) {
  char tmp[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);

  pthread_mutex_lock(&g_mtx);
  g_state = CAM_ST_ERROR;
  snprintf(g_status, sizeof(g_status), "%s", tmp);
  pthread_mutex_unlock(&g_mtx);
}

/* ----------------------- tjpgd 回调（后台线程） ----------------------- */

/* tjpgd 的输入上下文：把整帧 JPEG 当作“只读流”顺序喂给解码器 */
typedef struct {
  const uint8_t *data; /* JPEG 数据首地址 */
  size_t size;         /* 总长度 */
  size_t pos;           /* 当前读到的偏移 */
} cam_io_t;

/* tjpgd 的“从内存顺序读 JPEG 字节”回调：dev 即 jd_prepare 传进来的 io 上下文 */
static size_t tj_input_cb(JDEC *jd, uint8_t *buff, size_t ndata) {
  cam_io_t *io = (cam_io_t *)jd->device;
  if (io == NULL)
    return 0;

  size_t left = io->size - io->pos;
  size_t take = (ndata < left) ? ndata : left;
  if (take == 0)
    return 0; /* 0 = 流结束（tjpgd 据此判定 JPEG 数据是否完整） */

  if (buff != NULL)
    memcpy(buff, io->data + io->pos, take);
  io->pos += take;
  return take;
}

/* tjpgd 的“输出一块矩形区域”回调：
 * data 里是 JD_FORMAT=0 的 RGB888 像素（3 字节/像素，行优先）；
 * 逐像素转成 32bit ARGB（0xFF RRGGBB）写进全局目标缓冲 g_dec_dst。
 * 注意：小端下 lv_color_t.full = alpha<<24 | red<<16 | green<<8 | blue。
 *
 * 这里只做 1:1 写入（写入“源区”），不做放大：tjpgd 是按 MCU 分块回调的，
 * rect->left 可能是块内起点而非整幅图列号，若在回调里同时做坐标映射，
 * 分块边界一错就是花屏。放大统一交给后面的 scale_up_nearest() 整图做。 */
static int tj_output_cb(JDEC *jd, void *data, JRECT *rect) {
  (void)jd; /* device 走全局 g_dec_dst，无需 jd */
  const int row_w = rect->right - rect->left + 1;  /* 本块像素数 */
  const int row_bytes = row_w * 3;                 /* 本块字节数（RGB888） */
  const uint8_t *src = (const uint8_t *)data;
  uint32_t *dst = g_dec_dst;

  if (dst == NULL)
    return 0; /* 返回 0 让 tjpgd 中止解码 */

  for (int y = rect->top; y <= rect->bottom; y++) {
    uint32_t *line = dst + (size_t)y * g_dec_w + rect->left;
    for (int x = 0; x < row_w; x++) {
      const uint32_t r = src[x * 3 + 0];
      const uint32_t g = src[x * 3 + 1];
      const uint32_t b = src[x * 3 + 2];
      line[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
    src += row_bytes;
  }
  return 1; /* 继续 */
}

/* ----------------------- 等比最近邻放大 ----------------------- */

/* 把 sw x sh 的 ARGB32 图放大成 dw x dh 写进 dst（src/dst 不得重叠）。
 * 水平方向用“累加器”推进源像素下标，避免逐像素除法（QEMU 软解很慢）。
 * 每目标行算一次源行号（整除），开销可忽略。 */
static void scale_up_nearest(const uint32_t *src, uint32_t *dst,
                             uint16_t sw, uint16_t sh, uint16_t dw,
                             uint16_t dh) {
  for (uint32_t dy = 0; dy < dh; dy++) {
    const uint32_t sy = (uint32_t)dy * sh / dh; /* 本目标行取哪一行源像素 */
    const uint32_t *srow = src + (size_t)sy * sw;
    uint32_t *drow = dst + (size_t)dy * dw;
    int acc = 0; /* 源像素步进累加器 */
    uint32_t sx = 0;
    for (uint32_t dx = 0; dx < dw; dx++) {
      drow[dx] = srow[sx];
      acc += (int)sw;
      while (acc >= (int)dw) {
        acc -= (int)dw;
        if (++sx >= sw)
          sx = sw - 1; /* 下标钳位，杜绝越界 */
      }
    }
  }
}

/* ----------------------- JPEG 解码（后台线程） ----------------------- */

/* 把 len 字节的 JPEG 解码进 g_rgb[idx]，成功后填好 g_dsc[idx]。
 * 返回 0 成功，-1 失败（调用方按“丢帧”处理，不断流）。
 * out_w/out_h 输出本次解码后的实际尺寸（调用方持锁发布时使用）。 */
static int decode_frame(const uint8_t *jpeg, size_t len, int idx,
                        uint16_t *out_w, uint16_t *out_h) {
  cam_io_t io;
  io.data = jpeg;
  io.size = len;
  io.pos = 0;

  /* 1) 解析 JPEG 头，拿原始宽高 */
  JDEC jd;
  JRESULT rc = jd_prepare(&jd, tj_input_cb, g_work, sizeof(g_work), &io);
  if (rc != JDR_OK) {
    CAM_LOG("jd_prepare 失败 rc=%d（坏帧/非 JPEG？）\n", (int)rc);
    return -1;
  }
  if (jd.width == 0 || jd.height == 0 || jd.width > CAM_MAX_W ||
      jd.height > CAM_MAX_H) {
    CAM_LOG("帧尺寸异常 %ux%u\n", (unsigned)jd.width, (unsigned)jd.height);
    return -1;
  }

  /* 2) 选 tjpgd 的缩放档位（0=1:1, 1=1:2, 2=1:4, 3=1:8）：
   *    让解码输出直接落进显示区，省 CPU/内存，也避免 LVGL transform
   *    图层在 fbdev 局部刷新下的不提交问题（flip_demo 踩过的坑）。 */
  int scale = 0;
  while (scale < 3 &&
         (((lv_coord_t)(jd.width >> scale) > g_disp_max_w) ||
          ((lv_coord_t)(jd.height >> scale) > g_disp_max_h))) {
    scale++;
  }
  const uint16_t w = (uint16_t)(jd.width >> scale);
  const uint16_t h = (uint16_t)(jd.height >> scale);

  /* 2b) 比显示区小就等比放大到尽量占满（tjpgd 只能缩不能放，放大在
   *     输出回调里做）。保持宽高比，不拉伸变形；倍率上限 CAM_MAX_UPSCALE。 */
  uint32_t dw = w, dh = h;
  if ((lv_coord_t)dw < g_disp_max_w || (lv_coord_t)dh < g_disp_max_h) {
    /* 用整数比较决定受哪边限制，避免浮点：w/h vs maxw/maxh */
    if ((int64_t)dw * g_disp_max_h <= (int64_t)dh * g_disp_max_w) {
      dh = (uint32_t)g_disp_max_h; /* 高度是瓶颈：按高度撑满 */
      dw = (uint32_t)dw * g_disp_max_h / h;
    } else {
      dw = (uint32_t)g_disp_max_w; /* 宽度是瓶颈：按宽度撑满 */
      dh = (uint32_t)dh * g_disp_max_w / w;
    }
    /* 放大倍率封顶（按面积倍数粗判，避免小图被撑成糊掉的巨幅） */
    while (dw * dh > (uint32_t)w * h * CAM_MAX_UPSCALE) {
      dw = dw - dw / 16; /* 每轮缩 1/16，渐近收敛，不会一步砍过头 */
      dh = dh - dh / 16;
      if (dw < w || dh < h) { /* 至少回到原始尺寸 */
        dw = w;
        dh = h;
        break;
      }
    }
  }
  /* 缓冲一次分配两段（都按最大可能尺寸算，跨帧复用、零重复分配）：
   *   [0, dw*dh)                显示区 —— LVGL 的 dsc 指向这里（放大后的画面）
   *   [dw*dh, dw*dh + w*h)     源区   —— tjpgd 1:1 输出到这里，再放大进显示区
   * 分开放是为了放大时读写不重叠（同一块内不同偏移也不会互相踩）。 */
  const size_t dst_pix = (size_t)dw * (size_t)dh;
  const size_t src_pix = (size_t)w * (size_t)h;
  const size_t need = (dst_pix + src_pix) * 4u; /* ARGB32 */

  /* 3) 按需扩容目标帧缓冲（跨帧复用，尺寸不变则零分配） */
  if (g_rgb_cap[idx] < need) {
    uint32_t *p = (uint32_t *)realloc(g_rgb[idx], need);
    if (p == NULL) {
      CAM_LOG("帧缓冲 realloc %u 字节失败\n", (unsigned)need);
      return -1;
    }
    g_rgb[idx] = p;
    g_rgb_cap[idx] = (uint32_t)need;
  }
  uint32_t *dst_img = g_rgb[idx];             /* 显示区 */
  uint32_t *src_img = g_rgb[idx] + dst_pix;   /* 源区 */

  /* 4) 先让 tjpgd 1:1 输出到源区（回调只做 RGB888->ARGB 转换，不碰坐标映射） */
  g_dec_dst = src_img;
  g_dec_w = w;
  g_dec_h = h;
  rc = jd_decomp(&jd, tj_output_cb, (uint8_t)scale);
  g_dec_dst = NULL;
  if (rc != JDR_OK) {
    CAM_LOG("jd_decomp 失败 rc=%d\n", (int)rc);
    return -1;
  }

  /* 5) 再整图等比放大：源区 -> 显示区。dw/dh 与源一致时无需搬运，
   *    直接把源区首地址给 dsc（省掉每帧 1MB 的 memcpy）。 */
  uint32_t *show_img;
  if (dw != w || dh != h) {
    scale_up_nearest(src_img, dst_img, w, h, (uint16_t)dw, (uint16_t)dh);
    show_img = dst_img;
  } else {
    show_img = src_img;
  }

  /* 6) 填 lv_img_dsc_t：TRUE_COLOR + 32bit 色深 = 4 字节/像素。
   *    w/h 必须是放大后的尺寸，且 data 必须指向对应的图像区。 */
  g_dsc[idx].header.cf = LV_IMG_CF_TRUE_COLOR;
  g_dsc[idx].header.always_zero = 0;
  g_dsc[idx].header.w = (uint16_t)dw;
  g_dsc[idx].header.h = (uint16_t)dh;
  g_dsc[idx].data_size = (uint32_t)(dst_pix * 4u);
  g_dsc[idx].data = (const uint8_t *)show_img;

  /* 7) 记录尺寸（由调用方持锁发布，避免与主线程竞态） */
  *out_w = (uint16_t)dw;
  *out_h = (uint16_t)dh;
  return 0;
}

/* ----------------------- 后台收流线程 ----------------------- */

/* 线程参数：连接目标（open 时 UI 文本的拷贝，避免指向 UI 对象生命周期） */
typedef struct {
  char ip[64];
  char port[16];
} cam_conn_t;

/* 完整接收 n 字节（阻塞 socket 下循环 recv）。
 * 返回 0 成功；-1 对端关闭/出错；-2 收到停止标志。 */
static int recv_all(int sock, uint8_t *buf, size_t n) {
  size_t got = 0;
  while (got < n) {
    if (__atomic_load_n(&g_stop, __ATOMIC_SEQ_CST))
      return -2;
    ssize_t r = recv(sock, buf + got, n - got, 0);
    if (r == 0)
      return -1; /* 对端关闭连接 */
    if (r < 0) {
      if (errno == EINTR)
        continue; /* 被信号中断，重试 */
      return -1;  /* 真实错误 */
    }
    got += (size_t)r;
  }
  return 0;
}

/* 尝试连接单个 addrinfo：非阻塞 connect + select 超时。
 * 成功返回 socket fd，失败返回 -1。 */
static int try_connect(const struct addrinfo *ai) {
  int sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
  if (sock < 0)
    return -1;

  fcntl(sock, F_SETFL, O_NONBLOCK); /* 非阻塞：connect 立即返回 EINPROGRESS */
  int r = connect(sock, ai->ai_addr, ai->ai_addrlen);
  if (r < 0 && errno != EINPROGRESS) {
    close(sock);
    return -1;
  }

  fd_set wfds;
  FD_ZERO(&wfds);
  FD_SET(sock, &wfds);
  struct timeval tv;
  tv.tv_sec = CAM_CONNECT_TIMEOUT_MS / 1000;
  tv.tv_usec = (CAM_CONNECT_TIMEOUT_MS % 1000) * 1000;

  r = select(sock + 1, NULL, &wfds, NULL, &tv);
  if (r <= 0) { /* 0=超时, <0=出错 */
    close(sock);
    return -1;
  }

  /* select 说可写，还要用 SO_ERROR 确认真的连上了（可写也可能是连接失败） */
  int err = 0;
  socklen_t elen = sizeof(err);
  getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &elen);
  if (err != 0) {
    close(sock);
    return -1;
  }
  return sock;
}

/* 线程主体：连接 -> 收流 -> 解码 -> 发布帧序号 */
static void *cam_thread(void *arg) {
  cam_conn_t *c = (cam_conn_t *)arg;

  /* 1) 解析服务器地址（填 IP 字面量时是纯本地解析，毫秒级返回） */
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;     /* IPv4 / IPv6 都可以 */
  hints.ai_socktype = SOCK_STREAM; /* TCP */

  struct addrinfo *res = NULL;
  int gai = getaddrinfo(c->ip, c->port, &hints, &res);
  if (gai != 0) {
    cam_set_error("地址解析失败: %s", gai_strerror(gai));
    free(c);
    return NULL;
  }

  /* 2) 依次尝试解析出来的地址，连上一个即可 */
  int sock = -1;
  for (struct addrinfo *p = res; p != NULL; p = p->ai_next) {
    if (__atomic_load_n(&g_stop, __ATOMIC_SEQ_CST))
      break;
    sock = try_connect(p);
    if (sock >= 0)
      break;
  }
  freeaddrinfo(res);

  if (sock < 0) {
    cam_set_error(__atomic_load_n(&g_stop, __ATOMIC_SEQ_CST)
                      ? "已取消连接"
                      : "连接失败或超时（%d 秒），请检查 IP/端口/网络",
                  CAM_CONNECT_TIMEOUT_MS / 1000);
    free(c);
    return NULL;
  }

  /* 3) 连上了：改回阻塞模式（收流阶段简单可靠），发布 STREAMING 状态 */
  int flags = fcntl(sock, F_GETFL, 0);
  fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);

  pthread_mutex_lock(&g_mtx);
  g_sock = sock;
  g_state = CAM_ST_STREAMING;
  snprintf(g_status, sizeof(g_status), "已连接 %s:%s", c->ip, c->port);
  pthread_mutex_unlock(&g_mtx);
  CAM_LOG("已连接 %s:%s\n", c->ip, c->port);

  /* 4) 收流循环：[4B 大端长度][JPEG] 一帧一帧收、解码、发布 */
  for (;;) {
    uint8_t hdr[4];
    int r = recv_all(sock, hdr, 4);
    if (r != 0) {
      cam_set_error(r == -2 ? "已主动断开" : "连接已断开（服务器关闭了连接）");
      break;
    }

    const uint32_t n = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
                       ((uint32_t)hdr[2] << 8) | (uint32_t)hdr[3];
    if (n == 0 || n > CAM_JPEG_MAX) {
      cam_set_error("帧长度异常: %u（协议不匹配？应为 4B 大端长度 + JPEG）",
                    (unsigned)n);
      break;
    }

    r = recv_all(sock, g_jpeg, n);
    if (r != 0) {
      cam_set_error(r == -2 ? "已主动断开" : "连接已断开（帧数据不完整）");
      break;
    }

    /* 选后台帧槽：与“后台最近写入”交替的那块。
     * 主线程显示的是 g_frame_seq 对应的槽；若后台发现主线程落后超过
     * 1 帧，下面会 usleep 限速等它消费，保证不会踩主线程正在显示的槽。 */
    pthread_mutex_lock(&g_mtx);
    const int idx = (int)((g_write_seq + 1u) & 1u);
    pthread_mutex_unlock(&g_mtx);

    uint16_t dec_w = 0, dec_h = 0;
    if (decode_frame(g_jpeg, n, idx, &dec_w, &dec_h) != 0) {
      /* 解码失败只丢这一帧，保持流同步继续收下一帧 */
      pthread_mutex_lock(&g_mtx);
      g_drop_total++;
      pthread_mutex_unlock(&g_mtx);
      continue;
    }

    /* 发布新帧：g_write_seq++ 后，g_frame_seq 指向的槽就是刚解好的这块 */
    pthread_mutex_lock(&g_mtx);
    g_write_seq++;
    g_frame_seq = g_write_seq;
    g_img_w = dec_w; /* 尺寸与帧序号在同一临界区发布，主线程读到的必然配套 */
    g_img_h = dec_h;
    g_frames_total++;
    const uint32_t behind = g_write_seq - g_shown_seq;
    pthread_mutex_unlock(&g_mtx);

    if (behind > 1)
      usleep(4000); /* 主线程还没消费上一帧：限速等它，防覆盖显示中的缓冲 */
  }

  /* 5) 收尾：关 socket；若还是 STREAMING 状态则标记为断开 */
  close(sock);
  pthread_mutex_lock(&g_mtx);
  if (g_sock == sock)
    g_sock = -1;
  if (g_state == CAM_ST_STREAMING) {
    g_state = CAM_ST_ERROR;
    snprintf(g_status, sizeof(g_status), "连接已断开");
  }
  pthread_mutex_unlock(&g_mtx);
  CAM_LOG("线程退出\n");

  free(c);
  return NULL;
}

/* ----------------------- 线程生命周期 ----------------------- */

/* 停止并回收后台线程（幂等）。
 * 置 stop -> shutdown 唤醒阻塞中的 recv -> join。
 * 注意：join 会阻塞主线程；正常断开时 recv 立即返回，join 很快；
 * 若线程恰好卡在 connect 的 select（最坏 3s），主线程会等它超时后退出。 */
static void cam_stop_thread(void) {
  if (!g_thread_running)
    return;

  __atomic_store_n(&g_stop, 1, __ATOMIC_SEQ_CST);

  pthread_mutex_lock(&g_mtx);
  int s = g_sock;
  pthread_mutex_unlock(&g_mtx);
  if (s >= 0)
    shutdown(s, SHUT_RDWR); /* 让阻塞的 recv/connect 立刻出错返回 */

  pthread_join(g_thread, NULL);
  g_thread_running = 0;

  pthread_mutex_lock(&g_mtx);
  if (g_sock >= 0) {
    close(g_sock);
    g_sock = -1;
  }
  pthread_mutex_unlock(&g_mtx);
}

/* ----------------------- 主线程：UI 刷新 ----------------------- */

/* LVGL 定时器（33ms）：把后台线程的共享状态刷到 UI。
 * 所有 lv_* 调用只在这里和按钮回调里发生（主线程），保证 LVGL 线程安全。 */
static void cam_refresh_cb(lv_timer_t *t) {
  (void)t;

  /* 1) 取共享状态快照 */
  pthread_mutex_lock(&g_mtx);
  cam_state_t st = g_state;
  char status[192];
  snprintf(status, sizeof(status), "%s", g_status);
  uint32_t seq = g_frame_seq;
  uint16_t iw = g_img_w, ih = g_img_h;
  char ip[64], port[16];
  snprintf(ip, sizeof(ip), "%s", g_conn_ip);
  snprintf(port, sizeof(port), "%s", g_conn_port);

  /* fps：每秒按累计帧数差算一次 */
  static uint32_t s_last_ms = 0;
  static uint32_t s_last_total = 0;
  uint32_t now = lv_tick_get();
  if (now - s_last_ms >= 1000) {
    if (s_last_ms != 0)
      g_fps = (float)(g_frames_total - s_last_total) * 1000.0f /
              (float)(now - s_last_ms);
    s_last_ms = now;
    s_last_total = g_frames_total;
  }
  float fps = g_fps;
  uint32_t total = g_frames_total;
  pthread_mutex_unlock(&g_mtx);

  /* 2) 状态切换：设置面板 <-> 相机视图 */
  if (st != g_last_st) {
    if (st == CAM_ST_STREAMING) {
      /* 连接成功：切到相机视图 */
      lv_obj_add_flag(g_setup, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(g_view, LV_OBJ_FLAG_HIDDEN);
    } else if (st == CAM_ST_ERROR || st == CAM_ST_IDLE) {
      /* 失败/断开/重置：回设置面板，状态标签显示原因 */
      lv_obj_add_flag(g_view, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(g_setup, LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text(g_status_lab, status);
      lv_obj_set_style_text_color(g_status_lab,
                         st == CAM_ST_ERROR ? lv_color_make(0xF8, 0x71, 0x71)
                                            : lv_color_make(0x9C, 0xA3, 0xB8),
                          0);
    }
    g_last_st = st;
  }

  /* 连接中：状态标签持续显示“正在连接 …” */
  if (st == CAM_ST_CONNECTING)
    lv_label_set_text(g_status_lab, status);

  /* 3) 有新帧 -> 贴到 lv_img（指向 g_frame_seq 对应的 dsc） */
  if (seq != g_shown_seq) {
    lv_img_set_src(g_img, &g_dsc[seq & 1u]);
    /* 收到第一帧，隐藏中央的“等待画面”提示 */
    lv_obj_add_flag(g_wait_lab, LV_OBJ_FLAG_HIDDEN);
    pthread_mutex_lock(&g_mtx);
    g_shown_seq = seq;
    pthread_mutex_unlock(&g_mtx);
  }

  /* 4) 信息栏 */
  if (st == CAM_ST_STREAMING) {
    lv_label_set_text_fmt(g_info_lab, "%s:%s  %ux%u  %u 帧", ip, port,
                          (unsigned)iw, (unsigned)ih, (unsigned)total);
    lv_label_set_text_fmt(g_fps_lab, "%.1f fps", (double)fps);
  }
}

/* ----------------------- 按钮回调（主线程） ----------------------- */

/* [输入框被点击]：把虚拟键盘绑定到它并弹出。
 * QEMU 里没有真实键盘输入设备（lvgl_demo 在 fbdev 下跑），所以 IP/端口
 * 只能靠屏幕键盘输入。数字模式含 0-9、点、退格、OK、左右移动光标，
 * 正好够填 IP 和端口，不用切字母表。 */
static void cam_ta_click_cb(lv_event_t *e) {
  lv_obj_t *ta = lv_event_get_target(e);
  if (g_kb == NULL)
    return;
  lv_keyboard_set_mode(g_kb, LV_KEYBOARD_MODE_NUMBER);
  lv_keyboard_set_textarea(g_kb, ta);
  lv_obj_clear_flag(g_kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(g_kb); /* 键盘浮在设置面板之上 */
  /* 面板让位到屏幕顶部：键盘贴底弹出，两者不重叠，"连接"按钮始终可点 */
  lv_obj_align(g_setup, LV_ALIGN_TOP_MID, 0, 0);
}

/* [键盘上的 OK 键]：收起键盘，把屏幕还给设置面板（好点"连接"） */
static void cam_kb_ready_cb(lv_event_t *e) {
  if (g_kb == NULL)
    return;
  lv_keyboard_set_textarea(g_kb, NULL); /* 解绑，下次点输入框再重新绑定 */
  lv_obj_add_flag(g_kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_center(g_setup); /* 面板回到正中 */
}

/* [连接]：读 UI 输入 -> 校验 -> 起后台线程 */
static void cam_connect_cb(lv_event_t *e) {
  if (e->code != LV_EVENT_CLICKED)
    return;

  /* 开始连接了就把虚拟键盘收起来，把"连接"按钮露出来 */
  if (g_kb != NULL) {
    lv_keyboard_set_textarea(g_kb, NULL);
    lv_obj_add_flag(g_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_center(g_setup);
  }

  pthread_mutex_lock(&g_mtx);
  cam_state_t st = g_state;
  pthread_mutex_unlock(&g_mtx);
  if (st == CAM_ST_CONNECTING || st == CAM_ST_STREAMING)
    return; /* 已在连接/收流中，忽略重复点击 */

  const char *ip = lv_textarea_get_text(g_ip_ta);
  const char *port = lv_textarea_get_text(g_port_ta);
  if (ip == NULL || *ip == '\0') {
    lv_label_set_text(g_status_lab, "请先填写服务器 IP");
    lv_obj_set_style_text_color(g_status_lab, lv_color_make(0xF8, 0x71, 0x71), 0);
    return;
  }
  int p = atoi(port);
  if (p <= 0 || p > 65535) {
    lv_label_set_text(g_status_lab, "端口无效（应为 1~65535）");
    lv_obj_set_style_text_color(g_status_lab, lv_color_make(0xF8, 0x71, 0x71), 0);
    return;
  }

  /* 确保旧线程已彻底退出（幂等） */
  cam_stop_thread();

  /* 记录连接参数（信息栏回显）并拷给线程 */
  pthread_mutex_lock(&g_mtx);
  snprintf(g_conn_ip, sizeof(g_conn_ip), "%s", ip);
  snprintf(g_conn_port, sizeof(g_conn_port), "%d", p);
  g_state = CAM_ST_CONNECTING;
  snprintf(g_status, sizeof(g_status), "正在连接 %s:%d …", ip, p);
  pthread_mutex_unlock(&g_mtx);

  cam_conn_t *arg = (cam_conn_t *)malloc(sizeof(cam_conn_t));
  if (arg == NULL) {
    cam_set_error("内存不足，无法启动连接线程");
    return;
  }
  snprintf(arg->ip, sizeof(arg->ip), "%s", ip);
  snprintf(arg->port, sizeof(arg->port), "%d", p);

  __atomic_store_n(&g_stop, 0, __ATOMIC_SEQ_CST);
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  /* 默认 joinable：close() 里要 join 确保线程退出后再释放缓冲 */
  pthread_create(&g_thread, &attr, cam_thread, arg);
  pthread_attr_destroy(&attr);
  g_thread_running = 1;
}

/* [重新设置]：断开连接，回到设置面板（线程退出时会置 ERROR 状态） */
static void cam_reconf_cb(lv_event_t *e) {
  if (e->code != LV_EVENT_CLICKED)
    return;
  cam_stop_thread();
}

/* ----------------------- open / close ----------------------- */

void camera_demo_open(void) {
  CAM_LOG("open enter\n");
  lv_obj_t *scr = lv_scr_act(); /* main 已 lv_scr_load 到 demo 屏 */
  if (scr == NULL) {
    CAM_LOG("open: scr is NULL! abort\n");
    return;
  }

  /* 1) 复位一切共享状态（支持反复进入/退出本 demo） */
  cam_stop_thread();
  if (g_timer) {
    lv_timer_del(g_timer);
    g_timer = NULL;
  }
  for (int i = 0; i < 2; i++) {
    free(g_rgb[i]);
    g_rgb[i] = NULL;
    g_rgb_cap[i] = 0;
  }
  free(g_jpeg);
  g_jpeg = NULL;

  __atomic_store_n(&g_stop, 0, __ATOMIC_SEQ_CST);
  pthread_mutex_lock(&g_mtx);
  g_state = CAM_ST_IDLE;
  g_last_st = CAM_ST_IDLE;
  g_frame_seq = g_shown_seq = g_write_seq = 0;
  g_frames_total = g_drop_total = 0;
  g_fps = 0.0f;
  g_img_w = g_img_h = 0;
  snprintf(g_status, sizeof(g_status), "请输入服务器 IP 后点击「连接」");
  pthread_mutex_unlock(&g_mtx);

  /* 显示区上限：四边各留 20px 边距（信息栏/退出按钮是浮在图上的浮层，
   * 不需要额外留高度，所以尽量放大画面占满屏） */
  lv_disp_t *disp = lv_disp_get_default();
  g_disp_max_w = lv_disp_get_hor_res(disp) - 40;
  g_disp_max_h = lv_disp_get_ver_res(disp) - 40;

  /* 常驻收帧缓冲（整帧 JPEG 都在这里） */
  g_jpeg = (uint8_t *)malloc(CAM_JPEG_MAX);
  if (g_jpeg == NULL) {
    CAM_LOG("收帧缓冲 malloc %d 字节失败，demo 退出\n", CAM_JPEG_MAX);
    return; /* UI 尚未创建，直接返回；main 的 close 会安全收尾 */
  }

  /* 2) 设置面板（打开 demo 先看到这个） */
  g_setup = lv_obj_create(scr);
  lv_obj_set_size(g_setup, 500, 320);
  lv_obj_center(g_setup);
  lv_obj_set_style_bg_color(g_setup, lv_color_make(0x1E, 0x29, 0x3B), 0);
  lv_obj_set_style_bg_opa(g_setup, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(g_setup, lv_color_make(0x33, 0x44, 0x66), 0);
  lv_obj_set_style_border_width(g_setup, 1, 0);
  lv_obj_set_style_radius(g_setup, 16, 0);
  lv_obj_set_style_pad_all(g_setup, 20, 0);
  lv_obj_set_flex_flow(g_setup, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(g_setup, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(g_setup, 12, 0);

  lv_obj_t *title = lv_label_create(g_setup);
  lv_label_set_text(title, "相机 Demo");
  lv_obj_set_style_text_color(title, lv_color_white(), 0);

  lv_obj_t *hint = lv_label_create(g_setup);
  lv_label_set_text(hint, "TCP 接收手机上传的相机画面（JPEG 帧）\n"
                          "帧格式: [4 字节大端长度][JPEG 数据]");
  lv_obj_set_style_text_color(hint, lv_color_make(0x9C, 0xA3, 0xB8), 0);

  /* IP 输入行：标签 + 单行输入框（只接受数字和点） */
  lv_obj_t *ip_row = lv_obj_create(g_setup);
  lv_obj_set_size(ip_row, 460, 46);
  lv_obj_set_style_bg_opa(ip_row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(ip_row, 0, 0);
  lv_obj_set_style_pad_all(ip_row, 0, 0);
  lv_obj_set_flex_flow(ip_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(ip_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(ip_row, 10, 0);

  lv_obj_t *ip_lab = lv_label_create(ip_row);
  lv_label_set_text(ip_lab, "服务器 IP");
  lv_obj_set_style_text_color(ip_lab, lv_color_white(), 0);

  g_ip_ta = lv_textarea_create(ip_row);
  lv_obj_set_size(g_ip_ta, 360, 42);
  lv_textarea_set_one_line(g_ip_ta, true);
  lv_textarea_set_placeholder_text(g_ip_ta, "如 192.168.1.100");
  lv_textarea_set_accepted_chars(g_ip_ta, "0123456789.");
  lv_textarea_set_max_length(g_ip_ta, 63);
  /* 点输入框即弹出虚拟键盘（QEMU 无实体键盘） */
  lv_obj_add_event_cb(g_ip_ta, cam_ta_click_cb, LV_EVENT_CLICKED, NULL);

  /* 端口输入行：标签 + 单行输入框（只接受数字，默认 8888） */
  lv_obj_t *port_row = lv_obj_create(g_setup);
  lv_obj_set_size(port_row, 460, 46);
  lv_obj_set_style_bg_opa(port_row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(port_row, 0, 0);
  lv_obj_set_style_pad_all(port_row, 0, 0);
  lv_obj_set_flex_flow(port_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(port_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(port_row, 10, 0);

  lv_obj_t *port_lab = lv_label_create(port_row);
  lv_label_set_text(port_lab, "端口");
  lv_obj_set_style_text_color(port_lab, lv_color_white(), 0);

  g_port_ta = lv_textarea_create(port_row);
  lv_obj_set_size(g_port_ta, 200, 42);
  lv_textarea_set_one_line(g_port_ta, true);
  lv_textarea_set_accepted_chars(g_port_ta, "0123456789");
  lv_textarea_set_max_length(g_port_ta, 5);
  lv_textarea_set_text(g_port_ta, CAM_DEFAULT_PORT);
  lv_obj_add_event_cb(g_port_ta, cam_ta_click_cb, LV_EVENT_CLICKED, NULL);

  /* 屏幕虚拟键盘：浮在设置面板之上、贴屏幕底部，默认隐藏。
   * 宽度跟随屏幕（左右各留 20），高度取屏幕 45%（上限 200）——
   * 数字键盘只有 4 行，200px 足够；上限是为了别在小屏上吃掉整个下半屏。 */
  g_kb = lv_keyboard_create(scr);
  lv_obj_set_width(g_kb, lv_disp_get_hor_res(disp) - 40);
  lv_obj_set_height(g_kb, LV_MIN(200, lv_disp_get_ver_res(disp) * 45 / 100));
  lv_obj_align(g_kb, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_keyboard_set_mode(g_kb, LV_KEYBOARD_MODE_NUMBER);
  /* 按键文字用 Montserrat 24（紧凑、纯 ASCII）；不用默认中文字体，
     否则单个按键太宽导致 btnnmatrix 排不下 */
  lv_obj_set_style_text_font(g_kb, &lv_font_montserrat_24, LV_PART_ITEMS);
  lv_obj_add_event_cb(g_kb, cam_kb_ready_cb, LV_EVENT_READY, NULL);
  lv_obj_add_flag(g_kb, LV_OBJ_FLAG_HIDDEN);

  /* 连接按钮 */
  lv_obj_t *connect_btn = lv_btn_create(g_setup);
  lv_obj_set_size(connect_btn, 460, 46);
  lv_obj_set_style_bg_color(connect_btn, lv_color_make(0x10, 0xB9, 0x81), 0);
  lv_obj_set_style_radius(connect_btn, 10, 0);
  lv_obj_t *connect_lab = lv_label_create(connect_btn);
  lv_label_set_text(connect_lab, "连 接");
  lv_obj_set_style_text_color(connect_lab, lv_color_white(), 0);
  lv_obj_add_event_cb(connect_btn, cam_connect_cb, LV_EVENT_CLICKED, NULL);

  /* 状态标签（连接中/失败原因都显示在这里） */
  g_status_lab = lv_label_create(g_setup);
  lv_label_set_text(g_status_lab, g_status);
  lv_label_set_long_mode(g_status_lab, LV_LABEL_LONG_CLIP);
  lv_obj_set_width(g_status_lab, 460);
  lv_obj_set_style_text_color(g_status_lab, lv_color_make(0x9C, 0xA3, 0xB8), 0);

  /* 3) 相机视图（连接成功后由定时器切出来；初始隐藏） */
  g_view = lv_obj_create(scr);
  lv_obj_set_size(g_view, lv_disp_get_hor_res(disp), lv_disp_get_ver_res(disp));
  lv_obj_set_pos(g_view, 0, 0);
  lv_obj_set_style_bg_color(g_view, lv_color_make(0x00, 0x00, 0x00), 0);
  lv_obj_set_style_bg_opa(g_view, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(g_view, 0, 0);
  lv_obj_set_style_pad_all(g_view, 0, 0);
  lv_obj_clear_flag(g_view, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(g_view, LV_OBJ_FLAG_HIDDEN);

  g_img = lv_img_create(g_view);
  lv_obj_center(g_img);

  /* 中央等待提示（收到第一帧后隐藏） */
  g_wait_lab = lv_label_create(g_view);
  lv_label_set_text(g_wait_lab, "已连接，等待手机上传画面…");
  lv_obj_set_style_text_color(g_wait_lab, lv_color_make(0x9C, 0xA3, 0xB8), 0);
  lv_obj_center(g_wait_lab);

  /* 左上：地址 + 分辨率 + 帧数；右上：fps */
  g_info_lab = lv_label_create(g_view);
  lv_label_set_text(g_info_lab, "");
  lv_obj_set_style_text_color(g_info_lab, lv_color_make(0x34, 0xD3, 0x99), 0);
  lv_obj_align(g_info_lab, LV_ALIGN_TOP_LEFT, 12, 10);

  g_fps_lab = lv_label_create(g_view);
  lv_label_set_text(g_fps_lab, "");
  lv_obj_set_style_text_color(g_fps_lab, lv_color_make(0x34, 0xD3, 0x99), 0);
  lv_obj_align(g_fps_lab, LV_ALIGN_TOP_LEFT, 12, 34);

  /* 右下：重新设置（断开回设置面板） */
  lv_obj_t *reconf_btn = lv_btn_create(g_view);
  lv_obj_set_size(reconf_btn, 130, 40);
  lv_obj_align(reconf_btn, LV_ALIGN_BOTTOM_RIGHT, -12, -12);
  lv_obj_set_style_bg_color(reconf_btn, lv_color_make(0x25, 0x32, 0x4A), 0);
  lv_obj_set_style_radius(reconf_btn, 10, 0);
  lv_obj_t *reconf_lab = lv_label_create(reconf_btn);
  lv_label_set_text(reconf_lab, "重新设置");
  lv_obj_set_style_text_color(reconf_lab, lv_color_white(), 0);
  lv_obj_add_event_cb(reconf_btn, cam_reconf_cb, LV_EVENT_CLICKED, NULL);

  /* 4) 主线程刷新定时器 */
  g_timer = lv_timer_create(cam_refresh_cb, CAM_REFRESH_MS, NULL);
  CAM_LOG("open done\n");
}

void camera_demo_close(void) {
  CAM_LOG("close enter\n");

  /* 顺序：先停线程（它可能还在写共享缓冲/占着 g_mtx），再停定时器、
   * 释放缓冲，最后清 UI。timer 回调持有 g_mtx 的概率与线程相同，
   * 但线程已 join 后 timer 再跑也只会读到复位后的状态，安全。 */
  cam_stop_thread();

  if (g_timer) {
    lv_timer_del(g_timer);
    g_timer = NULL;
  }

  for (int i = 0; i < 2; i++) {
    free(g_rgb[i]);
    g_rgb[i] = NULL;
    g_rgb_cap[i] = 0;
    memset(&g_dsc[i], 0, sizeof(g_dsc[i]));
  }
  free(g_jpeg);
  g_jpeg = NULL;

  /* 清 UI（demo 屏上的所有子对象）并复位指针 */
  lv_obj_clean(lv_scr_act());
  g_setup = g_ip_ta = g_port_ta = g_status_lab = NULL;
  g_kb = NULL; /* 键盘对象已被 lv_obj_clean 删除，必须清指针 */
  g_view = g_img = g_info_lab = g_fps_lab = g_wait_lab = NULL;
  CAM_LOG("close done\n");
}
