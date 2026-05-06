# 1. 创建镜像文件
dd if=/dev/zero of=full_sd.img bs=1M count=1024

# 2. 分区：P1 为 128M FAT32, P2 为剩余 Ext4
printf "n\np\n1\n\n+128M\nt\nc\nn\np\n2\n\n\nw\n" | fdisk full_sd.img

# 3. 格式化
sudo losetup -Pf full_sd.img
LOOP_DEV=$(losetup -j full_sd.img | cut -d: -f1)

sudo mkfs.vfat -F 32 ${LOOP_DEV}p1
sudo mkfs.ext4 ${LOOP_DEV}p2

# 4. 写入引导文件 (P1)
mkdir -p mnt_p1
sudo mount ${LOOP_DEV}p1 mnt_p1
sudo cp ./zImage mnt_p1/
sudo cp ./vexpress-v2p-ca9.dtb mnt_p1/main.dtb
sudo cp my_overlay.dtbo mnt_p1/
sudo cp rootfs.uimg mnt_p1/
sudo umount mnt_p1



# 6. 清理
sudo losetup -d $LOOP_DEV

