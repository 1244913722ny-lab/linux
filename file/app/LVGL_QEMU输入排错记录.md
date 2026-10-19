# LVGL framebuffer demo 在 QEMU(vexpress-a9) 上的输入排错记录

> 目的：记录「界面能显示、但鼠标点不到按钮」这一问题的根因与解决，方便后续回顾。
> 环境：QEMU 模拟 ARM vexpress-a9（PL111 LCD → `/dev/fb0`，1024×768，**16bit RGB565**）
> 代码：LVGL v8.3.11 + 自写 framebuffer 显示驱动 + EVDEV 输入（`file/app/` 下）

---

## 1. 现象

LVGL 界面能正常上屏（标题 + 按钮 + 计数器），但**鼠标点击按钮毫无反应**，计数器数字不增加。

---

## 2. 根因：LVGL 的 pointer 输入接错了 `/dev/input` 节点

核心结论：**读不到鼠标坐标，坐标永远停在 `(0,0)`，所以点不到按钮。** 具体由三层原因叠加而成。

### 2.1 平台层 —— vexpress-a9 没有 USB

QEMU 的 `vexpress-a9` 机器模型里**不含 PCI / USB 控制器**，所以想加 `-device usb-tablet` 会直接报：

```
qemu-system-arm: -device usb-tablet: No 'usb-bus' bus found for device 'usb-tablet'
```

这台板子默认只有 **PL050 KMI 的 PS/2 键盘和鼠标**（鼠标是**相对坐标** `EV_REL`）。

### 2.2 驱动层 —— 只支持触摸屏坐标，漏了鼠标

最早的 EVDEV 驱动 `evdev_read_cb` 只处理 `EV_ABS`（触摸屏的绝对坐标）：

```c
if (e.type == EV_ABS) {            // 触摸屏
    if (e.code == ABS_X) evdev_x = e.value;
    ...
}
```

而 PS/2 / USB 鼠标产生的是 `EV_REL`（相对位移）和 `BTN_LEFT`，原代码根本没处理，坐标读不到。

### 2.3 最关键 —— 旧二进制把「键盘」错配成了指针设备

排查时在 demo 启动日志里看到：

```
[lv_fb] EVDEV 输入已启用: /dev/input/event0
```

在 guest 里执行 `cat /proc/bus/input/devices`，确认了节点的真实身份：

```
event0 = "AT Raw Set 2 keyboard"      EV=120013  (含 EV_KEY，无 EV_REL/EV_ABS)
event1 = "ImExPS/2 Generic Explorer Mouse"  EV=7  (EV_KEY+EV_REL，含 BTN_LEFT)
```

也就是说当时跑的是**旧版本二进制**，把 **`event0`（键盘）当成了 pointer 设备**。

键盘只发 `EV_KEY`（字母、回车等），**没有 `BTN_LEFT` / `EV_REL` / `EV_ABS`**。于是：

- `evdev_read_cb` 里的 `BTN_TOUCH/BTN_LEFT` 分支永远不触发 → `evdev_pressed` 恒为 `false`；
- 没有任何 `EV_ABS/EV_REL` 来更新 → `evdev_x/y` 永远停在初始的 `(0,0)`。

光标卡在左上角、点不到按钮 —— 这就是「不能点击」的**直接原因**。

> 顺带说明：`event0` 被当成 pointer 是「旧硬编码版本」的锅，代码后来已改成自动探测。当时只要重编最新代码即可。

---

## 3. 解决方案：按设备能力自动分流

改造 `lv_fb_drv.c`，对每个 `/dev/input/eventX` 调用 `ioctl(EVIOCGBIT)` 查询它支持的事件类型，按能力分流，而不是写死节点：

- **pointer（鼠标 / 触摸屏）**：必须含 `EV_REL`，或（`EV_ABS` + `EV_KEY`）。
  - 鼠标：`EV_REL` 累加到当前坐标（带边界钳制），按键用 `BTN_LEFT`；
  - 触摸屏：`EV_ABS` 直接取绝对坐标。
- **keypad（键盘）**：含 `KEY_ENTER` 且**不是** pointer 设备 → 把 `KEY_ENTER` 映射成 LVGL 的 `LV_KEY_ENTER`。

分流后结果（与本机 `event` 节点完全吻合）：

| 节点 | 设备 | 分配角色 |
|------|------|----------|
| `event0` | AT Raw Set 2 keyboard | **keypad**（回车 = 点击） |
| `event1` | ImExPS/2 Generic Explorer Mouse | **pointer**（红光标 + 点击） |

`lv_keyboard_init()` 里把按钮放进 `lv_group`，再 `lv_indev_set_group(kbd, group)`，键盘回车即可触发按钮的 `LV_EVENT_CLICKED`。

---

## 4. 验证步骤

```bash
# 1) 在装有交叉编译器的环境重编（默认 arm-linux-gnueabihf-gcc，已 -static）
cd /d/linux-study/linux/file/app
make clean && make

# 2) 把新的 ARM 静态二进制刷进 QEMU 的 SD 镜像（FAT P1 偏移 1MiB）
mcopy -i "full_sd.img@@1M" build/bin/lvgl_demo ::/lvgl_demo

# 3) 启动 QEMU（后台运行，串口/monitor 改接本地 TCP，不再占用宿主终端）
cd /d/linux-study/linux/file/uboot-start
./start_uboot.sh
#   脚本打印 PID，并在 127.0.0.1:4321(串口控制台) / 4322(monitor) 监听

# 4) 另开一个终端，连串口控制台，进 ARM Linux 后运行 demo
telnet 127.0.0.1 4321
#   （连上即是 guest 的登录 shell）
./lvgl_demo
```

> **退出 demo / 虚拟机的正确方式（见第 7 节）**：现用 `-serial telnet` + 后台 QEMU，
> 宿主终端的 Ctrl+C 不会再杀 QEMU。关 demo 在 telnet 里 Ctrl+C 或 `pkill lvgl_demo`；
> 关虚拟机在 monitor(`telnet 127.0.0.1 4322`) 输 `quit`，或宿主端 `pkill -f qemu-system-arm`。

启动日志应能看到（确认分流正确）：

```
[lv_fb] probe /dev/input/event0 ("AT Raw Set 2 keyboard"): ABS=0 REL=0 KEY=1
[lv_fb] probe /dev/input/event1 ("ImExPS/2 Generic Explorer Mouse"): ABS=0 REL=1 KEY=1
[lv_fb] EVDEV 输入已启用: /dev/input/event1 (mouse/rel)
[lv_fb] kbdprobe /dev/input/event0 ("AT Raw Set 2 keyboard"): ENTER=1 REL=0 ABS=0
[lv_fb] KEYPAD 输入已启用: /dev/input/event0
```

两种点击验证方式：

1. **键盘回车（最稳，推荐）**：QEMU 的 PS/2 键盘一定在，直接按 **回车** → `Clicked:` 数字 +1。
2. **鼠标**：在 gtk 窗口里**点一下抓取鼠标**（Ctrl+Alt 释放），红色光标跟随移动，移到蓝色 `Click me` 按钮上按下 → `Clicked:` +1。

> 注：PS/2 相对鼠标在窗口里会有漂移，体验不如绝对坐标；验证点击功能用键盘回车最快最稳。

---

## 5. 运行后左侧出现黑色方块

### 现象
QEMU 窗口左上角附近总有一块黑色小方块，如图：

![左侧黑色滚动条示意](clipboard-2026-10-12T06-11-21-462Z-117744ea.png)

### 根因
这块黑块不是滚动条（滚动条会更长更细），而是 **LVGL 默认主题给光标对象加的阴影/边框**。创建光标时只改了背景色和 border_width，没清掉主题默认的 `shadow_width`/`shadow_color`/`outline` 等样式；光标在 `(0,0)` 首次绘制后移动到别处，旧位置如果没被重绘，就会留下一块黑色残影。

### 解决
创建光标对象时先把所有默认样式清掉，再只设红色背景：

```c
lv_obj_t *cursor = lv_obj_create(lv_scr_act());
lv_obj_remove_style_all(cursor);                       /* 清掉主题默认阴影/边框 */
lv_obj_set_size(cursor, 12, 12);
lv_obj_set_style_bg_color(cursor, lv_color_hex(0xFF0000), 0);
lv_obj_set_style_border_width(cursor, 0, 0);
lv_indev_set_cursor(mouse, cursor);
```

同时 screen 也建议关闭滚动（避免滚动条混淆）：

```c
lv_obj_t *scr = lv_scr_act();
lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
```

> 注意：必须**重新编译运行**才能生效。如果改完还看到黑块，确认跑的是新二进制（`make clean && make`）。

---

## 6. 附：本项目其它构建踩坑速查

| 现象 | 根因 | 解决 |
|------|------|------|
| `ld: cannot open output file build/bin/lvgl_demo` | Makefile 漏建 `build/bin`，链接目标只依赖 `.o` | 给 `build/bin` 加创建规则，并作为 order-only 依赖挂到链接目标 |
| `line 2: syntax error: unexpected (` | 之前用 `make CC=gcc` 编出 **x86-64** 二进制，却在 ARM 上跑（ENOEXEC → bash 退化当脚本解释） | `CC` 改成 `arm-linux-gnueabihf-gcc` 重编；`file` 确认是 ARM |
| `./lvgl_demo: not found` | 精简 rootfs 缺 `ld-linux-armhf.so.3` 动态链接器 | 链接加 `-static` 静态链接 |
| `CC ?= arm-linux-gnueabihf-` 末尾多余横杠 | 实际执行 `arm-linux-gnueabihf- -c`（命令不存在） | 写成完整 `arm-linux-gnueabihf-gcc` |

---

## 6. 运行与退出方式（QEMU 控制台占用 / Ctrl+C 误杀虚拟机）

**现象**：运行 `./lvgl_demo` 后，控制台不能输入别的命令；在宿主机终端按 `Ctrl+C` 会直接把整个 QEMU 虚拟机杀掉。

**根因**：旧版 `start_uboot.sh` 用的是 `-serial stdio` + 前台 QEMU。
- demo 在前台进程里运行，按 Unix 前台风俗占用 guest 控制台（这是正常行为，不是 bug）。
- 更关键：`-serial stdio` 把 QEMU 串口接到**宿主终端**；QEMU 又是前台进程，于是宿主终端的 `Ctrl+C`（SIGINT）直接发给 **QEMU 进程本身** → 整个虚拟机退出，而不是送给 guest 里的 demo。

**解决**（已改 `start_uboot.sh`）：
- 串口控制台 `-serial stdio` → `-serial telnet:127.0.0.1:4321,server,nowait`
- 另开 QEMU monitor `-monitor telnet:127.0.0.1:4322,server,nowait`（可输入 `quit` 退出虚拟机）
- QEMU 命令末尾加 `&` **后台运行**，脚本立即返回，宿主终端可自由输入

**现在的标准操作流程**：
```bash
# 终端 A：启动 QEMU（后台）
cd /d/linux-study/linux/file/uboot-start
./start_uboot.sh          # 打印 PID，监听 4321(串口)/4322(monitor)

# 终端 B：连串口控制台，进 ARM Linux 跑 demo
telnet 127.0.0.1 4321
./lvgl_demo               # 前台跑：LVGL 界面在 gtk 窗口，按钮可用

# 想让控制台继续可用，可后台运行：
./lvgl_demo &
```

**退出方式**（分清两者，别再误杀虚拟机）：
- **只关 demo、保留虚拟机**：在 telnet 串口控制台里按 `Ctrl+C`（只杀 demo 进程），或 `pkill lvgl_demo`；也可 `./lvgl_demo &` 后台运行让控制台继续可用。
- **关掉虚拟机**：在 monitor（`telnet 127.0.0.1 4322`）里输入 `quit`，或在宿主终端 `pkill -f qemu-system-arm`。
- 由于 QEMU 已后台运行，**宿主终端（启动 QEMU 的那个）按 Ctrl+C 现在只影响宿主 shell，不会再杀 QEMU**——这正是本次改动的目的。

---

*记录于 2026-10-12，问题已闭环：重编最新代码后，鼠标（event1）与键盘回车（event0）点击均正常工作；QEMU 改后台 + telnet 串口后，控制台可自由输入、Ctrl+C 不再误杀虚拟机。*
