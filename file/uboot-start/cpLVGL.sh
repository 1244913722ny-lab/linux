#!/bin/sh
IMG=/mnt/d/linux-study/linux/file/uboot-start/full_sd.img
APP_DIR=/mnt/d/linux-study/linux/file/app/build/bin
MNT=/mnt/d/linux-study/linux/file/uboot-start/mnt_p1
FONT_DIR=/mnt/d/linux-study/linux/file/uboot-start/zw.ttf

sudo losetup -Pf "$IMG"
LOOP=$(losetup -j "$IMG" | cut -d: -f1)
mkdir -p "$MNT"
sudo mount ${LOOP}p1 "$MNT"          # 若提示未知类型，加 -t vfat
sudo cp "$APP_DIR"/* "$MNT"/
sudo cp "$FONT_DIR" "$MNT"/
ls -l "$MNT"/*                    # 确认写进去了
sudo umount "$MNT"
sudo losetup -d "$LOOP"