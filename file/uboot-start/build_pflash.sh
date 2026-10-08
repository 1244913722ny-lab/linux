#!/bin/bash
set -e
UBOOT_BIN="./u-boot.bin"
SD_PATH="./full_sd.img"
FLASH_IMG="./flash0.bin"


echo "[1/2] 制作128MB Flash镜像..."
dd if=/dev/zero of="$FLASH_IMG" bs=1M count=64 status=none
echo "[2/2] 烧写U‑Boot bin到flash偏移0..."
dd if="$UBOOT_BIN" of="$FLASH_IMG" conv=notrunc bs=512 status=none