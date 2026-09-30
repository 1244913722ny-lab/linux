#include <linux/module.h>
#include <linux/kernel.h>

// 驱动加载时执行
static int __init hello_init(void) {
    printk(KERN_INFO "Hello! 驱动已加载 - 这里的 printk 相当于你的串口 printf\n");
    return 0;
}

// 驱动卸载时执行
void  __exit hello_exit(void) {
    printk(KERN_INFO "Goodbye! 驱动已卸载\n");
}

module_init(hello_init);
module_exit(hello_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("YourName");

