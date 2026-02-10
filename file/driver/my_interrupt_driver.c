#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h> // 必须包含：中断相关 API
#include <linux/mod_devicetable.h>  // 必须包含：定义了 struct of_device_id
#include <linux/of.h>               // 建议包含：处理设备树节点的常用函数
#include <linux/irq.h>
static int my_irq;
static DECLARE_WAIT_QUEUE_HEAD(my_wait_queue); // 2. 定义并初始化等待队列头
static int condition = 0; // 唤醒条件，0 为等待，1 为苏
// --- 中断处理函数 (Top Half) ---
static irqreturn_t my_irq_handler(int irq, void *dev_id) {
    // 这里就像 STM32 的中断服务函数，动作要快！
    printk("内核检测到中断！中断号: %d\n", irq);
    condition = 1;               // 3. 改变条件
    wake_up_interruptible(&my_wait_queue); // 4. 唤醒在队列里睡觉的进程
    return IRQ_HANDLED; // 告诉内核：我已经处理过这个中断了
}

static int my_probe(struct platform_device *pdev) {
    int ret;
    printk("中断驱动匹配成功！\n");

    // 1. 从设备树节点中获取中断号
    my_irq = platform_get_irq(pdev, 0);
    if (my_irq < 0) return my_irq;

    // 2. 申请中断 (类似 HAL_GPIO_EXTI_Callback)
    // 参数：中断号, 处理函数, 标志, 名称, 传给处理函数的私有数据
    ret = request_irq(my_irq, my_irq_handler, IRQF_TRIGGER_RISING, "my_vdev_irq", NULL);
    if (ret) {
        printk("无法申请中断 %d\n", my_irq);
        return ret;
    }
    
    printk("成功申请中断号: %d\n", my_irq);
    my_irq_handler(my_irq, NULL);
    return 0;
}
// --- 驱动 Read 函数 ---
static ssize_t dev_read(struct file *file, char __user *buf, size_t len, loff_t *offset) {
    printk("APP：我打算读数据，但还没中断，我要睡了...\n");
    
    // 5. 进入休眠。如果 condition 为 0 就一直睡，直到被 wake_up 且 condition 为 1
    if (wait_event_interruptible(my_wait_queue, condition != 0)) {
        return -ERESTARTSYS; // 如果被信号（如 Ctrl+C）中断，返回这个
    }

    // 6. 醒来后执行的操作
    condition = 0; // 重置条件，下次读还要睡
    printk("APP：喔！我被中断叫醒了，拿到了数据！\n");
    
    copy_to_user(buf, "Interrupt Data", 15);
    return 15;
}

static int my_remove(struct platform_device *pdev) {
    free_irq(my_irq, NULL); // 退出时必须释放
    printk("中断驱动已卸载\n");
    return 0;
}

static const struct of_device_id my_of_match[] = {
    {.compatible = "ny,irq-vdev" },
    { }
};

static struct platform_driver my_irq_driver = {
    .driver = { .name = "my_irq_drv", .of_match_table = my_of_match },
    .probe = my_probe,
    .remove = my_remove,
};

module_platform_driver(my_irq_driver);
MODULE_LICENSE("GPL");
