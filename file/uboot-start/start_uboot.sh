UBOOT_PATH="./u-boot"
SD_PATH="./full_sd.img"

# 说明（重要）：
#  - 串口控制台(-serial)与 QEMU monitor 都改接本地 TCP(telnet)，
#    不再占用宿主终端 stdio —— 宿主终端按 Ctrl+C 不会再杀掉 QEMU。
#  - QEMU 以 & 后台方式启动，本脚本立即返回，宿主终端可继续自由输入。
#  - LVGL 界面仍经 -display gtk 显示在独立窗口（PL111 framebuffer）。
#  - vexpress-a9 没有 USB 总线，不能加 -device usb-tablet / -usb
#    （会报 "No 'usb-bus' bus found"），只用 PL050 的 PS/2 键鼠。
#  - 鼠标在 gtk 窗口里抓取(Ctrl+Alt+G)后，PS/2 相对坐标可被 LVGL 读取(EV_REL)；
#    键盘回车被 demo 映射为"点击按钮"，是最可靠的演示方式。
#  - 网络：vexpress-a9 板载 LAN9118 网卡。下面用 QEMU 用户态网络(user-mode
#    networking)给它接一个 NAT 网卡，板子起来后可在 rootfs 里用 udhcpc 自动
#    拿到 10.0.2.15/网关 10.0.2.2/DNS 10.0.2.3，从而主动连外网
#    （适合做 TCP 连通测试 demo；外网不能连回板子，对 client 模式足够）。
#    若你的 QEMU 版本不接受 -device lan9118，可改用旧式写法：
#      -net nic,model=lan9118 -net user

qemu-system-arm \
    -M vexpress-a9 \
    -m 512M \
    -kernel $UBOOT_PATH \
    -sd $SD_PATH \
    -display gtk \
    -serial telnet:127.0.0.1:4321,server,nowait \
    -monitor telnet:127.0.0.1:4322,server,nowait \
    &

QEMU_PID=$!
echo "QEMU 已在后台启动 (PID=$QEMU_PID)"
echo "  串口控制台:  telnet 127.0.0.1 4321   （进去后运行 ./lvgl_demo）"
echo "  QEMU monitor: telnet 127.0.0.1 4322   （输入 quit 可退出虚拟机）"
echo "  停止 QEMU:   pkill -f qemu-system-arm   或 在 monitor 里输 quit"

 # echo -e "\033[?25l" > /dev/tty1  关闭光标显示
 # echo -e "\033[H\033[2J" > /dev/tty1  清除屏幕

 

    

