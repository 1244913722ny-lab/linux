# linux
学习使用qemu使用uboot加载linux

# 环境准备
需要准备三个核心组件：U-Boot,Linux 内核,busybox,rootfs以及交叉编译链（通常为 arm-linux-gnueabihhf-）

# 编译 U-Boot
1. 修改配置文件
打开 U-Boot 源码中的头文件：
include/configs/vexpress_common.h （或者在该目录下搜索 CONFIG_BOOTCOMMAND）
2. 定义 CONFIG_BOOTCOMMAND
在文件中找到 CONFIG_BOOTCOMMAND 的定义，或者在文件末尾添加以下内容。我们将你的命令串联起来（用 ; 分隔）：
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

执行编译:
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- vexpress_ca9x4_defconfig #生成配置
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- -j4 #编译

# 编译 Linux 内核 (5.0.17)
修改 Linux 源码中的 scripts/Makefile.lib，确保 DTC_FLAGS 包含 -@。
直接修改 .config，添加：
CONFIG_OF=y
CONFIG_OF_OVERLAY=y
CONFIG_CONFIGFS_FS=y
## 注意：CONFIG_OF_CONFIGFS 在主线 4.4+ 后已移除，通常由第三方补丁提供
## 如果你只是在 U-Boot 中加载插件，只需开启 CONFIG_OF_OVERLAY 即可

编译：
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- vexpress_defconfig
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- zImage dtbs -j4
## 产物：arch/arm/boot/zImage 和 arch/arm/boot/dts/vexpress-v2p-ca9.dtb

# 设备树插件 (Overlay) 准备
创建一个测试插件 my_device.dts：
/dts-v1/;
/plugin/;
&{/smb/motherboard/v2m_timer0} {
    status = "okay";
    my-property = "hello-overlay";
};
编译插件：
dtc -@ -I dts -O dtb -o my_device.dtbo my_device.dts

# 编译 BusyBox
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- defconfig
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- menuconfig
在菜单中务必开启 静态编译（方便在没有 glibc 的环境下运行）：
Settings --->
[*] Build static binary (no shared libs)
编译并安装：
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- install -j4  # 默认安装到 ./_install 目录


# 构建 rootfs 目录结构
需要在 _install 基础上补充 Linux 启动必需的系统目录和初始化脚本
mkdir -p dev etc lib proc sys tmp root var/log
然后把 _install目录下的东西全部复制到rootfs文件夹下
创建必要的初始化脚本：
创建 etc/inittab：
::sysinit:/etc/init.d/rcS
::askfirst:-/bin/sh
::restart:/sbin/init
::ctrlaltdel:/sbin/reboot
创建 etc/init.d/rcS（系统启动脚本）:
mkdir -p etc/init.d
touch etc/init.d/rcS
chmod +x etc/init.d/rcS
在rcS里写入：
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
echo /sbin/mdev > /proc/sys/kernel/hotplug 
/sbin/mdev -s
使用find . | cpio -o --format=newc > ./rootfs.img把rootfs 压成一个压缩包，然后使用mkimage 转换格式：
mkimage -A arm -O linux -T ramdisk -C gzip -n "BusyBox RootFS" -d rootfs.img rootfs.uimg
由 U-Boot 直接加载到内存

# 制作虚拟 SD 卡镜像
dd if=/dev/zero of=sdcard.img bs=1M count=64
mkfs.vfat sdcard.img
## 将 zImage, main.dtb, my_device.dtbo 放入镜像
mcopy -i sdcard.img zImage ::/zImage
mcopy -i sdcard.img vexpress-v2p-ca9.dtb ::/main.dtb
mcopy -i sdcard.img my_device.dtbo ::/my_device.dtbo
mcopy -i sdcard.img rootfs.uimg ::/rootfs.uimg

# 启动qemu
目前已经得到了zImage，vexpress-v2p-ca9.dtb，my_device.dtbo，rootfs.uimg，执行start_uboot.sh即可成功进入。


