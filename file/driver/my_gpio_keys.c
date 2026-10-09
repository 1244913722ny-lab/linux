#include "linux/device.h"
#include "linux/overflow.h"
#include <linux/gpio/consumer.h> /* gpiod_* API（现代 descriptor 接口） */
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h> /* of_get_named_gpiod_flags */
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/types.h>

static void my_gpio_keys_debounce(struct timer_list *t);
static irqreturn_t my_gpio_keys_isr(int irq, void *dev_id);
/* 每个按键的运行态 */
struct my_button_data {
  struct gpio_desc *gpiod; /* 代表一根引脚 */
  int irq;                 /* 该引脚对应的中断号*/
  unsigned int code;       /* 按键码，来自设备树 linux,code */
  const char *label;       /* 按键名字（from DT） */
  int debounce_ms;         /* 软件去抖时长 */
  bool last_state;         /* 上次上报的逻辑电平（去抖后比较用） */
  struct timer_list timer; /* 去抖定时器 */
  struct input_dev *input; /* 指向所属 input 设备 */
};

/* 整个设备的私有数据（柔性数组装所有按键） */
struct my_gpio_keys_drvdata {
  struct input_dev *input;         /* 指向所属 input 设备 */
  int n_buttons;                   /* 按键数量 */
  struct my_button_data buttons[]; /* 柔性数组：n_buttons 个 */
};

static int my_gpio_keys_probe(struct platform_device *pdev) {

  struct device *dev = &pdev->dev;
  struct device_node *np = dev->of_node;
  struct device_node *pp;
  struct my_gpio_keys_drvdata *ddata;
  struct input_dev *input;
  int nbuttons = 0, i = 0, ret;

  //   步骤 1：probe —— 统计按键数、分配结构
  /* 1.1 数子节点 = 按键个数 */
  for_each_child_of_node(np, pp) { nbuttons++; }
  if (nbuttons == 0) {
    dev_err(dev, "No buttons found\n");
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

  // 步骤 2：遍历每个子节点，setup 一个按键
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
      dev_err(dev, "button %d irq error\n", i);
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
      dev_err(dev, "button %d request irq error\n", i);
      continue;
    }

    /* 2.7 告诉 input 子系统：这个设备能产生该键值 */
    input_set_capability(input, EV_KEY, b->code);

    i++;
  }
  /* 5.1 注册 input 设备（注册成功前别用 input_free_device） */
  ret = input_register_device(input);
  if (ret) {
    dev_err(dev, "input_register error\n");
    return ret;
  }

  platform_set_drvdata(pdev, ddata);
  dev_info(dev, "my_gpio_keys probed with %d buttons\n", nbuttons);
  return 0;
}
// 步骤 3：中断上半部（只启动去抖定时器）
static irqreturn_t my_gpio_keys_isr(int irq, void *dev_id) {
  struct my_button_data *bdata = dev_id;
  /* 上半部：什么重活都不干，只重置去抖定时器。
   * 抖动期会频繁进中断，每次都“续命”定时器；
   * 只有电平稳定、不再进中断，定时器才会真的到期。 */
  mod_timer(&bdata->timer, jiffies + msecs_to_jiffies(bdata->debounce_ms));
  return IRQ_HANDLED;
}
// 步骤 4：去抖下半部（定时器回调，真正读值 +上报）
static void my_gpio_keys_debounce(struct timer_list *t) {
  //   去抖下半部（定时器回调，真正读值 + 上报）
  struct my_button_data *bdata = from_timer(bdata, t, timer);
  bool state;
  /* 定时器到期 = 电平已稳定超过 debounce 间隔，读真实逻辑电平 */
  state = gpiod_get_value(bdata->gpiod); /* 已考虑 active_low，返回逻辑值 */
  if (state != bdata->last_state) {      /* 确实翻转了，才算有效按键 */
    bdata->last_state = state;
    input_report_key(bdata->input, bdata->code,
                     state);  /* state=1 按下, 0 松开 */
    input_sync(bdata->input); /* 必须 sync，事件才真正提交 */
  }
}
static int my_gpio_keys_remove(struct platform_device *pdev) {
  struct my_gpio_keys_drvdata *ddata = platform_get_drvdata(pdev);
  int i;

  for (i = 0; i < ddata->n_buttons; i++)
    del_timer_sync(&ddata->buttons[i].timer); /* 停掉所有去抖定时器 */

  /*devm_input_allocate_device 会自动注销 input 设备*/
  // input_unregister_device(ddata->input);      /* 注销 input 设备 */
  return 0;
}

static const struct of_device_id my_gpio_keys_of_match[] = {
    {
        .compatible = "my,gpio-keys",
    },
    {/* sentinel（必须留空表示结束） */}};

MODULE_DEVICE_TABLE(of, my_gpio_keys_of_match);

static struct platform_driver my_gpio_keys_driver = {
    .probe = my_gpio_keys_probe,
    .remove = my_gpio_keys_remove,
    .driver =
        {
            .name = "my_gpio_keys",
            .of_match_table = my_gpio_keys_of_match, /* 只匹配设备树 */
        },
};
module_platform_driver(my_gpio_keys_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ny");
MODULE_DESCRIPTION("Minimal DT-only GPIO keys driver with software debounce");