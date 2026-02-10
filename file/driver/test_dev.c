#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h> // 必须用这个处理内核与用户空间的数据交换
#include <linux/delay.h>

#define MAX_SIZE 1024
#define DEVICE_NAME "test_dev"

static int major; // 主设备号
static dev_t dev_num; // 设备号
static struct cdev my_cdev; // 字符设备结构体
static struct class *my_class; // 设备类

static char kernel_buf[MAX_SIZE]; // 模拟硬件缓存



static int dev_open(struct inode *inode, struct file *file) {
    printk("设备被打开\n");
    return 0;
}

static ssize_t dev_read(struct file *file, char __user *buf, size_t len, loff_t *offset)
{
    if(*offset >= MAX_SIZE) {
        printk("设备已读完\n");
        return 0; // 已经读完了
    }
    if(len > MAX_SIZE - *offset) {
        len = MAX_SIZE - *offset; // 调整读取长度
    }
    if(copy_to_user(buf, kernel_buf + *offset, len)) {
        printk("设备读取失败\n");
        return -EFAULT; // 复制失败
    }
    *offset += len;
    return len;
}

static ssize_t dev_write(struct file *file, const char __user *buf, size_t len, loff_t *offset)
{
    if(*offset >= MAX_SIZE) {
        printk("设备已写满\n");
        return 0; // 已经写满了
    }
    
    if(len > MAX_SIZE - *offset) {
        len = MAX_SIZE - *offset; // 调整写入长度
    }
     mdelay(10); 
    if(copy_from_user(kernel_buf + *offset, buf, len)) {
        printk("设备写入失败\n");
        
        return -EFAULT; // 复制失败
    }

    *offset += len;
    kernel_buf[*offset] = '\0'; // 确保字符串以'\0'结尾
    printk("设备被写入: %s\n", kernel_buf);
    
    return len;
}   

static int dev_release(struct inode *inode, struct file *file) {
    printk("设备被关闭\n");
    return 0;
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = dev_open,
    .read = dev_read,
    .write = dev_write,
    .release = dev_release,
    .llseek = default_llseek, // 加上这一行，内核会自动帮你处理 offset 的加减
};

static int __init dev_init(void) {
    // 1. 分配设备号
    if (alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME) < 0) {
        printk("无法分配设备号\n");
        return -1;
    }
    major = MAJOR(dev_num);
    printk("分配的设备号: %d\n", major);

    // 2. 初始化字符设备
    cdev_init(&my_cdev, &fops);
    my_cdev.owner = THIS_MODULE;

    // 3. 添加字符设备到系统
    if (cdev_add(&my_cdev, dev_num, 1) < 0) {
        printk("无法添加字符设备\n");
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    // 4. 创建设备类
    my_class = class_create(THIS_MODULE, DEVICE_NAME);
    if (IS_ERR(my_class)) {
        printk("无法创建设备类\n");
        cdev_del(&my_cdev);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    // 5. 创建设备文件
    device_create(my_class, NULL, dev_num, NULL, DEVICE_NAME);
    return 0;
}

static void __exit dev_exit(void) {
    device_destroy(my_class, dev_num);
    class_destroy(my_class);
    cdev_del(&my_cdev);
    unregister_chrdev_region(dev_num, 1);
}

module_init(dev_init);
module_exit(dev_exit);
MODULE_LICENSE("GPL");

