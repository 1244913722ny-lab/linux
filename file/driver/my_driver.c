#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>  // 必须包含：定义了 struct of_device_id
#include <linux/of.h>               // 建议包含：处理设备树节点的常用函数
#include <linux/io.h> // 必须包含这个处理 IO 映射的头文件
#include <linux/slab.h>
void __iomem *my_reg_base; // 定义一个内核虚拟地址指针
// 当设备树和驱动匹配成功时，内核会调用这个函数
static int my_probe(struct platform_device *pdev) {
    printk("相亲成功！发现了设备树里的虚拟设备\n");
    // 就像 STM32 获取外设基地址一样
    struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    if (res) {
        printk("获取到寄存器地址：0x%08x\n", res->start);
    }

    void *test_mem = kmalloc(4096, GFP_KERNEL); // 申請一塊真實內存
    unsigned long phys_addr = test_mem; // 拿到它的物理地址
    printk("真實物理地址: 0x%lx\n", phys_addr);

    my_reg_base=phys_addr;
   // my_reg_base = ioremap(phys_addr ,4);
    
    if (my_reg_base) {
        printk("地址映射成功！虚拟地址指针 = %px\n", my_reg_base);
        
        // 2. 尝试写入一个值（虽然这里是虚拟内存，但在真实驱动里这就是写寄存器）
        iowrite32(0xABCDEF01, my_reg_base);
        
        // 3. 再读回来验证
       unsigned int  val = ioread32(my_reg_base);
        printk("从映射地址读取到的值: 0x%X\n", val);
    }
    return 0;
}

static int my_remove(struct platform_device *pdev) {
    printk("设备已移除\n");
  //  iounmap(my_reg_base);
    return 0;
}

// 匹配表：必须和设备树里的 compatible 字符串一模一样
static const struct of_device_id my_of_match[] = {
    { .compatible = "ny,my-vdev" },
    { }
};

static struct platform_driver my_driver = {
    .driver = {
        .name = "my_vdev_driver",
        .of_match_table = my_of_match,
    },
    .probe = my_probe,
    .remove = my_remove,
};

module_platform_driver(my_driver);
MODULE_LICENSE("GPL");
