KERN_DIR = ../linux-5.0.17
CROSS_COMPILE = /mnt/d/linux-study/linux/gcc-linaro-7.5.0-2019.12-x86_64_arm-linux-gnueabihf/bin/arm-linux-gnueabihf-
OUT_KO := $(PWD)/out/ko

all:
	mkdir -p $(OUT_TMP) $(OUT_KO)
	make ARCH=arm CROSS_COMPILE=$(CROSS_COMPILE) -C $(KERN_DIR)  M=$(PWD) modules
	# 将ko复制到out/ko
	cp *.ko $(OUT_KO)/
	# 等待复制完成
	sync
	sleep 1
	# 手动删除源码目录残留所有编译产物
	rm -f *.o *.ko *.mod.c *.mod *.symvers *.cmd *.order 
	rm -f .[!.]*.cmd

clean:
	make ARCH=arm CROSS_COMPILE=$(CROSS_COMPILE) -C $(KERN_DIR) O=$(OUT_TMP) M=$(PWD) clean
	rm -f $(OUT_KO)/*.ko
	rm -f *.o *.ko *.mod.c *.mod *.symvers *.cmd *.order 
	rm -f .[!.]*.cmd