# boot.cmd 内容
tftp 0x60000000 zImage
tftp 0x61000000 main.dtb
tftp 0x62000000 my_overlay.dtbo
tftp 0x64000000 rootfs.uimg

fdt addr 0x61000000
fdt resize 8192
fdt apply 0x62000000

setenv bootargs "console=ttyAMA0 rdinit=/sbin/init rw"
bootz 0x60000000 0x64000000 0x61000000

