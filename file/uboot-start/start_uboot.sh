UBOOT_PATH="./u-boot"
SD_PATH="./full_sd.img"


qemu-system-arm \
    -M vexpress-a9 \
    -m 512M \
    -kernel $UBOOT_PATH \
    -sd $SD_PATH \
    -nographic 
    

