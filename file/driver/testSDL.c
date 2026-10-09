#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#define PL111_FB_PHYS 0x4c000000
#define XRES 1024
#define YRES 768
#define BPP 16

static void __iomem *vmem;

static int __init pl111_red_init(void) {
  int x, y;
  uint16_t __iomem *p;
  vmem = ioremap(PL111_FB_PHYS, XRES * YRES * BPP / 8);
  if (!vmem) {
    pr_err("ioremap failed\n");
    return -ENOMEM;
  }
  p = (uint16_t __iomem *)vmem;
  for (y = 0; y < YRES; y++) {
    for (x = 0; x < XRES; x++) {
      // 注意：访问ioremap内存必须用 writel / writew，不能直接 *p 赋值！
      writew(0xF800, &p[y * XRES + x]);
    }
  }
  pr_info("fill red done\n");
  return 0;
}

static void __exit pl111_red_exit(void) {
  iounmap(vmem);
  pr_info("unload\n");
}

module_init(pl111_red_init);
module_exit(pl111_red_exit);
MODULE_LICENSE("GPL");
