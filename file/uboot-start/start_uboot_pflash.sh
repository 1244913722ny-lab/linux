#!/bin/bash
set -e
UBOOT_BIN="./u-boot.bin"
SD_PATH="./full_sd.img"
FLASH_IMG="./flash0.bin"

echo "启动QEMU pflash(可写)+SD"
qemu-system-arm \
    -M vexpress-a9 \
    -m 512M \
    -drive if=pflash,format=raw,file="$FLASH_IMG",readonly=off \
    -drive format=raw,file="$SD_PATH",if=sd \
    -nographic
