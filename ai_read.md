# 先连接到wsl的ssh
 连接密码是19971226
```bash
ssh hp@172.21.148.252 -p 22
```
# 编译app
 在/mnt/d/linux-study/linux/file/app/Makefile路径执行make all指令编译
```bash
make all
```
# 把编译文件copy到目标机器上
 在/mnt/d/linux-study/linux/file/uboot-start目录执行cpLVGL.sh脚本
```bash
./cpLVGL.sh
```
# 运行qemu模拟的arm-linux机器
 在/mnt/d/linux-study/linux/file/uboot-start目录执行start_uboot.sh脚本
```bash
./start_uboot.sh
```
此时就运行起了qemu模拟的arm-linux机器
# 连接到qemu模拟的arm-linux机器
```bash
telnet 127.0.0.1 4321
```
此时就连接到了qemu模拟的arm-linux机器，然后在qemu模拟的arm-linux机器上进入./mnt/sdcard目录执行lvgl_demo
```bash
./lvgl_demo
```
此时就运行起了lvgl_demo，lvgl_demo会在qemu模拟的arm-linux机器上运行


