# 创建一个 64M 的空镜像
dd if=/dev/zero of=sdcard.img bs=1M count=64
# 2. 分区 (按顺序输入: n -> p -> 1 -> 回车 -> 回车 -> t -> c -> w)
printf "n\np\n1\n\n\nt\nc\nw\n" | fdisk sdcard.img
# 3. 挂载镜像为环回设备 (-P 会自动探测分区)
sudo losetup -Pf sdcard.img


# 将文件考入镜像 (可以使用 mtools 方便操作)
mcopy -i sdcard.img ./zImage ::/zImage
mcopy -i sdcard.img ./vexpress-v2p-ca9.dtb ::/main.dtb
#mcopy -i sdcard.img my_overlay.dtbo ::/my_overlay.dtbo

