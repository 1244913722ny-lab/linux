# 自己写一个极简 GPIO-Keys 驱动（设备树版）

> 目标：从零写一个 Linux 输入子系统驱动，把“多个 GPIO 引脚的电平变化”变成“键盘按键事件”（最终在 `/dev/input/eventX` 出 `EV_KEY`）。
> 约束（你定的）：**只支持设备树**、**支持多个按键**、**软件去抖**、**不做休眠(suspend/resume)**、**不做 sysfs**。
> 配套源码：内核自带 `gpio_keys.c`（已在同目录注释版 `gpio_keys.c` 中详细讲解）；本文档教你**自己**写一个裁剪版。


---

## 0. 这个驱动在 Linux 里“长什么样”

| 角色 | 用什么 | 作用 |
|------|--------|------|
| 平台驱动 | `struct platform_driver` + `of_match_table` | 由设备树 `compatible` 匹配后 `probe` |
| 输入设备 | 一个 `struct input_dev` | 代表整个“键盘”，所有按键共用 |
| 每按键运行态 | 一个 `struct my_button_data` | 存该按键的 GPIO、IRQ、键值、去抖定时器 |
| 中断 | `request_irq` | 引脚电平变化触发，但**只启动去抖定时器** |
| 下半部 | `timer_list` 回调 | 定时器到期（电平已稳定）后才读值、上报 |

关键设计：**中断上半部极快**（只 `mod_timer`），**真正的读电平 + 上报放在定时器回调里**——这就是“软件去抖”的核心。

---

## 1. 先写设备树（驱动照着解析）

驱动是“被动”的：它读设备树里描述了几个按键，就建几个。先在设备树（或 overlay）里描述你的按键：

```dts
/ {
    my_gpio_keys {
        compatible = "my,gpio-keys";      /* 驱动靠这个字符串匹配 */
        autorepeat;                        /* 可选：长按自动重复 */

        button@0 {
            label = "User Button 0";
            linux,code = <KEY_0>;          /* 键值，来自 include/uapi/linux/input-event-codes.h */
            gpios = <&gpio2 1 GPIO_ACTIVE_LOW>;  /* 控制器=GPIO2, 引脚=1, 低电平有效 */
            debounce-interval-ms = <30>;   /* 软件去抖 30ms */
        };

        button@1 {
            label = "User Button 1";
            linux,code = <KEY_ENTER>;
            gpios = <&gpio2 2 GPIO_ACTIVE_LOW>;
            debounce-interval-ms = <30>;
        };
    };
};
```

字段含义：
- `compatible`：必须和驱动里 `of_device_id` 的 `.compatible` **完全一致**，否则不匹配。
- `linux,code`：内核标准键值（`KEY_0`、`KEY_ENTER`、`KEY_VOLUMEUP`…）。头文件 `include/uapi/linux/input-event-codes.h`。
- `gpios`：`<控制器 phandle> <引脚号> <有效电平>`。第三项是物理有效极性，`gpiod_get_value()` 会帮你转成**逻辑值**，所以驱动里不用自己算 active_low。
- `debounce-interval-ms`：你的软件去抖时长，单位毫秒。

> 注意：`&gpio2` 要换成你平台上真实存在的 GPIO 控制器（vexpress 上是 `&gpio0`~`&gpio3`，对应 PL061）。下面“测试”一节会讲怎么用 `gpio-mockup` 造一个虚拟控制器，免得真有硬件。

---

## 2. 驱动骨架：头文件与结构体

### 头文件
```c
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/input.h>
#include <linux/gpio/consumer.h>   /* gpiod_* API（现代 descriptor 接口） */
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_gpio.h>         /* of_get_named_gpiod_flags */
#include <linux/timer.h>
#include <linux/slab.h>
#include <linux/types.h>
```

### 两个结构体
```c
/* 每个按键的运行态 */
struct my_button_data {
    struct gpio_desc *gpiod;   /* 代表一根引脚 */
    int irq;                   /* 该引脚对应的中断号 */
    unsigned int code;         /* 按键码，来自设备树 linux,code */
    const char *label;         /* 按键名字（from DT） */
    int debounce_ms;           /* 软件去抖时长 */
    bool last_state;           /* 上次上报的逻辑电平（去抖后比较用） */
    struct timer_list timer;   /* 去抖定时器 */
    struct input_dev *input;   /* 指向所属 input 设备 */
};

/* 整个设备的私有数据（柔性数组装所有按键） */
struct my_gpio_keys_drvdata {
    struct input_dev *input;
    int n_buttons;
    struct my_button_data buttons[];  /* 柔性数组：n_buttons 个 */
};
```

> **柔性数组** `buttons[]` 是内核常用技巧：用 `struct_size()` 一次性分配“头部 + N 个按键”，比先 alloc 头部再 alloc 数组更省事、零碎内存少。

---

## 3. 一步步实现

### 步骤 1：probe —— 统计按键数、分配结构

```c
static int my_gpio_keys_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    struct device_node *np = dev->of_node;
    struct device_node *pp;
    struct my_gpio_keys_drvdata *ddata;
    struct input_dev *input;
    int nbuttons = 0, i = 0, ret;

    /* 1.1 数子节点 = 按键个数 */
    for_each_child_of_node(np, pp)
        nbuttons++;
    if (!nbuttons) {
        dev_err(dev, "no buttons defined in DT\n");
        return -EINVAL;
    }

    /* 1.2 一次性分配“私有数据 + 柔性数组” */
    ddata = devm_kzalloc(dev, struct_size(ddata, buttons, nbuttons), GFP_KERNEL);
    if (!ddata)
        return -ENOMEM;

    /* 1.3 分配 input 设备 */
    input = devm_input_allocate_device(dev);
    if (!input)
        return -ENOMEM;
    input->name = "my_gpio_keys";
    input->id.bustype = BUS_HOST;
    input->dev.parent = dev;

    ddata->input = input;
    ddata->n_buttons = nbuttons;
    ...
```

### 步骤 2：遍历每个子节点，setup 一个按键

把下面这段放进 `for_each_child_of_node(np, pp)` 循环里（接着步骤 1）：

```c
    for_each_child_of_node(np, pp) {
        struct my_button_data *b = &ddata->buttons[i];
        enum of_gpio_flags flags;

        /* 2.1 读 linux,code（必填） */
        if (of_property_read_u32(pp, "linux,code", &b->code)) {
            dev_err(dev, "button %d missing linux,code\n", i);
            continue;
        }

        /* 2.2 读可选属性 */
        b->label = of_get_property(pp, "label", NULL);
        b->debounce_ms = 30;
        of_property_read_u32(pp, "debounce-interval-ms", &b->debounce_ms);

        /* 2.3 拿 GPIO 描述符 */
          b->gpiod = devm_gpiod_get_from_of_node(dev, pp, "gpios", 0, GPIOD_IN,
                                           b->label ? b->label : "my_gpio_key");
        if (IS_ERR(b->gpiod)) {
            dev_err(dev, "button %d gpios error\n", i);
            continue;
        }

        /* 2.4 拿中断号 + 记录初始逻辑电平 */
        b->irq = gpiod_to_irq(b->gpiod);
        if (b->irq < 0) {
            dev_err(dev, "button %d no irq\n", i);
            continue;
        }
        b->input = input;
        b->last_state = gpiod_get_value(b->gpiod);

        /* 2.5 初始化去抖定时器（回调见步骤 4） */
        timer_setup(&b->timer, my_gpio_keys_debounce, 0);

        /* 2.6 注册中断：物理双边沿（按下/松开都进） */
        ret = devm_request_irq(dev, b->irq, my_gpio_keys_isr,
                    IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
                    b->label ? b->label : "my_gpio_key", b);
        if (ret) {
            dev_err(dev, "button %d request_irq failed\n", i);
            continue;
        }

        /* 2.7 告诉 input 子系统：这个设备能产生该键值 */
        input_set_capability(input, EV_KEY, b->code);
        i++;
    }
```

> **为什么中断用物理双边沿？** 因为按下（物理下降沿或上升沿，取决于 active_low）和松开都会改变电平，我们都想知道。逻辑极性由 `gpiod_get_value()` 在回调里处理，中断这层只看物理边沿。

### 步骤 3：中断上半部（只启动去抖定时器）

```c
static irqreturn_t my_gpio_keys_isr(int irq, void *dev_id)
{
    struct my_button_data *bdata = dev_id;

    /* 上半部：什么重活都不干，只重置去抖定时器。
     * 抖动期会频繁进中断，每次都“续命”定时器；
     * 只有电平稳定、不再进中断，定时器才会真的到期。 */
    mod_timer(&bdata->timer, jiffies + msecs_to_jiffies(bdata->debounce_ms));
    return IRQ_HANDLED;
}
```

### 步骤 4：去抖下半部（定时器回调，真正读值 + 上报）

```c
static void my_gpio_keys_debounce(struct timer_list *t)
{
    struct my_button_data *bdata = from_timer(bdata, t, timer);
    bool state;

    /* 定时器到期 = 电平已稳定超过 debounce 间隔，读真实逻辑电平 */
    state = gpiod_get_value(bdata->gpiod);   /* 已考虑 active_low，返回逻辑值 */

    if (state != bdata->last_state) {        /* 确实翻转了，才算有效按键 */
        bdata->last_state = state;
        input_report_key(bdata->input, bdata->code, state);  /* state=1 按下, 0 松开 */
        input_sync(bdata->input);            /* 必须 sync，事件才真正提交 */
    }
}
```

> 软件去抖原理小结：抖动期间引脚在 0/1 间乱跳，每次跳都进中断、都 `mod_timer` 把到期时间往后推；当电平真正稳定，不再有中断，`mod_timer` 不再被续命，定时器才到期，此时读到的就是干净的稳定电平——再和上次状态比，变了才上报。完美过滤毛刺。

### 步骤 5：注册 input 设备 & 收尾

回到 `probe`（步骤 1 的 `...` 处），在所有按键 setup 完之后：

```c
    /* 5.1 注册 input 设备（注册成功前别用 input_free_device） */
    ret = input_register_device(input);
    if (ret) {
        dev_err(dev, "cannot register input device\n");
        input_free_device(input);   /* 仅注册失败才 free */
        return ret;
    }

    platform_set_drvdata(pdev, ddata);
    dev_info(dev, "my_gpio_keys probed with %d buttons\n", nbuttons);
    return 0;
}
```

### 步骤 6：remove（因为没用 devm_ 注册 input，要手动注销）

```c
static int my_gpio_keys_remove(struct platform_device *pdev)
{
    struct my_gpio_keys_drvdata *ddata = platform_get_drvdata(pdev);
    int i;

    for (i = 0; i < ddata->n_buttons; i++)
        del_timer_sync(&ddata->buttons[i].timer);   /* 停掉所有去抖定时器 */
    /*devm_input_allocate_device 会自动注销 input 设备*/
    // input_unregister_device(ddata->input);           /* 注销 input 设备 */
    return 0;
}
```

> 注意顺序：`del_timer_sync` 必须在 `input_unregister_device` 之前，避免定时器回调里还在访问已经释放的 `input`。

### 步骤 7：platform_driver 框架

```c
static const struct of_device_id my_gpio_keys_of_match[] = {
    { .compatible = "my,gpio-keys", },
    { /* sentinel（必须留空表示结束） */ }
};
MODULE_DEVICE_TABLE(of, my_gpio_keys_of_match);

static struct platform_driver my_gpio_keys_driver = {
    .probe  = my_gpio_keys_probe,
    .remove = my_gpio_keys_remove,
    .driver = {
        .name = "my_gpio_keys",
        .of_match_table = my_gpio_keys_of_match,  /* 只匹配设备树 */
    },
};
module_platform_driver(my_gpio_keys_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("your name");
MODULE_DESCRIPTION("Minimal DT-only GPIO keys driver with software debounce");
```

到此，驱动就写完了。对照一下你定的约束：
- ✅ **只支持设备树**：匹配靠 `of_match_table`，解析全用 `of_*` API，没有 `platform_data` 分支。
- ✅ **多个按键**：柔性数组 + 子节点遍历，几个写几个。
- ✅ **软件去抖**：定时器下半部。
- ✅ **不做休眠**：没有 `suspend`/`resume` 回调，没有 `wakeup-source`。
- ✅ **不做 sysfs**：没有 `device_attribute`、没有 `sysfs_create_*`。

---

## 4. 编译（树外模块，交叉编译到 ARM）

在同目录放一个 `Makefile`（参考同目录已有 `Makefile` 的写法）：

```makefile
KDIR ?= /mnt/d/linux-study/linux/file/linux-5.0.17
CROSS_COMPILE ?= arm-linux-gnueabihf-
ARCH ?= arm
PWD := $(shell pwd)

obj-m += my_gpio_keys.o

all:
	$(MAKE) -C $(KDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) M=$(PWD) clean
```

```bash
cd /mnt/d/linux-study/linux/file/driver
make                 # 编出 my_gpio_keys.ko
```

---

## 5. 在 QEMU (vexpress-a9) 上测试

你平台上没有真实按钮，用 **`gpio-mockup`**（内核自带的虚拟 GPIO 控制器）来制造“按键”：

1. **开内核选项**（menuconfig）：
   `Device Drivers → GPIO Support → Memory mapped GPIO drivers → GPIO Testing Driver` 设 `<M>`（这就是 `gpio-mockup`）。
   顺便 `GPIO PL061` 也可设 `<M>`（vexpress 真实 GPIO）。
2. **加载 mockup，造一个 4 线虚拟芯片**：
   ```bash
   insmod gpio-mockup.ko gpio_mockup_ranges=-1,4
   ```
   会得到 `gpiochipN`（用 `cat /sys/class/gpio/gpiochip*/label` 找 label 含 `gpio-mockup` 的那个，记下编号 X）。
3. **改你的设备树 overlay**，把 `gpios` 指向它，例如：
   ```dts
   gpios = <&mockup 0 GPIO_ACTIVE_LOW>;
   ```
   其中 `&mockup` 要换成 mockup 对应的 gpio controller node（或直接用编号，取决于你 overlay 写法）。最省事的办法：先不绑真实控制器，在驱动里用 `gpio=0` 编号测试——但“只支持设备树”约束下，还是写正确的 `gpios` 属性。
4. **刷进 QEMU 并加载你的驱动**：
   ```bash
   # 宿主机：把 .ko 推进 SD 卡镜像（你已有的 cpko.sh 流程）
   sh cpko.sh
   # QEMU 里
   mount /dev/mmcblk0p1 /mnt/sd
   insmod /mnt/sd/my_gpio_keys.ko
   dmesg | tail        # 看 "probed with N buttons"
   ```
5. **从用户态翻转 mockup 引脚，模拟按键**：
   ```bash
   # 找到 mockup 第一条线的 sysfs 编号，翻转它
   gpiodetect          # 找 mockup 的 chip 与基地址
   gpioset <chip> 0=1  # 拉高（触发一次电平变化 → 中断 → 去抖 → 上报）
   gpioset <chip> 0=0  # 拉低（松开）
   ```
   若没 `gpiod` 工具，可临时开 `CONFIG_GPIO_SYSFS` 后用 `echo 1 > /sys/class/gpio/gpioY/value`。
6. **看按键事件**：
   ```bash
   evtest /dev/input/eventX     # 翻转引脚时应看到 KEY_0 / KEY_ENTER 的 press/release
   ```
   或 `cat /proc/bus/input/devices` 确认设备已注册、`hexdump /dev/input/eventX` 看原始事件。

> 提示：第一次跑建议先 `dmesg` 看 probe 是否成功、IRQ 是否申请到；若 `gpiod_to_irq` 返回错误，多半是设备树 `gpios` 指向的控制器/引脚不存在。

---

## 6. 常见坑速查

| 现象 | 原因 / 解决 |
|------|------------|
| 驱动不 probe | `compatible` 与设备树不一致；或设备树节点没被编译进 DTB |
| `gpiod_to_irq` 失败 / irq<0 | `gpios` 指向的控制器在平台上不存在（mockup 没加载或编号错） |
| 按键事件“抖”得乱报 | 没走定时器下半部，在中断里直接 `input_report_key` 了；或 debounce_ms 太小 |
| 事件出不来 | 忘了 `input_sync()`；或忘了 `input_set_capability(EV_KEY, code)` |
| remove 时内核崩溃 | 没先 `del_timer_sync` 就 `input_unregister_device`；或重复 free |
| 双机/重复加载报 exists | 驱动名/ko 名与内核自带 `gpio_keys` 冲突，把本驱动改名（如 `my_gpio_keys`） |

---

## 7. 进阶（学完基础再碰）

- **硬件去抖**：PL061 自带硬件去抖寄存器，可在 `gpiod` 阶段配置，比软件定时器更省 CPU。
- **共享中断 / 单中断多按键**：某些板子多个按键共用一条中断线，需要在 ISR 里轮询所有引脚判断是谁。
- **PM 唤醒**：你暂时不做；以后想让按键唤醒挂起的系统，要加 `device_init_wakeup` + `suspend/resume` 里 `enable_irq_wake`。

祝你写顺。建议顺序：**先按步骤 1→7 自己敲一遍 → 编译 → 用 mockup 测通 → 最后再回来看本文当核对参考**。卡住哪一步，把报错贴给我。
