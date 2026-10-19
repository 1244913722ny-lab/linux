/**
 * network_demo.c - Linux 网络编程入门：对外 TCP 连通性测试 (LVGL v8.3.11)
 *
 * 这个 demo 演示的“网络编程”知识点：
 *   1) getaddrinfo()  —— 域名/服务名 ->  sockaddr 列表（含 DNS 解析）；
 *   2) socket()/connect() —— TCP 建连；
 *   3) 非阻塞 connect + select() 超时 —— 避免阻塞 UI 线程；
 *   4) 多线程 + LVGL 线程安全 —— 网络是阻塞操作，必须放后台线程；而 LVGL
 *      不是线程安全的，所有 lv_xxx() 调用都必须回到主线程。本 demo 用
 *      “后台线程写共享缓冲(mutex 保护) + 主线程定时器轮询刷新 UI” 的模式，
 *      这是嵌入式 GUI 里做网络/文件 IO 的标准做法。
 *
 * 运行前提：QEMU 已用 -device lan9118 接网卡，rootfs 用 udhcpc 拿到 IP/DNS。
 * 连外网依赖 QEMU 用户态 NAT；连 127.0.0.1 等本地地址则永远可用。
 */

#include "network_demo.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define user_log(fmt, ...)                                                     \
  fprintf(stderr, "[__func__ = %s] " fmt, __func__, ##__VA_ARGS__)
/* ----------------------- 全局状态 ----------------------- */

/* 后台测试线程与主线程之间的“日志”通过下面这段共享内存传递。
 * 写入方：后台测试线程；读取方：主线程的刷新定时器。
 * 两者用 g_mtx 互斥，避免并发读写导致文本错乱。 */
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static char g_log_buf[2048];     /* 累积日志文本 */
static bool g_log_dirty = false; /* 是否有新内容待刷新到 UI */

static lv_obj_t *g_log_label = NULL; /* 显示日志的标签（主线程持有） */
static lv_timer_t *g_timer = NULL;   /* 主线程刷新定时器 */
static int g_running = 0;   /* 是否正在测试（防止并发点击）。跨线程读写，统一用 __atomic_* 访问 */
static int g_epoch =
    0; /* 每次测试自增，区分“当前测试”，避免旧线程误清标志 */
static int g_running_epoch = 0; /* 当前正在跑的测试 epoch */
static long g_test_start_ms =
    0; /* 当前测试开始时刻(lv_tick_get ms)，供看门狗判断超时 */
#define NET_WATCHDOG_MS 4000 /* 测试超过此时长强制结束，避免 UI 卡死 */

/* 预设的可测试目标（host:port）。点击对应按钮即用该目标发起测试。 */
static const char *g_targets[] = {
    "example.com:80", /* 经典 HTTP 站点，验证 DNS + 外网 TCP */
    "8.8.8.8:53",     /* Google 公共 DNS，验证纯 IP 直连 */
    "1.1.1.1:53",     /* Cloudflare DNS */
    "github.com:443", /* HTTPS 站点 */
};
#define TARGET_COUNT ((int)(sizeof(g_targets) / sizeof(g_targets[0])))

/* ----------------------- 日志辅助 ----------------------- */

/* 往共享日志缓冲追加一行（线程安全）。被后台线程调用。 */
static void log_append(const char *fmt, ...) {
  char tmp[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);

  pthread_mutex_lock(&g_mtx);
  uint32_t used = strnlen(g_log_buf, sizeof(g_log_buf) - 1);
  uint32_t cap = sizeof(g_log_buf) - 1;
  uint32_t add = strnlen(tmp, sizeof(tmp));
  if (used + add + 1 > cap) {                       /* 将满 */
    size_t drop = (used + add + 1) - cap + cap / 4; /* 多丢 1/4 留余量 */
    if (drop >= used)
      used = 0; /* 实在太多就清空 */
    else {
      memmove(g_log_buf, g_log_buf + drop, used - drop + 1);
      used -= drop;
    }
  }
  strncat(g_log_buf, tmp, cap - used - 1);

  g_log_dirty = true;
  pthread_mutex_unlock(&g_mtx);
}

/* ----------------------- 后台测试线程 ----------------------- */

/* 把 "host:port" 拆成 host 与 port 两段（按最后一个 ':' 切分，兼容 IPv6
 * 之外的场景）。 */
static void split_target(const char *target, char *host, size_t host_sz,
                         char *port, size_t port_sz) {
  const char *colon = strrchr(target, ':');
  if (!colon) {
    snprintf(host, host_sz, "%s", target);
    snprintf(port, port_sz, "80");
    return;
  }
  snprintf(host, host_sz, "%.*s", (int)(colon - target), target);
  snprintf(port, port_sz, "%s", colon + 1);
}

/* 对单个 sockaddr 做“非阻塞 connect + select 超时”测试。
 * 返回 0 表示连通成功，非 0 表示失败（并返回耗时 ms）。 */
static int try_connect(const struct addrinfo *ai, long *ms_out) {
  int sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
  if (sock < 0) {
    log_append("  [ERR] socket() 失败: %s (%d)\n", strerror(errno), errno);
    return -1;
  }

  /* 设为非阻塞：connect 会立即返回 EINPROGRESS，我们在 select 里等它可写 */
  fcntl(sock, F_SETFL, O_NONBLOCK);

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  int r = connect(sock, ai->ai_addr, ai->ai_addrlen);
  if (r < 0 && errno != EINPROGRESS) {
    log_append("  [ERR] connect() 立即失败: %s (%d)\n", strerror(errno), errno);
    close(sock);
    *ms_out = 0;
    return -1;
  }

  /* 等待 socket 可写（连接成功）或超时（2 秒，demo 里足够判断是否可达） */
  fd_set wfds;
  FD_ZERO(&wfds);
  FD_SET(sock, &wfds);
  struct timeval tv = {2, 0};

  r = select(sock + 1, NULL, &wfds, NULL, &tv);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  *ms_out =
      (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;

  if (r < 0) {
    log_append("  [ERR] select() 失败: %s (%d)\n", strerror(errno), errno);
    close(sock);
    return -1;
  }
  if (r == 0) {
    log_append("  [TIMEOUT] %ld ms 内未连上（目标不可达/被过滤）\n", *ms_out);
    close(sock);
    return -1;
  }

  /* select 说可写，还需用 SO_ERROR 确认是否真的连上了（可写也可能是连接失败）
   */
  int err = 0;
  socklen_t elen = sizeof(err);
  getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &elen);
  if (err != 0) {
    log_append("  [ERR] connect 失败: %s (%d)\n", strerror(err), err);
    close(sock);
    return -1;
  }

  close(sock);
  return 0; /* 成功 */
}

/* 线程入口：解析 -> 逐个地址尝试 -> 汇总结果 */
static void *test_thread(void *arg) {
  const char *target = (const char *)arg;
  int my_epoch =
      __atomic_load_n(&g_running_epoch, __ATOMIC_SEQ_CST); /* 捕获本测试 epoch，结束时只清自己那次测试的标志 */

  char host[256], port[32];
  split_target(target, host, sizeof(host), port, sizeof(port));

  log_append("\n>>> 测试目标: %s\n", target);

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;     /* IPv4 / IPv6 都接受 */
  hints.ai_socktype = SOCK_STREAM; /* TCP */

  struct addrinfo *res = NULL;
  int gai = getaddrinfo(host, port, &hints, &res);
  if (gai != 0) {
    log_append("  [DNS] 解析失败: %s\n", gai_strerror(gai));
    log_append("  [RESULT] 失败（域名解析阶段）\n");
    free((void *)target);
    if (__atomic_load_n(&g_epoch, __ATOMIC_SEQ_CST) == my_epoch)
      __atomic_store_n(&g_running, 0, __ATOMIC_SEQ_CST); /* 仅当仍是本测试才清标志 */
    return NULL;
  }

  log_append("  [DNS] 解析到以下地址，依次尝试:\n");
  int ok = 0;
  long best_ms = 0;
  for (struct addrinfo *p = res; p != NULL; p = p->ai_next) {
    char ipbuf[INET6_ADDRSTRLEN] = {0};
    if (p->ai_family == AF_INET) {
      inet_ntop(AF_INET, &((struct sockaddr_in *)p->ai_addr)->sin_addr, ipbuf,
                sizeof(ipbuf));
    } else if (p->ai_family == AF_INET6) {
      inet_ntop(AF_INET6, &((struct sockaddr_in6 *)p->ai_addr)->sin6_addr,
                ipbuf, sizeof(ipbuf));
    }
    log_append("  - %s\n", ipbuf);
    log_append("  [CONNECT] 正在连接 %s …\n", ipbuf);

    long ms = 0;
    if (try_connect(p, &ms) == 0) {
      log_append("  [OK ] 连接成功，耗时 %ld ms\n", ms);
      ok = 1;
      best_ms = ms;
      break; /* 连上一个即可 */
    }
  }

  freeaddrinfo(res);

  if (ok) {
    log_append("  [RESULT] 成功（%s 可达，%ld ms）\n", target, best_ms);
  } else {
    log_append("  [RESULT] 失败（所有地址均不可达，请检查网络/DNS/防火墙）\n");
  }

  free((void *)target);
  if (__atomic_load_n(&g_epoch, __ATOMIC_SEQ_CST) == my_epoch)
    __atomic_store_n(&g_running, 0, __ATOMIC_SEQ_CST); /* 标记测试结束，允许下一次点击（仅限本测试） */
  return NULL;
}

/* 按钮点击：启动一次测试（若正在测试则忽略） */
static void target_btn_cb(lv_event_t *e) {
  if (e->code != LV_EVENT_CLICKED)
    return;

  if (__atomic_load_n(&g_running, __ATOMIC_SEQ_CST)) {
    log_append("[提示] 上一次测试还在进行，请稍候…\n");
    return;
  }

  const char *target = (const char *)lv_event_get_user_data(e);
  /* 开启新测试：自增 epoch，并用原子操作更新“当前正在跑的测试”与“运行中”标志。
   * g_epoch 自增发生在 pthread_create 之前，配合 __atomic_* 使后台线程能读到本测试
   * 的 epoch，避免旧(detach)线程误清本测试的标志。 */
  int new_epoch = __atomic_add_fetch(&g_epoch, 1, __ATOMIC_SEQ_CST);
  __atomic_store_n(&g_running_epoch, new_epoch, __ATOMIC_SEQ_CST);
  __atomic_store_n(&g_running, 1, __ATOMIC_SEQ_CST);
  g_test_start_ms = lv_tick_get(); /* 记录开始时刻，供看门狗判断 */

  /* 复制字符串传给线程（线程结束后 free），避免指向常量区的生命周期问题 */
  char *arg = strdup(target);
  if (!arg) {
    log_append("[ERR] 内存不足\n");
    __atomic_store_n(&g_running, 0, __ATOMIC_SEQ_CST);
    return;
  }

  /* 用 detach 状态创建：线程结束后自动回收，close() 无需也不能 pthread_join，
   * 避免 join 一个(因 DNS 无响应而)卡住的线程导致整个 app 冻结。 */
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_t tid;
  pthread_create(&tid, &attr, test_thread, arg);
  pthread_attr_destroy(&attr);
}

/* ----------------------- 主线程刷新定时器 ----------------------- */

/* 由 LVGL 定时器在主线程周期性调用：把共享缓冲里的新日志刷到标签上。
 * 关键：lv_label_set_text() 只在主线程调用，保证 LVGL 线程安全。 */
static void refresh_timer_cb(lv_timer_t *t) {
  (void)t;
  /* 看门狗：若测试已超过 NET_WATCHDOG_MS 仍无结束（多半卡在 getaddrinfo
   * 等无超时的 阻塞调用），强制清 g_running，让按钮恢复可用，避免“卡死”体感。
   */
  if (__atomic_load_n(&g_running, __ATOMIC_SEQ_CST) &&
      (lv_tick_get() - ((uint32_t)g_test_start_ms)) > NET_WATCHDOG_MS) {
    log_append(
        "[提示] 测试超过 %d 秒无响应，已强制结束（可能 DNS/网络阻塞）。\n",
        NET_WATCHDOG_MS / 1000);
    if (__atomic_load_n(&g_epoch, __ATOMIC_SEQ_CST) ==
        __atomic_load_n(&g_running_epoch, __ATOMIC_SEQ_CST))
      __atomic_store_n(&g_running, 0, __ATOMIC_SEQ_CST); /* 仅清当前测试 */
  }

  /* 防御：lv_timer_del() 只是“标记删除”，本次 handler
   * 仍可能在真正移除前再跑一次； 而 close() 在 del 之后会把 g_log_label 置
   * NULL，若不判空便会解引用 -> 段错误。 （反复进入/退出 Network demo
   * 时极易触发，是隐藏的崩溃源） */
  if (g_log_label == NULL)
    return;

  pthread_mutex_lock(&g_mtx);
  if (g_log_dirty) {
    lv_label_set_text(g_log_label, g_log_buf);
    /* 滚到日志最底部，方便看到最新结果 */
    lv_obj_scroll_to_y(lv_obj_get_parent(g_log_label), LV_COORD_MAX,
                       LV_ANIM_OFF);
    g_log_dirty = false;
  }
  pthread_mutex_unlock(&g_mtx);
}

/* ----------------------- open / close ----------------------- */

void network_demo_open(void) {
  fprintf(stderr, "[net] open enter\n");
  lv_obj_t *scr = lv_scr_act(); /* 当前是 demo 屏（main 已 lv_scr_load 过） */
  if (scr == NULL) {
    fprintf(stderr, "[net] open: scr is NULL! abort\n");
    return;
  }
  lv_obj_set_style_bg_color(scr, lv_color_make(0x12, 0x18, 0x2B), 0);

  /* 重置共享状态（支持反复进入/退出） */
  pthread_mutex_lock(&g_mtx);
  strcpy(g_log_buf, "日志区。点击上方任意目标开始测试。\n"
                    "(若全部失败，多半是 QEMU 网卡/udhcpc 未就绪，可在串口执行 "
                    "ifconfig 自查。)\n");
  g_log_dirty = true;
  pthread_mutex_unlock(&g_mtx);
  __atomic_store_n(&g_running, 0, __ATOMIC_SEQ_CST);
  __atomic_add_fetch(&g_epoch, 1, __ATOMIC_SEQ_CST); /* 使上一屏遗留的(detach)后台线程无法再清本屏的 g_running */

  /* 标题 */
  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "Linux 网络编程: TCP 连通测试");
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
  lv_obj_set_style_text_font(title, LV_FONT_DEFAULT, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);
  fprintf(stderr, "[net] open: title ok\n");

  /* 说明 */
  lv_obj_t *hint = lv_label_create(scr);
  lv_label_set_text(hint, "点击下方目标发起 TCP connect（非阻塞+5s超时）。\n"
                          "后端: getaddrinfo -> socket -> connect -> "
                          "select。结果经后台线程回传主线程刷新。");
  lv_obj_set_style_text_color(hint, lv_color_make(0x9C, 0xA3, 0xB8), 0);
  lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 48);
  fprintf(stderr, "[net] open: hint ok\n");

  /* 目标按钮：flex 自动换行行 */
  lv_obj_t *btn_cont = lv_obj_create(scr);
  lv_obj_set_size(btn_cont, 720, 120);
  lv_obj_align(btn_cont, LV_ALIGN_TOP_MID, 0, 130);
  lv_obj_set_style_bg_color(btn_cont, lv_color_make(0x12, 0x18, 0x2B), 0);
  lv_obj_set_style_border_width(btn_cont, 0, 0);
  lv_obj_set_flex_flow(btn_cont, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(btn_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(btn_cont, 16, 0);
  lv_obj_set_style_pad_row(btn_cont, 12, 0);

  for (int i = 0; i < TARGET_COUNT; i++) {
    lv_obj_t *b = lv_btn_create(btn_cont);
    lv_obj_set_size(b, 200, 44);
    lv_obj_set_style_bg_color(b, lv_color_make(0x0E, 0xA5, 0xE9), 0);
    lv_obj_set_style_radius(b, 10, 0);

    lv_obj_t *bl = lv_label_create(b);
    lv_label_set_text(bl, g_targets[i]);
    lv_obj_set_style_text_color(bl, lv_color_white(), 0);

    lv_obj_add_event_cb(b, target_btn_cb, LV_EVENT_ALL, (void *)g_targets[i]);
  }
  fprintf(stderr, "[net] open: buttons ok\n");

  /* 日志面板：固定大小的滚动容器，里面放一个“固定像素宽度”的多行标签。
   * 标签用 LV_LABEL_LONG_CLIP：固定像素宽度内不折行、溢出裁切（不用 WRAP，
   * 可避免 v8.3.11 对 UTF-8 中文多字节的断行越界问题）；日志本身已用 \n
   * 手动换行。
   *
   * 排雷提醒：给 label 设字体必须用“字体对象指针” &lv_font_montserrat_14，
   * 不能写 LV_FONT_MONTSERRAT_14 —— 后者只是 lv_conf.h 里的“使能开关”，
   * 展开为常量 1。把 1 当 const lv_font_t* 传进去，LVGL 后续
   * (lv_label_refr_text -> lv_txt_get_size) 解引用 0x1 会直接段错误。 */
  fprintf(stderr, "[net] open: create panel\n");
  lv_obj_t *panel = lv_obj_create(scr);
  lv_obj_set_size(panel, 720, 360);
  lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 270);
  lv_obj_set_style_bg_color(panel, lv_color_make(0x0B, 0x0F, 0x1A), 0);
  lv_obj_set_style_border_color(panel, lv_color_make(0x1F, 0x29, 0x37), 0);
  lv_obj_set_style_border_width(panel, 1, 0);
  lv_obj_set_style_pad_all(panel, 8, 0);
  lv_obj_add_flag(panel, LV_OBJ_FLAG_SCROLLABLE); /* 日志超长时可滚动查看 */

  fprintf(stderr, "[net] open: create label\n");
  g_log_label = lv_label_create(panel);
  lv_obj_set_width(g_log_label, 700); /* ① 先设固定像素宽度 */
  fprintf(stderr, "[net] open: set_width\n");
  lv_label_set_long_mode(
      g_log_label,
      LV_LABEL_LONG_CLIP); /* ② CLIP：不折行、溢出裁切，绕开 WRAP 崩溃 */
  fprintf(stderr, "[net] open: set_long_mode\n");
  lv_obj_set_style_text_color(g_log_label, lv_color_make(0x34, 0xD3, 0x99), 0);
  lv_obj_set_style_text_font(g_log_label, LV_FONT_DEFAULT, 0);
  fprintf(stderr, "[net] open: set_font\n");
  lv_label_set_text(g_log_label, "日志区。点击上方任意目标开始测试。\n"
                                 "(若全部失败，多半是 QEMU 网卡/udhcpc "
                                 "未就绪，可在串口执行 ifconfig 自查。)\n");
  fprintf(stderr, "[net] open: label ok\n");

  /* 主线程刷新定时器：每 100ms 把后台线程日志刷到 UI */
  if (g_timer)
    lv_timer_del(g_timer); /* 防御：上一次没清理干净 */
  g_timer = lv_timer_create(refresh_timer_cb, 100, NULL);
  fprintf(stderr, "[net] open done\n");
}

void network_demo_close(void) {
  /* 顺序很重要：先停定时器、确保后台线程结束，再删 UI 对象。
   * 否则定时器/线程可能访问已删除的 g_log_label -> 崩溃。 */
  if (g_timer) {
    lv_timer_del(g_timer);
    g_timer = NULL;
  }

  /* 后台测试线程是 DETACHED 的，无需也不能 pthread_join（join 一个可能因 DNS
   * 无响应而挂住的线程会把整个 app 冻结）；它会在自己结束后自动回收，其末尾对
   * g_log_buf 的写入受 g_mtx 保护，不会访问已删除的 UI 对象。 */
  g_log_label = NULL;
  lv_obj_clean(
      lv_scr_act()); /* 清掉 demo 屏上的所有子对象（与内置 demos 一致） */
}
