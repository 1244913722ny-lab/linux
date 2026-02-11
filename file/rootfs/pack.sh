cd ./build
find . | cpio -o --format=newc > ../rootfs.img

cd ..
mkimage -A arm -O linux -T ramdisk -C gzip \
        -n "ConfigFS_RootFS" \
        -d rootfs.img rootfs.uimg
