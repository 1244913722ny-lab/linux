# 这里的路径改为你之前编译的内核源码路径
KERN_DIR = ../linux-5.0.17 

obj-m += hello_driver.o
obj-m += test_dev.o
obj-m +=my_driver.o
obj-m +=my_interrupt_driver.o

all:
	make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- -C $(KERN_DIR) M=$(PWD) modules

clean:
	make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- -C $(KERN_DIR) M=$(PWD) clean

