#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>

static int my_irq;
static DECLARE_WAIT_QUEUE_HEAD(my_wait_queue);
static int condition = 0;

static struct proc_dir_entry *irq_proc;

static irqreturn_t my_irq_handler(int irq, void *dev_id) {
  printk("内核检测到中断！中断号: %d\n", irq);
  condition = 1;
  wake_up_interruptible(&my_wait_queue);
  return IRQ_HANDLED;
}

static ssize_t dev_read(struct file *file, char __user *buf, size_t len,
                        loff_t *offset) {
  printk("APP：我打算读数据，但还没中断，我要睡了...\n");

  if (wait_event_interruptible(my_wait_queue, condition != 0)) {
    return -ERESTARTSYS;
  }

  condition = 0;

  unsigned long ret = copy_to_user(buf, "Interrupt Data", 15);
  if (ret)
    return -EFAULT;

  return 15;
}

static int dev_open(struct inode *inode, struct file *file) {
  printk("app open dev\n");
  return 0;
}

static int dev_release(struct inode *inode, struct file *file) {
  printk("app close dev\n");
  return 0;
}

static const struct file_operations my_fops = {
    .owner = THIS_MODULE,
    .open = dev_open,
    .release = dev_release,
    .read = dev_read,
};

#define MYDEV_MAJOR 240
#define MYDEV_MINOR 0
static struct cdev my_cdev;

/* 5.0内核proc用file_operations，不要proc_ops */
static ssize_t proc_trigger_irq_write(struct file *file, const char __user *buf,
                                      size_t count, loff_t *ppos) {
  printk("proc: 手动触发中断！\n");
  my_irq_handler(my_irq, NULL);
  return count;
}

static const struct file_operations proc_trigger_fops = {
    .owner = THIS_MODULE,
    .write = proc_trigger_irq_write,
};

static int my_probe(struct platform_device *pdev) {
  int ret;
  dev_t devno;

  printk("中断驱动匹配成功！\n");

  my_irq = platform_get_irq(pdev, 0);
  if (my_irq < 0) {
    printk("get irq fail\n");
    return my_irq;
  }

  ret = request_irq(my_irq, my_irq_handler, IRQF_TRIGGER_RISING, "myirq_test",
                    NULL);
  if (ret) {
    printk("request_irq fail\n");
    return ret;
  }

  devno = MKDEV(MYDEV_MAJOR, MYDEV_MINOR);
  ret = register_chrdev_region(devno, 1, "myirqdev");
  if (ret < 0) {
    printk("register_chrdev_region fail\n");
    goto err_free_irq;
  }
  cdev_init(&my_cdev, &my_fops);
  my_cdev.owner = THIS_MODULE;
  ret = cdev_add(&my_cdev, devno, 1);
  if (ret < 0) {
    printk("cdev_add fail\n");
    goto err_unreg_region;
  }

  /* 创建proc节点，5.0接口 */
  irq_proc = proc_create("trigger_irq", 0644, NULL, &proc_trigger_fops);
  if (!irq_proc) {
    printk("proc create failed\n");
  }

  return 0;

err_unreg_region:
  unregister_chrdev_region(devno, 1);
err_free_irq:
  free_irq(my_irq, NULL);
  return ret;
}

static int my_remove(struct platform_device *pdev) {
  dev_t devno = MKDEV(MYDEV_MAJOR, MYDEV_MINOR);

  if (irq_proc) {
    proc_remove(irq_proc);
    irq_proc = NULL;
  }

  cdev_del(&my_cdev);
  unregister_chrdev_region(devno, 1);
  free_irq(my_irq, NULL);
  printk("中断驱动已卸载\n");
  return 0;
}

static const struct of_device_id my_of_match[] = {{.compatible = "ny,irq-vdev"},
                                                  {}};
MODULE_DEVICE_TABLE(of, my_of_match);

static struct platform_driver my_irq_driver = {
    .probe = my_probe,
    .remove = my_remove,
    .driver =
        {
            .name = "myirq_drv",
            .of_match_table = my_of_match,
        },
};

module_platform_driver(my_irq_driver);

MODULE_LICENSE("GPL");
