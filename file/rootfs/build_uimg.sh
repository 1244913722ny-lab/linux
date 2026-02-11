# 参数说明：
# -A arm: ARM架构
# -O linux: 操作系统
# -T ramdisk: 镜像类型为内存盘
# -C gzip: 压缩方式
# -d: 原始文件
# -n: 给镜像起个名字
mkimage -A arm -O linux -T ramdisk -C gzip \
        -n "ConfigFS_RootFS" \
        -d rootfs.img rootfs.uimg

