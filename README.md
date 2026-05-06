# 嵌入式 Linux 开发指南（QEMU vexpress_ca9 平台）

使用 QEMU 模拟 ARM 平台，通过 U-Boot 加载 Linux 内核并启动 BusyBox 根文件系统。

---

## 0. 工具链安装

嵌入式开发需要交叉编译工具链，将代码编译为目标架构（ARM）的可执行文件。

### Ubuntu/Debian 安装

```bash
# 安装 ARM 交叉编译工具链
sudo apt update
sudo apt install gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf

# 安装 QEMU 模拟器
sudo apt install qemu-system-arm

# 安装设备树编译器（DTC）
sudo apt install device-tree-compiler

# 安装 mkimage（U-Boot 镜像打包工具）
sudo apt install u-boot-tools

# 安装 mtools（用于操作 FAT 镜像）
sudo apt install mtools
```

### 验证安装

```bash
arm-linux-gnueabihf-gcc --version    # 确认交叉编译器可用
qemu-system-arm --version            # 确认 QEMU 可用
dtc --version                         # 确认设备树编译器可用
mkimage --version                     # 确认 U-Boot 工具可用
```

### 源码编译工具链（可选）

如果需要自定义工具链，可使用 [crosstool-ng](https://crosstool-ng.github.io/) 构建：

```bash
sudo apt install crosstool-ng
ct-ng arm-linux-gnueabihf
ct-ng build
```

> **注意**：本文统一使用 `arm-linux-gnueabihf-` 前缀（hard-float，硬件浮点），适用于 vexpress_ca9 平台。

---

## 0.1 Ubuntu 20.04 编译环境搭建

Ubuntu 20.04 是本文档各组件的最佳编译环境，其默认 GCC 9 完全兼容老版本内核源码，**无需**任何额外补丁或兼容性参数。

### 一键安装所有依赖

```bash
sudo apt update
sudo apt install -y \
    gcc-arm-linux-gnueabihf \
    g++-arm-linux-gnueabihf \
    qemu-system-arm \
    device-tree-compiler \
    u-boot-tools \
    mtools \
    build-essential \
    bison \
    flex \
    libssl-dev \
    bc \
    cpio \
    kmod \
    lzop
```

各包用途说明：

| 包名 | 用途 |
|------|------|
| `gcc-arm-linux-gnueabihf` / `g++-arm-linux-gnueabihf` | ARM 交叉编译工具链 |
| `qemu-system-arm` | ARM 架构 QEMU 模拟器 |
| `device-tree-compiler` (dtc) | 设备树编译器 |
| `u-boot-tools` (mkimage) | U-Boot 镜像打包工具 |
| `mtools` | 操作 FAT 格式镜像（虚拟 SD 卡） |
| `build-essential` | 主机编译基础工具（gcc、make 等） |
| `bison` / `flex` | 语法分析器生成器（内核和 dtc 编译依赖） |
| `libssl-dev` | 内核签名和证书功能依赖 |
| `bc` | 内核编译时计算版本号 |
| `cpio` | 打包 rootfs 归档 |
| `kmod` | 内核模块加载工具 |
| `lzop` | 内核压缩算法支持 |

### 验证环境

```bash
# 交叉编译器
arm-linux-gnueabihf-gcc --version
# 预期输出：gcc (Ubuntu 9.3.0-17ubuntu1~20.04) 9.3.0

# QEMU
qemu-system-arm --version

# 主机 GCC（编译 host 工具用）
gcc --version
# 预期输出：gcc (Ubuntu 9.3.0-17ubuntu1~20.04) 9.3.0

# DTC
dtc --version

# mkimage
mkimage --version

# mtools
mcopy --version
```

### Ubuntu 20.04 的编译优势

因为 GCC 9 的默认行为是 `-fcommon`，且 binutils 版本兼容旧语法，本文档中所有组件（U-Boot、Linux 内核、BusyBox）均可**直接编译**，无需以下 workaround：

- ❌ 不需要 `HOSTCFLAGS="-fcommon"`（GCC 10+ 才需要）
- ❌ 不需要 `KCFLAGS="-march=armv7-a"`（binutils 版本足够旧）
- ❌ 不需要 Linaro 工具链（系统自带的交叉编译器即可）
- ❌ 不需要处理 `#` 段标志语法问题

直接按文档各章节的编译命令执行即可。

### 磁盘空间预估

| 组件 | 源码 | 编译产物 |
|------|------|----------|
| U-Boot | ~130MB | ~50MB |
| Linux 内核 | ~1.1GB | ~3GB |
| BusyBox | ~8MB | ~20MB |
| **总计** | ~1.3GB | ~3.1GB |

建议预留 **10GB** 以上可用空间。

---

## ⚠️ Ubuntu 24.04 编译兼容性问题

Ubuntu 24.04 默认 GCC 版本为 13+、binutils 为 2.42+，编译较老版本的 Linux 内核（如 5.x 及以下）时会出现多个兼容性错误。Ubuntu 20.04（GCC 9）不存在这些问题。

### 问题一：`yylloc` 多重定义错误

```
/usr/bin/ld: scripts/dtc/dtc-parser.tab.o:(.bss+0x20): multiple definition of `yylloc'
```

**原因**：GCC 10 起默认从 `-fcommon` 改为 `-fno-common`，不允许同一全局变量在多个编译单元中定义。

**修复**：编译时传入 `HOSTCFLAGS="-fcommon"`：

```bash
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- HOSTCFLAGS="-fcommon" zImage dtbs -j4
```

### 问题二：`selected processor does not support 'dmb ish' in ARM mode`

```
错误：selected processor does not support `dmb ish' in ARM mode
错误：selected processor does not support `isb ' in ARM mode
错误：selected processor does not support `cpsid i' in ARM mode
```

**原因**：新版 binutils 的汇编器 `as` 默认目标架构过低，不支持 ARMv7 指令（`dmb`、`isb`、`cpsid` 等均为 ARMv6K/ARMv7 指令）。

**修复**：强制指定 ARMv7 架构：

```bash
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- KCFLAGS="-march=armv7-a" zImage dtbs -j4
```

> 注意：`KCFLAGS` 仅对内核代码生效，主机工具（如 dtc）需用 `HOSTCFLAGS`。

### 问题三：`junk at end of line, first unrecognized character is '#'`

```
arch/arm/mm/proc-v7.S:640: 错误：junk at end of line, first unrecognized character is `#'
```

**原因**：新版 GNU 汇编器（GAS）不再接受 `#` 作为段标志的前缀（如 `#alloc`），需改为 `%alloc`。

**修复**：需同时指定旧版 GCC **和** 旧版汇编器，因为 `CC` 只控制 C 编译器，汇编器 `as` 是独立的 binutils 组件，版本不一定配套。

#### 推荐方案：使用 Linaro 工具链（GCC + binutils 版本配套）

```bash
# 下载 Linaro GCC 7.5（自带配套的 as、ld 等 binutils）
wget https://releases.linaro.org/components/toolchain/binaries/7.5-2019.12/arm-linux-gnueabihf/gcc-linaro-7.5.0-2019.12-x86_64_arm-linux-gnueabihf.tar.xz

tar xf gcc-linaro-7.5.0-2019.12-x86_64_arm-linux-gnueabihf.tar.xz

# 使用 Linaro 工具链编译
make ARCH=arm \
  CROSS_COMPILE=$(pwd)/gcc-linaro-7.5.0-2019.12-x86_64_arm-linux-gnueabihf/bin/arm-linux-gnueabihf- \
  HOSTCFLAGS="-fcommon" \
  zImage dtbs -j4
```

> **注意**：即使使用 Linaro 工具链，仍需 `HOSTCFLAGS="-fcommon"`。因为 `dtc`、`fixdep` 等是**主机工具**，由系统的 GCC（Ubuntu 24.04 的 GCC 13+）编译，不受 `CROSS_COMPILE` 影响。


### 总结

| 问题 | 现象 | 原因 |
|------|------|------|
| `yylloc` 多重定义 | `multiple definition of 'yylloc'` | GCC 10+ 默认 `-fno-common` |
| ARMv7 指令不支持 | `does not support 'dmb ish'` | binutils 汇编器默认架构过低 |
| `#` 段标志语法错误 | `junk at end of line, '#'` | 新版 GAS 不接受旧语法 |
| GCC 和 as 版本不配套 | 指定了 `CC=gcc-9` 仍报错 | `as` 是独立组件，不受 `CC` 控制 |

**核心原则**：旧内核源码需搭配旧版完整工具链（GCC + binutils 配套），或升级内核源码以兼容新工具链。推荐使用 Linaro 工具链或 Docker 隔离编译环境。

---

## 三大件总览

嵌入式 Linux 系统由三个核心组件构成：

| 组件 | 作用 | 本文产物 |
|------|------|----------|
| **U-Boot** (Bootloader) | 初始化硬件、加载内核、传递设备树 | `u-boot` 可执行文件 |
| **Linux Kernel** | 操作系统核心，管理硬件和进程 | `zImage` + `vexpress-v2p-ca9.dtb` |
| **Rootfs** (根文件系统) | 提供用户空间程序和目录结构 | `rootfs.uimg` |

---

## 1. 编译 U-Boot（Bootloader）

U-Boot 是嵌入式 Linux 最常用的引导加载程序，负责初始化硬件并加载内核。

### 1.1 修改启动命令

打开 U-Boot 源码头文件：`include/configs/vexpress_common.h`

找到或添加 `CONFIG_BOOTCOMMAND` 定义：

```c
#undef CONFIG_BOOTCOMMAND
#define CONFIG_BOOTCOMMAND \
    "fatload mmc 0:1 0x60000000 zImage; " \
    "fatload mmc 0:1 0x61000000 main.dtb; " \
    "fatload mmc 0:1 0x62000000 my_device.dtbo; " \
    "fdt addr 0x61000000; " \
    "fdt resize 8192; " \
    "fdt apply 0x62000000; " \
    "setenv bootargs 'console=ttyAMA0 root=/dev/mmcblk0 rw'; " \
    "bootz 0x60000000 - 0x61000000"
```

**命令解析：**
- `fatload mmc 0:1` — 从 SD 卡 FAT 分区加载文件到内存
- `zImage` → `0x60000000` — 内核镜像加载地址
- `main.dtb` → `0x61000000` — 设备树加载地址
- `my_device.dtbo` → `0x62000000` — 设备树插件加载地址
- `fdt apply` — 应用设备树 overlay
- `bootz` — 启动内核

### 1.2 编译

```bash
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- vexpress_ca9x4_defconfig
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- -j$(nproc)
```

> **产物**：根目录下的 `u-boot` 可执行文件。

---

## 2. 编译 Linux 内核

### 2.1 生成并配置内核

首先基于 vexpress 默认配置生成 `.config`：

```bash
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- vexpress_defconfig
```

修改 `scripts/Makefile.lib`，确保 DTC_FLAGS 包含 `-@`（启用设备树符号，overlay 必需）。

在生成的 `.config` 中添加以下配置：

```
CONFIG_OF=y
CONFIG_OF_OVERLAY=y
CONFIG_CONFIGFS_FS=y
```

**方式一：直接编辑 `.config`**

```bash
echo "CONFIG_OF=y" >> .config
echo "CONFIG_OF_OVERLAY=y" >> .config
echo "CONFIG_CONFIGFS_FS=y" >> .config
```

**方式二：使用 `menuconfig` 图形化配置（推荐）**

```bash
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- menuconfig
```

在菜单中依次找到并开启：

```
Device Drivers --->
  [*] Device Tree and Open Firmware support --->
    [*] Device Tree overlays                          # CONFIG_OF_OVERLAY

File systems --->
  Pseudo filesystems --->
    [*] Userspace-driven configuration filesystem      # CONFIG_CONFIGFS_FS
```

> `CONFIG_OF` 和 `CONFIG_OF_OVERLAY` 通常在开启 Device Tree 支持后自动选中。`menuconfig` 会自动处理依赖关系，比手动编辑更可靠。

> **说明**：`CONFIG_OF_CONFIGFS` 在主线 4.4+ 后已移除，通常由第三方补丁提供。如果只是在 U-Boot 中加载 overlay，只需开启 `CONFIG_OF_OVERLAY` 即可。

### 2.2 编译

```bash
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- vexpress_defconfig
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- zImage dtbs -j$(nproc) HOSTCFLAGS="-fcommon"
```

> **为什么需要 `HOSTCFLAGS="-fcommon"`？**
>
> GCC 10 起将默认行为从 `-fcommon` 改为 `-fno-common`，即不再允许多个编译单元对同一全局变量进行"暂定定义"（tentative definition）。Linux 5.0.17 等较老版本的内核源码中存在这类写法，使用新版 GCC 编译时会报 `multiple definition` 链接错误。
>
> 通过 `HOSTCFLAGS="-fcommon"` 将该标志传给宿主机编译器（用于编译内核构建过程中在主机上运行的工具，如 `fixdep`、`kallsyms`、`modpost` 等），可兼容老代码，避免报错。
>
> 如果你的 GCC 版本 < 10，则不需要此参数。

> **产物**：
> - `arch/arm/boot/zImage` — 内核镜像
> - `arch/arm/boot/dts/vexpress-v2p-ca9.dtb` — 设备树二进制

---

## 3. 构建 Rootfs（根文件系统）

根文件系统是 Linux 启动后用户空间的运行环境，包含必要的目录结构、设备节点和初始化脚本。本文使用 **BusyBox** 构建最小化 rootfs。

### 3.1 编译 BusyBox

```bash
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- defconfig
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- menuconfig
```

在菜单中务必开启**静态编译**（避免运行时依赖 glibc）：

```
Settings --->
  [*] Build static binary (no shared libs)
```

编译并安装：

```bash
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- install -j$(nproc)
```

> **产物**：`_install/` 目录，包含 BusyBox 提供的基本命令（`ls`, `cat`, `sh` 等）。

### 3.2 构建 rootfs 目录结构

在 `_install` 基础上补充 Linux 启动必需的系统目录和初始化脚本：

```bash
# 进入 BusyBox 安装目录
cd _install

# 创建目录结构
mkdir -p dev etc lib proc sys tmp root var/log

# 创建设备节点（必须用 root 权限，mknod 需要特权）
sudo mknod dev/console c 5 1
sudo mknod dev/null c 1 3

# 确认创建正确
ls -la dev/console dev/null
# 预期输出：
# crw-r--r-- 1 root root 5, 1 ... dev/console
# crw-r--r-- 1 root root 1, 3 ... dev/null
```

> **注意**：以上命令必须在 `_install/` 目录下执行。`dev/console` 和 `dev/null` 是相对于当前目录的路径，不是主机的 `/dev/console`。`mknod` 需要 `sudo` 因为创建设备节点是特权操作。

### 3.3 创建初始化脚本

**`etc/inittab`** — init 进程配置：

```bash
cat > etc/inittab << 'EOF'
::sysinit:/etc/init.d/rcS
::askfirst:-/bin/sh
::restart:/sbin/init
::ctrlaltdel:/sbin/reboot
EOF
```

各字段说明：

| 字段 | 含义 |
|------|------|
| `::sysinit:/etc/init.d/rcS` | 系统启动时执行 rcS（挂载 proc、sys 等） |
| `::askfirst:-/bin/sh` | 启动完弹出 shell，提示按回车激活（`-` 表示 login shell） |
| `::restart:/sbin/init` | 收到 restart 信号时重新执行 init |
| `::ctrlaltdel:/sbin/reboot` | Ctrl+Alt+Del 触发 reboot |

**`etc/init.d/rcS`** — 系统启动脚本：

```bash
mkdir -p etc/init.d
cat > etc/init.d/rcS << 'EOF'
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
echo /sbin/mdev > /proc/sys/kernel/hotplug
/sbin/mdev -s
EOF
```

赋予执行权限：

```bash
chmod +x etc/init.d/rcS
```

### 3.4 打包 rootfs

确保当前目录是 `_install/`（包含 `bin/`、`dev/`、`etc/` 等目录）：

```bash
# 确认当前在 _install 目录
ls bin dev etc
# 应该能看到 BusyBox 的命令和刚才创建的设备节点

# 将 rootfs 打包为 cpio 归档
find . | cpio -o --format=newc > ../rootfs.img

# 回到上级目录
cd ..

# 转换为 U-Boot 可识别的格式
mkimage -A arm -O linux -T ramdisk -C gzip -n "BusyBox RootFS" -d rootfs.img rootfs.uimg
```

> **产物**：`rootfs.uimg` — U-Boot 可直接加载到内存的根文件系统镜像。

---

## 3.5 内核模块（可选）

如果编译了自定义驱动（`.ko` 文件），需要将它们放入 rootfs 中，系统启动后才能加载。

### 放置模块

将编译好的 `.ko` 文件复制到 rootfs 的 `/lib/modules/` 目录：

```bash
mkdir -p lib/modules
cp /path/to/hello_driver.ko lib/modules/
cp /path/to/my_driver.ko lib/modules/
```

> **注意**：模块必须用同一套内核源码和配置编译，版本不匹配会导致 `insmod` 失败。

### 在 QEMU 中操作模块

系统启动后，在 shell 中可以对模块进行加载、查看、卸载：

```bash
# 加载模块
insmod hello_driver.ko

# 查看是否加载成功
lsmod | grep hello

# 查看内核日志（驱动的 printk 输出）
dmesg | tail

# 卸载模块
rmmod hello_driver
```

### 常用模块命令

| 命令 | 作用 |
|------|------|
| `insmod xxx.ko` | 加载指定模块（不处理依赖） |
| `modprobe xxx` | 加载模块并自动处理依赖（需要 `depmod` 生成依赖表） |
| `rmmod xxx` | 卸载模块 |
| `lsmod` | 列出当前已加载的所有模块 |
| `modinfo xxx.ko` | 查看模块信息（作者、描述、许可证等） |
| `dmesg` | 查看内核日志，驱动的 `printk` 输出在这里 |

---

## 4. 设备树插件（Overlay）

设备树 overlay 允许在运行时动态修改设备树，无需重新编译内核。

### 4.1 编写插件

创建 `my_device.dts`：

```dts
/dts-v1/;
/plugin/;

&{/smb/motherboard/v2m_timer0} {
    status = "okay";
    my-property = "hello-overlay";
};
```

### 4.2 编译

```bash
dtc -@ -I dts -O dtb -o my_device.dtbo my_device.dts
```

> **产物**：`my_device.dtbo` — 设备树 overlay 二进制。

---

## 5. 制作虚拟 SD 卡镜像

将所有产物打包为 FAT 格式的虚拟 SD 卡，供 U-Boot 读取：

```bash
# 创建 64MB 空白镜像
dd if=/dev/zero of=sdcard.img bs=1M count=64

# 格式化为 FAT
mkfs.vfat sdcard.img

# 将文件放入镜像
mcopy -i sdcard.img zImage ::/zImage
mcopy -i sdcard.img vexpress-v2p-ca9.dtb ::/main.dtb
mcopy -i sdcard.img my_device.dtbo ::/my_device.dtbo
mcopy -i sdcard.img rootfs.uimg ::/rootfs.uimg
```

---

## 6. 启动 QEMU

确保已生成以下文件：

| 文件 | 来源 |
|------|------|
| `u-boot` | U-Boot 编译产物 |
| `zImage` | Linux 内核编译产物 |
| `vexpress-v2p-ca9.dtb` | Linux 设备树编译产物 |
| `my_device.dtbo` | 设备树 overlay 编译产物 |
| `rootfs.uimg` | BusyBox rootfs 打包产物 |
| `sdcard.img` | 虚拟 SD 卡镜像 |

执行启动脚本即可进入系统：

```bash
# start_uboot.sh 内容示例：
qemu-system-arm \
    -M vexpress-a9 \
    -m 512M \
    -kernel u-boot \
    -sd sdcard.img \
    -nographic
```

---

## 附录：常见问题

### Q: 编译报错找不到交叉编译器
确认工具链已安装且在 PATH 中：`which arm-linux-gnueabihf-gcc`

### Q: U-Boot 启动后找不到文件
检查 SD 卡镜像中的文件名是否与 `CONFIG_BOOTCOMMAND` 中一致（区分大小写）。

### Q: 内核 panic 无法挂载 rootfs
确认 `bootargs` 中 `root=/dev/mmcblk0` 或使用 initrd 方式加载 rootfs.uimg。

### Q: 设备树 overlay 加载失败
确认内核开启了 `CONFIG_OF_OVERLAY=y`，且 DTC 编译时带 `-@` 参数。

