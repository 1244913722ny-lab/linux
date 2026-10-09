/*
 * Driver for keys on GPIO lines capable of generating interrupts.
 *
 * Copyright 2005 Phil Blundell
 * Copyright 2010, 2011 David Jander <david@protonic.nl>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

// /* =====================================================================
//  * 中文导读（学习用）
//  * ---------------------------------------------------------------------
//  * 本文件是 Linux 内核自带的「GPIO 按键驱动」源码（drivers/input/keyboard/
//  * gpio_keys.c）。它的作用是：把「一根 GPIO 引脚 + 一个外部中断」封装成
//  * 标准 Linux 输入设备（input device），最终在用户空间表现为
//  * /dev/input/eventX，并产生按键事件（KEY_*/SW_*）。
//  *
//  * 它是初学者学习「字符/输入设备驱动」最经典的范本，原因有三：
//  *   1. 依赖的子系统少而典型：GPIO 子系统 + 输入子系统 + 中断；
//  *   2. 同时演示了「设备树（Device Tree）」和「传统 platform_data」
//  *      两种获取硬件参数的方式；
//  *   3. 涉及了驱动开发中几乎必会的套路：probe、IRQ 注册、去抖、
//  *      工作队列/定时器、sysfs 属性、电源管理与唤醒。
//  *
//  * 阅读建议：
//  *   - 先看下面的两个核心结构体（gpio_button_data / gpio_keys_drvdata），
//  *     它们是整个驱动的「数据骨架」；
//  *   - 再看 gpio_keys_probe()，它是驱动被加载时执行的入口，能串起所有流程；
//  *   - 最后看中断处理函数，理解「一次物理按键 → 一个 input 事件」的链路。
//  *
//  * 注意：本文件仅对原版代码加中文注释，逻辑与内核源码完全一致，可直接
//  * 用于学习对照。GPL 许可证头必须保留。
//  * ===================================================================== */

// /* ---- 头文件说明 ----
//  * 内核驱动通常只 include 公共头文件（<linux/...>），这些头由内核构建系统
//  * 提供。下面列出与本驱动最相关的几个：
//  *   module.h        模块机制（MODULE_*/init/exit）
//  *   platform_device.h  platform 总线设备/驱动模型
//  *   input.h          输入子系统核心 API（input_dev、input_event 等）
//  *   gpio_keys.h      本驱动用到的数据结构定义（gpio_keys_button 等）
//  *   gpio/consumer.h  「GPIO 描述符」新接口（gpiod_*），推荐用法
//  *   of.h / of_irq.h  设备树解析（device tree）
//  *   workqueue.h      工作队列（延迟执行，用于软件去抖）
//  *   interrupt.h/irq.h 中断相关
//  */
#include <linux/module.h>

#include <dt-bindings/input/gpio-keys.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio_keys.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/sysctl.h>
#include <linux/workqueue.h>

/* =====================================================================
 * 核心结构体 1：gpio_button_data —— 描述「单个按键」的运行时状态
 * ---------------------------------------------------------------------
 * 一个 gpio-keys 设备可以有多个按键，每个按键对应一个 gpio_button_data。
 * 它把「设备树/平台数据里描述的静态参数（gpio_keys_button）」与
 * 「运行时才确定的资源（gpio_desc、irq、定时器/工作队列）」组合在一起。
 * ===================================================================== */
struct gpio_button_data {
  /* 指向该按键的静态描述（在探测阶段由平台数据或设备树填充） */
  const struct gpio_keys_button *button;
  /* 该按键所属的 input_dev（输入设备），所有按键共享同一个 */
  struct input_dev *input;
  /* GPIO 描述符（新接口）。若为 NULL，表示这是一个「纯中断按键」，
   * 即没有真实 GPIO 引脚可读电平，只有一根中断线。 */
  struct gpio_desc *gpiod;

  /* 该按键对应的键码（如 KEY_POWER、KEY_ENTER），指向 ddata->keymap 中
   * 的某一项，方便统一管理与 sysfs 操作。 */
  unsigned short *code;

  /* —— 仅「纯中断按键」使用：用于模拟「按键释放」——
   * 有些按键（如专用电源键）只会在「按下」时触发一次中断，松开时没有
   * 中断，于是用一个定时器在按下后一段时间自动补发一次「松开」事件。 */
  struct timer_list release_timer;
  unsigned int release_delay; /* 定时器延时，单位毫秒 */

  /* —— 仅「GPIO 型按键」使用：软件去抖 ——
   * GPIO 型按键既能读电平又能产生中断（双边沿触发）。为避免机械抖动
   * 产生多次中断，中断到来后不直接上报，而是延后 software_debounce 毫秒
   * 再由工作队列读取稳定后的电平上报。 */
  struct delayed_work work;
  unsigned int software_debounce; /* 单位毫秒 */

  unsigned int irq;                 /* 该按键使用的中断号 */
  unsigned int wakeup_trigger_type; /* 唤醒用中断触发边沿类型 */
  spinlock_t lock;  /* 保护 key_pressed 等字段（中断上下文用） */
  bool disabled;    /* 是否被 sysfs 临时禁用 */
  bool key_pressed; /* 当前是否处于「按下」状态（IRQ 型用） */
  bool suspended;   /* 是否已进入挂起（suspend）状态 */
};

/* =====================================================================
 * 核心结构体 2：gpio_keys_drvdata —— 描述「整个 gpio-keys 设备」
 * ---------------------------------------------------------------------
 * 一个 gpio-keys 设备 = 一个 input_dev + 一组按键。
 * 末尾的 data[0] 是「柔性数组（flexible array member）」技巧：分配时
 * 按实际按键数多申请一段内存，让 data[i] 紧随其后连续存放。
 * ===================================================================== */
struct gpio_keys_drvdata {
  /* 平台数据（静态：按键个数、每个按键参数）。可能来自设备树解析的结果 */
  const struct gpio_keys_platform_data *pdata;
  struct input_dev *input;         /* 统一的输入设备 */
  struct mutex disable_lock;       /* 保护「禁用/启用按键」的并发 */
  unsigned short *keymap;          /* 键码数组，长度 = nbuttons */
  struct gpio_button_data data[0]; /* 变长数组，每个按键一项 */
};

/* =====================================================================
 * sysfs 接口说明（保持原注释，便于对照内核文档）
 *
 * 在 /sys/devices/platform/gpio-keys/ 下提供 4 个属性：
 *	keys [ro]              - 可被禁用的按键（EV_KEY）位图
 *	switches [ro]          - 可被禁用的开关（EV_SW）位图
 *	disabled_keys [rw]     - 当前已禁用的按键位图
 *	disabled_switches [rw] - 当前已禁用的开关位图
 *
 * 用户空间可写 disabled_keys/disabled_switches 来关闭某个按键的中断，
 * 从而停止其事件上报（禁用即关闭该 IRQ 线）。
 * 只能禁用那些「不共享中断」的按键（can_disable 为 true）。
 * ===================================================================== */

/**
 * get_n_events_by_type() - 返回某类型按钮的最大事件数
 * @type: 按钮类型（%EV_KEY 或 %EV_SW）
 *
 * 返回值用来分配足够大的位图（bitmap）。KEY 类最大为 KEY_CNT，SW 类为 SW_CNT。
 */
static int get_n_events_by_type(int type) {
  BUG_ON(type != EV_SW && type != EV_KEY);

  return (type == EV_KEY) ? KEY_CNT : SW_CNT;
}

/**
 * get_bm_events_by_type() - 返回某类型按钮在 input_dev 上支持的位图
 * @dev: 输入设备
 * @type: 按钮类型（%EV_KEY 或 %EV_SW）
 *
 * 返回 dev->keybit（按键位图）或 dev->swbit（开关位图），用于校验
 * 用户请求禁用的按键是否真的存在。
 */
static const unsigned long *get_bm_events_by_type(struct input_dev *dev,
                                                  int type) {
  BUG_ON(type != EV_SW && type != EV_KEY);

  return (type == EV_KEY) ? dev->keybit : dev->swbit;
}

/**
 * gpio_keys_disable_button() - 禁用某个 GPIO 按键
 * @bdata: 要禁用的按键数据
 *
 * 通过关闭 IRQ 来禁用。禁用后该按键不再产生输入事件。
 * 注意：只能禁用「不共享中断」的按键。
 * 调用前应持 bdata->disable_lock，避免并发禁用造成竞态。
 */
static void gpio_keys_disable_button(struct gpio_button_data *bdata) {
  if (!bdata->disabled) {
    /* 关闭中断；若有 GPIO 则取消挂起的工作队列，
     * 否则删除定时器，确保不再有任何上报动作。 */
    disable_irq(bdata->irq);

    if (bdata->gpiod)
      cancel_delayed_work_sync(&bdata->work);
    else
      del_timer_sync(&bdata->release_timer);

    bdata->disabled = true;
  }
}

/**
 * gpio_keys_enable_button() - 启用某个 GPIO 按键
 * @bdata: 要启用的按键数据
 *
 * 重新打开中断。调用前应持 bdata->disable_lock。
 */
static void gpio_keys_enable_button(struct gpio_button_data *bdata) {
  if (bdata->disabled) {
    enable_irq(bdata->irq);
    bdata->disabled = false;
  }
}

/**
 * gpio_keys_attr_show_helper() - 把（可禁用/已禁用）按键位图格式化为字符串
 * @ddata: 设备私有数据
 * @buf:   输出缓冲区
 * @type:  按钮类型（%EV_KEY / %EV_SW）
 * @only_disabled: 是否只列「当前已禁用」的，还是列「所有可禁用」的
 *
 * 供 sysfs 的 show 函数调用，返回 0 或负错误码。
 */
static ssize_t gpio_keys_attr_show_helper(struct gpio_keys_drvdata *ddata,
                                          char *buf, unsigned int type,
                                          bool only_disabled) {
  int n_events = get_n_events_by_type(type);
  unsigned long *bits;
  ssize_t ret;
  int i;

  /* 按类型分配位图（清零） */
  bits = bitmap_zalloc(n_events, GFP_KERNEL);
  if (!bits)
    return -ENOMEM;

  /* 遍历每个按键，把符合条件的键码置位 */
  for (i = 0; i < ddata->pdata->nbuttons; i++) {
    struct gpio_button_data *bdata = &ddata->data[i];

    if (bdata->button->type != type)
      continue;

    if (only_disabled && !bdata->disabled)
      continue;

    __set_bit(*bdata->code, bits);
  }

  /* %*pbl 是内核 printk 格式，用于打印位图（如 "11-9,5"） */
  ret = scnprintf(buf, PAGE_SIZE - 1, "%*pbl", n_events, bits);
  buf[ret++] = '\n';
  buf[ret] = '\0';

  bitmap_free(bits);

  return ret;
}

/**
 * gpio_keys_attr_store_helper() - 依据用户传入的位图启用/禁用按键
 * @ddata: 设备私有数据
 * @buf:   用户空间传入的字符串位图（如 "11,5"）
 * @type:  按钮类型（%EV_KEY / %EV_SW）
 *
 * 解析位图 → 校验 → 调用 disable/enable。返回 0 或负错误码。
 */
static ssize_t gpio_keys_attr_store_helper(struct gpio_keys_drvdata *ddata,
                                           const char *buf, unsigned int type) {
  int n_events = get_n_events_by_type(type);
  const unsigned long *bitmap = get_bm_events_by_type(ddata->input, type);
  unsigned long *bits;
  ssize_t error;
  int i;

  bits = bitmap_zalloc(n_events, GFP_KERNEL);
  if (!bits)
    return -ENOMEM;

  /* 把 "11,5" 这种字符串解析成位图 */
  error = bitmap_parselist(buf, bits, n_events);
  if (error)
    goto out;

  /* 校验：用户请求禁用的按键必须都在「支持禁用」的集合内 */
  if (!bitmap_subset(bits, bitmap, n_events)) {
    error = -EINVAL;
    goto out;
  }

  /* 进一步校验：被点名禁用的按键必须声明了 can_disable */
  for (i = 0; i < ddata->pdata->nbuttons; i++) {
    struct gpio_button_data *bdata = &ddata->data[i];

    if (bdata->button->type != type)
      continue;

    if (test_bit(*bdata->code, bits) && !bdata->button->can_disable) {
      error = -EINVAL;
      goto out;
    }
  }

  /* 加锁后统一执行禁用/启用 */
  mutex_lock(&ddata->disable_lock);

  for (i = 0; i < ddata->pdata->nbuttons; i++) {
    struct gpio_button_data *bdata = &ddata->data[i];

    if (bdata->button->type != type)
      continue;

    if (test_bit(*bdata->code, bits))
      gpio_keys_disable_button(bdata);
    else
      gpio_keys_enable_button(bdata);
  }

  mutex_unlock(&ddata->disable_lock);

out:
  bitmap_free(bits);
  return error;
}

/* 宏：批量生成「只读」sysfs show 函数。
 * 用法举例：ATTR_SHOW_FN(keys, EV_KEY, false) 会生成一个
 * gpio_keys_show_keys() 函数，内部调用 show_helper 并传入对应类型。 */
#define ATTR_SHOW_FN(name, type, only_disabled)                                \
  static ssize_t gpio_keys_show_##name(                                        \
      struct device *dev, struct device_attribute *attr, char *buf) {          \
    struct platform_device *pdev = to_platform_device(dev);                    \
    struct gpio_keys_drvdata *ddata = platform_get_drvdata(pdev);              \
                                                                               \
    return gpio_keys_attr_show_helper(ddata, buf, type, only_disabled);        \
  }

ATTR_SHOW_FN(keys, EV_KEY, false);
ATTR_SHOW_FN(switches, EV_SW, false);
ATTR_SHOW_FN(disabled_keys, EV_KEY, true);
ATTR_SHOW_FN(disabled_switches, EV_SW, true);

/*
 * 只读属性：
 *   /sys/devices/platform/gpio-keys/keys
 *   /sys/devices/platform/gpio-keys/switches
 */
static DEVICE_ATTR(keys, S_IRUGO, gpio_keys_show_keys, NULL);
static DEVICE_ATTR(switches, S_IRUGO, gpio_keys_show_switches, NULL);

/* 宏：批量生成「可写」sysfs store 函数 */
#define ATTR_STORE_FN(name, type)                                              \
  static ssize_t gpio_keys_store_##name(struct device *dev,                    \
                                        struct device_attribute *attr,         \
                                        const char *buf, size_t count) {       \
    struct platform_device *pdev = to_platform_device(dev);                    \
    struct gpio_keys_drvdata *ddata = platform_get_drvdata(pdev);              \
    ssize_t error;                                                             \
                                                                               \
    error = gpio_keys_attr_store_helper(ddata, buf, type);                     \
    if (error)                                                                 \
      return error;                                                            \
                                                                               \
    return count;                                                              \
  }

ATTR_STORE_FN(disabled_keys, EV_KEY);
ATTR_STORE_FN(disabled_switches, EV_SW);

/*
 * 读写属性：
 *   /sys/devices/platform/gpio-keys/disabled_keys
 *   /sys/devices/platform/gpio-keys/disables_switches
 */
static DEVICE_ATTR(disabled_keys, S_IWUSR | S_IRUGO,
                   gpio_keys_show_disabled_keys, gpio_keys_store_disabled_keys);
static DEVICE_ATTR(disabled_switches, S_IWUSR | S_IRUGO,
                   gpio_keys_show_disabled_switches,
                   gpio_keys_store_disabled_switches);

/* 把 4 个属性组织成属性组，便于在 probe 时一次性注册 */
static struct attribute *gpio_keys_attrs[] = {
    &dev_attr_keys.attr,
    &dev_attr_switches.attr,
    &dev_attr_disabled_keys.attr,
    &dev_attr_disabled_switches.attr,
    NULL,
};

static const struct attribute_group gpio_keys_attr_group = {
    .attrs = gpio_keys_attrs,
};

/* =====================================================================
 * 上报事件（GPIO 型按键专用）
 * ---------------------------------------------------------------------
 * 读取 GPIO 当前电平，把它作为按键状态上报给输入子系统。
 * 这是「GPIO 型」与「纯中断型」的分水岭：只有 GPIO 型才有 gpiod，能读电平。
 * ===================================================================== */
static void gpio_keys_gpio_report_event(struct gpio_button_data *bdata) {
  const struct gpio_keys_button *button = bdata->button;
  struct input_dev *input = bdata->input;
  unsigned int type = button->type ?: EV_KEY; /* 默认按 EV_KEY 处理 */
  int state;

  /* 读取引脚电平（cansleep 版本：可能睡眠，需在进程上下文调用，
   * 正对应后面用工作队列调用的设计）。返回 1=高/按下相关，0=低。 */
  state = gpiod_get_value_cansleep(bdata->gpiod);
  if (state < 0) {
    dev_err(input->dev.parent, "failed to get gpio state: %d\n", state);
    return;
  }

  if (type == EV_ABS) {
    /* 绝对轴事件（如旋钮），仅在「按下」时上报 value */
    if (state)
      input_event(input, type, button->code, button->value);
  } else {
    /* 普通按键/开关：直接用引脚电平作为状态（1=按下, 0=松开） */
    input_event(input, type, *bdata->code, state);
  }
  /* input_sync：告诉输入子系统「一次事件上报完毕」，用户空间据此
   * 才能 read 到一个完整事件帧。非常重要，缺一不可。 */
  input_sync(input);
}

/* 工作队列回调函数：由中断下半部（delayed_work）触发，在进程上下文执行 */
static void gpio_keys_gpio_work_func(struct work_struct *work) {
  struct gpio_button_data *bdata =
      container_of(work, struct gpio_button_data, work.work);

  gpio_keys_gpio_report_event(bdata);

  /* 若该按键可唤醒系统，上报后释放 wakeup 源（与中断里的 pm_stay_awake 配对）
   */
  if (bdata->button->wakeup)
    pm_relax(bdata->input->dev.parent);
}

/* =====================================================================
 * GPIO 型按键的中断上半部（top half）
 * ---------------------------------------------------------------------
 * 中断上下文，必须快进快出：这里不做任何可能睡眠或耗时的操作，
 * 只是「登记一次唤醒」并「延后到工作队列去读电平」。
 * ===================================================================== */
static irqreturn_t gpio_keys_gpio_isr(int irq, void *dev_id) {
  struct gpio_button_data *bdata = dev_id;

  /* 防御性检查：进来的 irq 必须就是本按键注册的那个 */
  BUG_ON(irq != bdata->irq);

  if (bdata->button->wakeup) {
    const struct gpio_keys_button *button = bdata->button;

    /* 标记系统应保持唤醒（防止 suspend 过程中被误挂起） */
    pm_stay_awake(bdata->input->dev.parent);
    if (bdata->suspended && (button->type == 0 || button->type == EV_KEY)) {
      /* 若系统在挂起期间收到此中断，可能中断处理跑到时
       * 按键已经松开，这里主动补发一次「按下」事件，
       * 保证唤醒行为正确。 */
      input_report_key(bdata->input, button->code, 1);
    }
  }

  /* 核心：把「读电平+上报」推迟 software_debounce 毫秒后由工作队列执行。
   * 这样既实现了软件去抖，又避免在中断上下文做可能睡眠的 GPIO 读取。 */
  mod_delayed_work(system_wq, &bdata->work,
                   msecs_to_jiffies(bdata->software_debounce));

  return IRQ_HANDLED;
}

/* =====================================================================
 * 纯中断型按键的定时器回调：用于「自动补发松开事件」
 * ===================================================================== */
static void gpio_keys_irq_timer(struct timer_list *t) {
  struct gpio_button_data *bdata = from_timer(bdata, t, release_timer);
  struct input_dev *input = bdata->input;
  unsigned long flags;

  /* 中断上下文/软中断上下文，用 spin_lock_irqsave 保护共享字段 */
  spin_lock_irqsave(&bdata->lock, flags);
  if (bdata->key_pressed) {
    /* 时间到仍未收到新的「按下」中断，视为按键已松开，补发 0 */
    input_event(input, EV_KEY, *bdata->code, 0);
    input_sync(input);
    bdata->key_pressed = false;
  }
  spin_unlock_irqrestore(&bdata->lock, flags);
}

/* =====================================================================
 * 纯中断型按键的中断处理函数（无 GPIO，只有中断线）
 * ---------------------------------------------------------------------
 * 这种按键的典型场景：专用电源键、或某些键盘控制器，只给「按下/松开」
 * 各一次中断，或干脆只有按下中断。
 * ===================================================================== */
static irqreturn_t gpio_keys_irq_isr(int irq, void *dev_id) {
  struct gpio_button_data *bdata = dev_id;
  struct input_dev *input = bdata->input;
  unsigned long flags;

  BUG_ON(irq != bdata->irq);

  spin_lock_irqsave(&bdata->lock, flags);

  if (!bdata->key_pressed) {
    /* 唤醒源上报 */
    if (bdata->button->wakeup)
      pm_wakeup_event(bdata->input->dev.parent, 0);

    /* 第一次按下：先报「按下=1」 */
    input_event(input, EV_KEY, *bdata->code, 1);
    input_sync(input);

    if (!bdata->release_delay) {
      /* 没有 release_delay：立即补发「松开=0」，即一次完整点击 */
      input_event(input, EV_KEY, *bdata->code, 0);
      input_sync(input);
      goto out;
    }

    bdata->key_pressed = true;
  }

  if (bdata->release_delay)
    /* 设定定时器，超时后由 gpio_keys_irq_timer 补发松开事件 */
    mod_timer(&bdata->release_timer,
              jiffies + msecs_to_jiffies(bdata->release_delay));
out:
  spin_unlock_irqrestore(&bdata->lock, flags);
  return IRQ_HANDLED;
}

/* 资源释放回调：在设备注销时调用，确保等待中的工作/定时器彻底结束 */
static void gpio_keys_quiesce_key(void *data) {
  struct gpio_button_data *bdata = data;

  if (bdata->gpiod)
    cancel_delayed_work_sync(&bdata->work);
  else
    del_timer_sync(&bdata->release_timer);
}

/* =====================================================================
 * gpio_keys_setup_key() —— 单个按键的「资源申请与初始化」
 * ---------------------------------------------------------------------
 * 这是理解整个驱动的关键函数，它完成：
 *   1. 获取 GPIO 描述符（设备树方式或传统 GPIO 编号方式）；
 *   2. 设置硬件去抖（若硬件不支持则退化为软件去抖）；
 *   3. 取得中断号（从 GPIO 映射，或直接使用给定的 irq）；
 *   4. 根据「有 GPIO / 纯中断」选择不同的 ISR 与初始化方式；
 *   5. 注册中断（devm 托管，自动释放）；
 *   6. 声明该按键在输入设备上的能力（input_set_capability）。
 * ===================================================================== */
static int gpio_keys_setup_key(struct platform_device *pdev,
                               struct input_dev *input,
                               struct gpio_keys_drvdata *ddata,
                               const struct gpio_keys_button *button, int idx,
                               struct fwnode_handle *child) {
  const char *desc = button->desc ? button->desc : "gpio_keys";
  struct device *dev = &pdev->dev;
  struct gpio_button_data *bdata = &ddata->data[idx];
  irq_handler_t isr;
  unsigned long irqflags;
  int irq;
  int error;

  bdata->input = input;
  bdata->button = button;
  spin_lock_init(&bdata->lock);

  /* 方式一：设备树/ACPI 子节点方式获取 GPIO（现代推荐做法）
   * child 非空表示来自设备树解析。 */
  if (child) {
    /* 从子节点读取 GPIO，方向设为输入（GPIOD_IN） */
    bdata->gpiod =
        devm_fwnode_get_gpiod_from_child(dev, NULL, child, GPIOD_IN, desc);
    if (IS_ERR(bdata->gpiod)) {
      error = PTR_ERR(bdata->gpiod);
      if (error == -ENOENT) {
        /* GPIO 是可选的：允许「纯中断按键」的情况，
         * 此时 GPIO 取不到是正常的。 */
        bdata->gpiod = NULL;
      } else {
        if (error != -EPROBE_DEFER)
          dev_err(dev, "failed to get gpio: %d\n", error);
        return error;
      }
    }
  } else if (gpio_is_valid(button->gpio)) {
    /* 方式二：传统 GPIO 编号（整数）方式，兼容老的平台数据。
     * 申请 GPIO 并转为描述符。 */
    unsigned flags = GPIOF_IN;

    if (button->active_low)
      flags |= GPIOF_ACTIVE_LOW;

    error = devm_gpio_request_one(dev, button->gpio, flags, desc);
    if (error < 0) {
      dev_err(dev, "Failed to request GPIO %d, error %d\n", button->gpio,
              error);
      return error;
    }

    bdata->gpiod = gpio_to_desc(button->gpio);
    if (!bdata->gpiod)
      return -EINVAL;
  }

  /* —— 有 GPIO 的情况（GPIO 型按键）—— */
  if (bdata->gpiod) {
    bool active_low = gpiod_is_active_low(bdata->gpiod);

    /* 尝试让 GPIO 硬件控制器做去抖（纳秒转微秒：ms*1000） */
    if (button->debounce_interval) {
      error =
          gpiod_set_debounce(bdata->gpiod, button->debounce_interval * 1000);
      /* 若硬件不支持硬件去抖（返回错误），则退化为软件去抖：
       * 把 debounce_interval 记到 software_debounce，
       * 后续由中断里的 delayed_work 延后读取。 */
      if (error < 0)
        bdata->software_debounce = button->debounce_interval;
    }

    if (button->irq) {
      /* 平台数据直接给了 irq 号 */
      bdata->irq = button->irq;
    } else {
      /* 否则从 GPIO 映射出中断号：这是 GPIO 子系统的标准能力 */
      irq = gpiod_to_irq(bdata->gpiod);
      if (irq < 0) {
        error = irq;
        dev_err(dev, "Unable to get irq number for GPIO %d, error %d\n",
                button->gpio, error);
        return error;
      }
      bdata->irq = irq;
    }

    /* 初始化工作队列（下半部），绑定回调 */
    INIT_DELAYED_WORK(&bdata->work, gpio_keys_gpio_work_func);

    /* GPIO 型用双边沿触发：按下（上升沿）和松开（下降沿）都进中断 */
    isr = gpio_keys_gpio_isr;
    irqflags = IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING;

    /* 根据唤醒时的动作类型，决定挂起/唤醒阶段应配置哪种触发边沿。
     * 这影响「低电平有效」按键在唤醒时该认哪条边沿为「按下」。 */
    switch (button->wakeup_event_action) {
    case EV_ACT_ASSERTED:
      bdata->wakeup_trigger_type =
          active_low ? IRQ_TYPE_EDGE_FALLING : IRQ_TYPE_EDGE_RISING;
      break;
    case EV_ACT_DEASSERTED:
      bdata->wakeup_trigger_type =
          active_low ? IRQ_TYPE_EDGE_RISING : IRQ_TYPE_EDGE_FALLING;
      break;
    case EV_ACT_ANY:
      /* fall through */
    default:
      /* 其它情况：挂起/恢复时不重新配置触发类型 */
      break;
    }
  } else {
    /* —— 无 GPIO 的情况（纯中断按键）—— */
    if (!button->irq) {
      dev_err(dev, "Found button without gpio or irq\n");
      return -EINVAL;
    }

    bdata->irq = button->irq;

    if (button->type && button->type != EV_KEY) {
      dev_err(dev, "Only EV_KEY allowed for IRQ buttons.\n");
      return -EINVAL;
    }

    /* 纯中断按键没有「松开中断」，用 release_delay 定时器模拟松开 */
    bdata->release_delay = button->debounce_interval;
    timer_setup(&bdata->release_timer, gpio_keys_irq_timer, 0);

    isr = gpio_keys_irq_isr;
    irqflags = 0; /* 触发类型由硬件/平台数据决定，这里不追加 */

    /* 纯中断按键在松开时没有中断，因此无需为唤醒重配触发类型。 */
  }

  /* 把键码写入 keymap 并声明输入设备支持该键码 */
  bdata->code = &ddata->keymap[idx];
  *bdata->code = button->code;
  input_set_capability(input, button->type ?: EV_KEY, *bdata->code);

  /* 注册「设备注销时」要执行的清理动作（取消工作/定时器），
   * devm 自动管理生命周期。 */
  error = devm_add_action(dev, gpio_keys_quiesce_key, bdata);
  if (error) {
    dev_err(dev, "failed to register quiesce action, error: %d\n", error);
    return error;
  }

  /* 若平台声明该按键可禁用，则不允许共享中断线
   * （因为禁用=关闭 IRQ，共享会导致误伤其它使用者）。 */
  if (!button->can_disable)
    irqflags |= IRQF_SHARED;

  /* 注册中断处理函数。
   * devm_request_any_context_irq：若中断可在进程上下文处理则用线程化
   * 中断，否则用普通中断——灵活适配不同底层。 */
  error =
      devm_request_any_context_irq(dev, bdata->irq, isr, irqflags, desc, bdata);
  if (error < 0) {
    dev_err(dev, "Unable to claim irq %d; error %d\n", bdata->irq, error);
    return error;
  }

  return 0;
}

/* 把当前所有 GPIO 型按键的电平状态上报一次（用于 open/恢复时同步初始状态） */
static void gpio_keys_report_state(struct gpio_keys_drvdata *ddata) {
  struct input_dev *input = ddata->input;
  int i;

  for (i = 0; i < ddata->pdata->nbuttons; i++) {
    struct gpio_button_data *bdata = &ddata->data[i];
    if (bdata->gpiod)
      gpio_keys_gpio_report_event(bdata);
  }
  input_sync(input);
}

/* input_dev 的 open 回调：第一个用户打开设备时调用 */
static int gpio_keys_open(struct input_dev *input) {
  struct gpio_keys_drvdata *ddata = input_get_drvdata(input);
  const struct gpio_keys_platform_data *pdata = ddata->pdata;
  int error;

  /* 若平台提供了 enable 回调（如给 GPIO 上电），先调用 */
  if (pdata->enable) {
    error = pdata->enable(input->dev.parent);
    if (error)
      return error;
  }

  /* 上报当前各按键状态，确保用户空间一开始就能拿到正确的初始电平 */
  gpio_keys_report_state(ddata);

  return 0;
}

/* input_dev 的 close 回调：最后一个用户关闭设备时调用 */
static void gpio_keys_close(struct input_dev *input) {
  struct gpio_keys_drvdata *ddata = input_get_drvdata(input);
  const struct gpio_keys_platform_data *pdata = ddata->pdata;

  if (pdata->disable)
    pdata->disable(input->dev.parent);
}

/*
 * Handlers for alternative sources of platform_data
 */

/*
 * Translate properties into platform_data
 */

/* =====================================================================
 * 从设备树解析按键参数（设备树是主流 ARM 平台的硬件描述方式）
 * ---------------------------------------------------------------------
 * 设备树里大致这样描述：
 *   gpio-keys {
 *       compatible = "gpio-keys";
 *       autorepeat;                 // 对应 pdata->rep
 *       button@1 {
 *           label = "power";
 *           gpios = <&gpio0 5 GPIO_ACTIVE_LOW>;
 *           linux,code = <KEY_POWER>;
 *           wakeup-source;           // 对应 button->wakeup
 *           debounce-interval = <5>;
 *       };
 *   };
 * 本函数把上面这些属性读出来，填进 gpio_keys_platform_data / gpio_keys_button。
 * ===================================================================== */
static struct gpio_keys_platform_data *
gpio_keys_get_devtree_pdata(struct device *dev) {
  struct gpio_keys_platform_data *pdata;
  struct gpio_keys_button *button;
  struct fwnode_handle *child;
  int nbuttons;

  /* 统计子节点个数（每个子节点 = 一个按键） */
  nbuttons = device_get_child_node_count(dev);
  if (nbuttons == 0)
    return ERR_PTR(-ENODEV);

  /* 一次性分配：平台数据 + 紧随其后的 nbuttons 个 button 描述 */
  pdata = devm_kzalloc(dev, sizeof(*pdata) + nbuttons * sizeof(*button),
                       GFP_KERNEL);
  if (!pdata)
    return ERR_PTR(-ENOMEM);

  button = (struct gpio_keys_button *)(pdata + 1);

  pdata->buttons = button;
  pdata->nbuttons = nbuttons;

  /* 读取顶层属性：autorepeat（自动连发）、label（设备名） */
  pdata->rep = device_property_read_bool(dev, "autorepeat");

  device_property_read_string(dev, "label", &pdata->name);

  /* 逐个子节点解析 */
  device_for_each_child_node(dev, child) {
    if (is_of_node(child))
      button->irq = irq_of_parse_and_map(to_of_node(child), 0);

    /* 键码是必填项，缺失则报错 */
    if (fwnode_property_read_u32(child, "linux,code", &button->code)) {
      dev_err(dev, "Button without keycode\n");
      fwnode_handle_put(child);
      return ERR_PTR(-EINVAL);
    }

    fwnode_property_read_string(child, "label", &button->desc);

    if (fwnode_property_read_u32(child, "linux,input-type", &button->type))
      button->type = EV_KEY; /* 默认类型：按键 */

    /* 唤醒相关：新属性 wakeup-source 或旧属性 gpio-key,wakeup */
    button->wakeup = fwnode_property_read_bool(child, "wakeup-source") ||
                     fwnode_property_read_bool(child, "gpio-key,wakeup");

    fwnode_property_read_u32(child, "wakeup-event-action",
                             &button->wakeup_event_action);

    button->can_disable = fwnode_property_read_bool(child, "linux,can-disable");

    if (fwnode_property_read_u32(child, "debounce-interval",
                                 &button->debounce_interval))
      button->debounce_interval = 5; /* 默认 5ms 去抖 */

    button++;
  }

  return pdata;
}

/* 设备树匹配表：compatible 为 "gpio-keys" 的节点会匹配本驱动 */
static const struct of_device_id gpio_keys_of_match[] = {
    {
        .compatible = "gpio-keys",
    },
    {},
};
MODULE_DEVICE_TABLE(of, gpio_keys_of_match);

/* =====================================================================
 * gpio_keys_probe() —— 驱动的探测（初始化）入口
 * ---------------------------------------------------------------------
 * 当内核发现一个匹配的设备（设备树节点或平台设备）时，调用本函数。
 * 它负责：申请 input_dev、为每个按键调用 setup_key、创建 sysfs、
 * 最后注册输入设备。成功注册后，该设备即可在用户空间产生事件。
 * ===================================================================== */
static int gpio_keys_probe(struct platform_device *pdev) {
  struct device *dev = &pdev->dev;
  const struct gpio_keys_platform_data *pdata = dev_get_platdata(dev);
  struct fwnode_handle *child = NULL;
  struct gpio_keys_drvdata *ddata;
  struct input_dev *input;
  size_t size;
  int i, error;
  int wakeup = 0;

  /* 没有传统平台数据就用设备树解析一份 */
  if (!pdata) {
    pdata = gpio_keys_get_devtree_pdata(dev);
    if (IS_ERR(pdata))
      return PTR_ERR(pdata);
  }

  /* 分配驱动私有数据（含变长数组 data[nbuttons]） */
  size = sizeof(struct gpio_keys_drvdata) +
         pdata->nbuttons * sizeof(struct gpio_button_data);
  ddata = devm_kzalloc(dev, size, GFP_KERNEL);
  if (!ddata) {
    dev_err(dev, "failed to allocate state\n");
    return -ENOMEM;
  }

  /* 键码数组 */
  ddata->keymap =
      devm_kcalloc(dev, pdata->nbuttons, sizeof(ddata->keymap[0]), GFP_KERNEL);
  if (!ddata->keymap)
    return -ENOMEM;

  /* 分配输入设备（devm 自动释放） */
  input = devm_input_allocate_device(dev);
  if (!input) {
    dev_err(dev, "failed to allocate input device\n");
    return -ENOMEM;
  }

  ddata->pdata = pdata;
  ddata->input = input;
  mutex_init(&ddata->disable_lock);

  platform_set_drvdata(pdev, ddata);
  input_set_drvdata(input, ddata);

  /* 填充 input_dev 的基本信息 */
  input->name = pdata->name ?: pdev->name;
  input->phys = "gpio-keys/input0";
  input->dev.parent = dev;
  input->open = gpio_keys_open;
  input->close = gpio_keys_close;

  /* 输入设备的身份标识（总线/厂商/产品/版本），用户空间可用以区分设备 */
  input->id.bustype = BUS_HOST;
  input->id.vendor = 0x0001;
  input->id.product = 0x0001;
  input->id.version = 0x0100;

  /* 键码表信息（用于 EVIOCGKEYCODE 等 ioctl） */
  input->keycode = ddata->keymap;
  input->keycodesize = sizeof(ddata->keymap[0]);
  input->keycodemax = pdata->nbuttons;

  /* 若设备树设了 autorepeat，开启输入子系统的自动连发功能 */
  if (pdata->rep)
    __set_bit(EV_REP, input->evbit);

  /* 逐个按键初始化（申请 GPIO/IRQ、注册中断等） */
  for (i = 0; i < pdata->nbuttons; i++) {
    const struct gpio_keys_button *button = &pdata->buttons[i];

    if (!dev_get_platdata(dev)) {
      child = device_get_next_child_node(dev, child);
      if (!child) {
        dev_err(dev, "missing child device node for entry %d\n", i);
        return -EINVAL;
      }
    }

    error = gpio_keys_setup_key(pdev, input, ddata, button, i, child);
    if (error) {
      fwnode_handle_put(child);
      return error;
    }

    if (button->wakeup)
      wakeup = 1;
  }

  fwnode_handle_put(child);

  /* 创建 sysfs 属性组（keys/switches/disabled_*） */
  error = devm_device_add_group(dev, &gpio_keys_attr_group);
  if (error) {
    dev_err(dev, "Unable to export keys/switches, error: %d\n", error);
    return error;
  }

  /* 向输入子系统注册设备：此后 /dev/input/eventX 才会出现 */
  error = input_register_device(input);
  if (error) {
    dev_err(dev, "Unable to register input device, error: %d\n", error);
    return error;
  }

  /* 设置该设备是否可作为唤醒源（供后面 suspend 逻辑使用） */
  device_init_wakeup(dev, wakeup);

  return 0;
}

/* =====================================================================
 * 挂起/唤醒（电源管理）相关
 * ===================================================================== */

/* 把单个按键配置为唤醒源，并可按需要在挂起时切换中断触发边沿 */
static int __maybe_unused
gpio_keys_button_enable_wakeup(struct gpio_button_data *bdata) {
  int error;

  error = enable_irq_wake(bdata->irq);
  if (error) {
    dev_err(bdata->input->dev.parent,
            "failed to configure IRQ %d as wakeup source: %d\n", bdata->irq,
            error);
    return error;
  }

  if (bdata->wakeup_trigger_type) {
    error = irq_set_irq_type(bdata->irq, bdata->wakeup_trigger_type);
    if (error) {
      dev_err(bdata->input->dev.parent,
              "failed to set wakeup trigger %08x for IRQ %d: %d\n",
              bdata->wakeup_trigger_type, bdata->irq, error);
      disable_irq_wake(bdata->irq);
      return error;
    }
  }

  return 0;
}

/* 撤销上面的唤醒配置，并恢复为双边沿触发 */
static void __maybe_unused
gpio_keys_button_disable_wakeup(struct gpio_button_data *bdata) {
  int error;

  /* GPIO 型按键平时就是双边沿；纯中断型不支持改唤醒触发，这里仅在有
   * wakeup_trigger_type 时才把它恢复成双边沿。 */
  if (bdata->wakeup_trigger_type) {
    error = irq_set_irq_type(bdata->irq, IRQ_TYPE_EDGE_BOTH);
    if (error)
      dev_warn(bdata->input->dev.parent,
               "failed to restore interrupt trigger for IRQ %d: %d\n",
               bdata->irq, error);
  }

  error = disable_irq_wake(bdata->irq);
  if (error)
    dev_warn(bdata->input->dev.parent,
             "failed to disable IRQ %d as wake source: %d\n", bdata->irq,
             error);
}

/* 挂起前：把所有 wakeup 按键设为唤醒源，并标记 suspended */
static int __maybe_unused
gpio_keys_enable_wakeup(struct gpio_keys_drvdata *ddata) {
  struct gpio_button_data *bdata;
  int error;
  int i;

  for (i = 0; i < ddata->pdata->nbuttons; i++) {
    bdata = &ddata->data[i];
    if (bdata->button->wakeup) {
      error = gpio_keys_button_enable_wakeup(bdata);
      if (error)
        goto err_out;
    }
    bdata->suspended = true;
  }

  return 0;

err_out:
  while (i--) {
    bdata = &ddata->data[i];
    if (bdata->button->wakeup)
      gpio_keys_button_disable_wakeup(bdata);
    bdata->suspended = false;
  }

  return error;
}

/* 恢复后：撤销唤醒源，清除 suspended 标记 */
static void __maybe_unused
gpio_keys_disable_wakeup(struct gpio_keys_drvdata *ddata) {
  struct gpio_button_data *bdata;
  int i;

  for (i = 0; i < ddata->pdata->nbuttons; i++) {
    bdata = &ddata->data[i];
    bdata->suspended = false;
    if (irqd_is_wakeup_set(irq_get_irq_data(bdata->irq)))
      gpio_keys_button_disable_wakeup(bdata);
  }
}

/* 系统挂起时调用 */
static int __maybe_unused gpio_keys_suspend(struct device *dev) {
  struct gpio_keys_drvdata *ddata = dev_get_drvdata(dev);
  struct input_dev *input = ddata->input;
  int error;

  if (device_may_wakeup(dev)) {
    /* 该设备被标记为可唤醒：保持中断有效，仅配置唤醒源 */
    error = gpio_keys_enable_wakeup(ddata);
    if (error)
      return error;
  } else {
    /* 不可唤醒：若当前有用户打开设备，则关闭它（停止上报） */
    mutex_lock(&input->mutex);
    if (input->users)
      gpio_keys_close(input);
    mutex_unlock(&input->mutex);
  }

  return 0;
}

/* 系统恢复时调用 */
static int __maybe_unused gpio_keys_resume(struct device *dev) {
  struct gpio_keys_drvdata *ddata = dev_get_drvdata(dev);
  struct input_dev *input = ddata->input;
  int error = 0;

  if (device_may_wakeup(dev)) {
    gpio_keys_disable_wakeup(ddata);
  } else {
    mutex_lock(&input->mutex);
    if (input->users)
      error = gpio_keys_open(input);
    mutex_unlock(&input->mutex);
  }

  if (error)
    return error;

  /* 恢复后同步一次当前按键状态 */
  gpio_keys_report_state(ddata);
  return 0;
}

/* 把 suspend/resume 封装成 PM 操作集，挂到 driver.pm 上 */
static SIMPLE_DEV_PM_OPS(gpio_keys_pm_ops, gpio_keys_suspend, gpio_keys_resume);

/* =====================================================================
 * platform 驱动注册
 * ===================================================================== */
static struct platform_driver gpio_keys_device_driver = {
    .probe = gpio_keys_probe,
    .driver = {
        .name = "gpio-keys",
        .pm = &gpio_keys_pm_ops,
        .of_match_table = gpio_keys_of_match, /* 设备树匹配入口 */
    }};

/* 驱动初始化：注册 platform 驱动 */
static int __init gpio_keys_init(void) {
  return platform_driver_register(&gpio_keys_device_driver);
}

/* 驱动卸载：注销 platform 驱动 */
static void __exit gpio_keys_exit(void) {
  platform_driver_unregister(&gpio_keys_device_driver);
}

/* 由于按键设备通常要在其它子系统就绪后才探测，用 late_initcall
 * 推迟到启动后期注册（比普通 module_init 晚）。 */
late_initcall(gpio_keys_init);
module_exit(gpio_keys_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Phil Blundell <pb@handhelds.org>");
MODULE_DESCRIPTION("Keyboard driver for GPIOs");
MODULE_ALIAS("platform:gpio-keys");
