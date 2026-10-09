#!/bin/sh
IMG=/mnt/d/linux-study/linux/file/uboot-start/full_sd.img
KO_DIR=/mnt/d/linux-study/linux/file/driver/out/ko
MNT=/mnt/d/linux-study/linux/file/uboot-start/mnt_p1

sudo losetup -Pf "$IMG"
LOOP=$(losetup -j "$IMG" | cut -d: -f1)
mkdir -p "$MNT"
sudo mount ${LOOP}p1 "$MNT"          # 若提示未知类型，加 -t vfat
sudo cp "$KO_DIR"/*.ko "$MNT"/
ls -l "$MNT"/*.ko                    # 确认写进去了
sudo umount "$MNT"
sudo losetup -d "$LOOP"
