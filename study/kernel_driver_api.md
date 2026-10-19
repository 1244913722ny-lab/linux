# Linux 内核驱动常用 API 分类整理

> 面向学习路线：**字符设备 / Input 输入子系统 / GPIO-GPIOD / 平台驱动 / 设备树 / 内存分配 / 调试打印 / 中断与下半部 / 定时器与延时 / 并发与同步 / misc 设备 / sysfs-procfs 属性文件 / 完整 file_operations / 阻塞 IO 与 waitqueue / regmap 与 clk**
> 内核版本：**5.x**（基于你 QEMU 环境的 linux-5.0.17 源码核对，签名完全兼容）
> 约定：`dev` 指 `struct device *`（通常是 `&pdev->dev`）；返回值带 `__must_check` 的必须判错。
> 所有 `devm_*` 函数都会在**设备解绑（probe 失败 / 模块卸载）时自动释放**，无需手写 `xxx_put/_free/_unregister`。

---

## 0. devm 资源管理（先看这一段）

驱动里能用 `devm_*` 的就尽量用，省掉 `remove`/`exit` 里一大堆对称清理，也避免出错路径漏释放。

| 普通版 | devm 版 | 头文件 |
|---|---|---|
| `kzalloc` | `devm_kzalloc` | `<linux/device.h>` |
| `ioremap` | `devm_ioremap` / `devm_ioremap_resource` | `<linux/io.h>` / `<linux/device.h>` |
| `request_irq` | `devm_request_irq` | `<linux/interrupt.h>` |
| `gpiod_get` | `devm_gpiod_get` | `<linux/gpio/consumer.h>` |
| `clk_get` | `devm_clk_get` | `<linux/clk.h>` |
| `regulator_get` | `devm_regulator_get` | `<linux/regulator/consumer.h>` |

**例外（5.0.17 没有 devm 封装的）**：
- `cdev_add` → 没有 `devm_cdev_add`。清理用 `cdev_del`，或若要托管给设备生命周期，用 `devm_add_action_or_reset(dev, my_cdev_release, &my_cdev)` 自己包一层。
- `platform_get_irq` → 没有 `devm_platform_get_irq`（该 API 5.1 才加入）。正确姿势：`irq = platform_get_irq(pdev, 0);` 然后 `devm_request_irq(&pdev->dev, irq, ...)` 把**中断**托管给设备。

---

## 1. 字符设备（Char Device）

头文件：`<linux/cdev.h>`、`<linux/fs.h>`、`<linux/device.h>`

### 1.1 主/次设备号与注册

```c
int alloc_chrdev_region(dev_t *dev, unsigned baseminor, unsigned count, const char *name);
int register_chrdev_region(dev_t from, unsigned count, const char *name);
void unregister_chrdev_region(dev_t from, unsigned count);
```
- **作用**：向内核申请一段设备号（`dev_t`）。`alloc_*` 让内核自动分配主号（推荐）；`register_*` 用你指定的主号。
- **场景**：`module_init` 里申请，`module_exit` 里 `unregister`。

```c
// 老式一步到位封装（初学者友好，内部已含 cdev）
int register_chrdev(unsigned int major, const char *name, const struct file_operations *fops);
void unregister_chrdev(unsigned int major, const char *name);
```
- **作用**：一次性完成「申请设备号 + cdev_init + cdev_add」。主号传 0 让内核分配。
- **场景**：只创建一个字符设备的简单驱动；正式多设备驱动建议用下面的 cdev 流程。

### 1.2 cdev 结构体（正式做法）

```c
void cdev_init(struct cdev *cdev, const struct file_operations *fops);
int  cdev_add(struct cdev *cdev, dev_t dev, unsigned count);
void cdev_del(struct cdev *cdev);
struct cdev *cdev_alloc(void);   // 动态分配 cdev（否则用 static 全局）
```
- **作用**：把一个 `struct cdev` 与 `file_operations` 绑定，并注册到系统，使 `/dev/xxx` 可读写。
- **场景**：
  ```c
  static dev_t devno;
  cdev_init(&my_cdev, &my_fops);
  my_cdev.owner = THIS_MODULE;
  cdev_add(&my_cdev, devno, 1);
  ```

### 1.3 创建设备节点（/dev 下可见）

```c
struct class *class_create(struct module *owner, const char *name);   // 5.0.17 仍是两参数！
void class_destroy(struct class *cls);

struct device *device_create(struct class *cls, struct device *parent,
                              dev_t devt, void *drvdata, const char *fmt, ...);
void device_destroy(struct class *cls, dev_t devt);
```
- **作用**：`class_create` 建一个 `/sys/class/xxx`；`device_create` 在 class 下建设备，udev/mdev 据此自动在 `/dev` 生成节点。
- **场景**：probe/init 末尾 `device_create`，remove/exit 里 `device_destroy` + `class_destroy`。
- **注意**：`class_create(THIS_MODULE, "mydev")` —— 5.0.17 的 owner 参数**不能省略**（新版内核才改为单参数）。

### 1.4 file_operations 关键成员

```c
struct file_operations {
    struct module *owner;
    int  (*open)(struct inode *, struct file *);
    int  (*release)(struct inode *, struct file *);
    ssize_t (*read)(struct file *, char __user *, size_t, loff_t *);
    ssize_t (*write)(struct file *, const char __user *, size_t, loff_t *);
    long (*unlocked_ioctl)(struct file *, unsigned int, unsigned long);
    int  (*mmap)(struct file *, struct vm_area_struct *);
    __poll_t (*poll)(struct file *, struct poll_table_struct *);
};
```
- 用户态与内核态数据拷贝用 `copy_to_user` / `copy_from_user`（`<linux/uaccess.h>`）。
- `filp->private_data` 在 `open` 里保存上下文（如 `struct mydev *`），后续 read/write 取回。
- `iminor(inode)` 取次设备号；`container_of` 反查结构体。

---

## 2. Input 输入子系统

头文件：`<linux/input.h>`、`<linux/input-polldev.h>`（轮询设备）

```c
struct input_dev *input_allocate_device(void);
void input_free_device(struct input_dev *dev);          // 注册失败时释放
int  input_register_device(struct input_dev *dev);
void input_unregister_device(struct input_dev *dev);

void input_set_capability(struct input_dev *dev, unsigned int type, unsigned int code);
void input_set_drvdata(struct input_dev *dev, void *data);
void *input_get_drvdata(struct input_dev *dev);

void input_report_key(struct input_dev *dev, unsigned int code, int value);
void input_report_abs(struct input_dev *dev, unsigned int code, int value);
void input_report_rel(struct input_dev *dev, unsigned int code, int value);
void input_sync(struct input_dev *dev);                 // 一次事件上报完必调
```
- **作用**：把按键/相对/绝对坐标等事件递交给 input 子系统，由它合成 `/dev/input/eventX`。
- **标准流程**：
  ```c
  struct input_dev *idev = input_allocate_device();
  idev->name = "my_keys";
  idev->id.bustype = BUS_HOST;
  input_set_capability(idev, EV_KEY, KEY_0);            // 声明支持 KEY_0
  input_set_drvdata(idev, priv);
  input_register_device(idev);

  // 中断/轮询里：
  input_report_key(idev, KEY_0, 1);                     // 按下
  input_sync(idev);                                     // 同步
  input_report_key(idev, KEY_0, 0);                     // 抬起
  input_sync(idev);
  ```
- **场景**：gpio-keys、触摸屏、鼠标、键盘等一切人机输入。键码见 `<linux/input-event-codes.h>`（`KEY_0=11`、`BTN_0` 等）。
- **注意**：`input_allocate_device` 后若 `input_register_device` 失败，必须 `input_free_device`；注册成功后再失败要用 `input_unregister_device`。

---

## 3. GPIO / GPIOD（基于描述符的消费者接口）

头文件：`<linux/gpio/consumer.h>`（**不要**再用旧版 `<linux/gpio.h>` 的 `gpio_request`）

### 3.1 获取 GPIO 描述符

```c
struct gpio_desc *gpiod_get(struct device *dev, const char *con_id, enum gpiod_flags flags);
struct gpio_desc *gpiod_get_optional(struct device *dev, const char *con_id, enum gpiod_flags flags);
struct gpio_desc *gpiod_get_index(struct device *dev, const char *con_id, unsigned int index, enum gpiod_flags flags);

struct gpio_desc *devm_gpiod_get(struct device *dev, const char *con_id, enum gpiod_flags flags);
struct gpio_desc *devm_gpiod_get_optional(struct device *dev, const char *con_id, enum gpiod_flags flags);
struct gpio_desc *devm_gpiod_get_index(struct device *dev, const char *con_id, unsigned int index, enum gpiod_flags flags);

struct gpio_desc *devm_gpiod_get_from_of_node(struct device *dev, struct device_node *np,
                                              const char *propname, int index, enum gpiod_flags flags);
```
- **`con_id` 与设备树属性的对应**：DT 里属性名是 `my-gpios` → `con_id="my"`；属性名是 `gpios` → `con_id=NULL`。
- **`flags`**：`GPIOD_IN` / `GPIOD_OUT_LOW` / `GPIOD_OUT_HIGH` / `GPIOD_ASIS` / `GPIOD_ACTIVE_LOW`。`devm_*` 版本在设备解绑时自动 `gpiod_put`。
- **`_optional`**：DT 没配该 GPIO 时返回 `NULL`（不报错），适合「可有可无」的引脚。
- **`devm_gpiod_get_from_of_node`**：直接从某个 `device_node`（如子节点）读 `gpios` 属性——你 `my_gpio_keys.c` 用的就是它。
- **场景**：驱动需要某个 GPIO 时，优先用 `devm_gpiod_get_*`，把 `flags` 设成期望的初始方向/电平。

### 3.2 方向、读写、中断

```c
int  gpiod_direction_input(struct gpio_desc *desc);
int  gpiod_direction_output(struct gpio_desc *desc, int value);
int  gpiod_get_value(const struct gpio_desc *desc);
void gpiod_set_value(struct gpio_desc *desc, int value);
int  gpiod_to_irq(const struct gpio_desc *desc);        // 关键：把 GPIO 转成 IRQ 号
int  gpiod_set_consumer_name(struct gpio_desc *desc, const char *name);

// 可能睡眠的变体（I2C/SPI 扩展 GPIO 必须用它）
int  gpiod_get_value_cansleep(const struct gpio_desc *desc);
void gpiod_set_value_cansleep(struct gpio_desc *desc, int value);
```
- **作用**：设置方向、读引脚电平、写输出、把 GPIO 映射成中断号。
- **`gpiod_to_irq` 的坑（你已踩过）**：只有**带 irqchip 的 GPIO 控制器**才返回有效 IRQ；vexpress-a9 上 sysreg 的 bgpio（`sys_led`/`sys_mci`/`sys_flash`）**没有 `to_irq`**，返回 `-ENOSYS`，按键事件来不了。带中断的 GPIO（如 PL061）在 QEMU vexpress-a9 上**未建模**。学习 IRQ 请用 `gpio-mockup` 模块。
- **场景**：
  ```c
  int irq = gpiod_to_irq(desc);
  if (irq < 0) { /* 该 GPIO 不支持中断 */ }
  devm_request_irq(&pdev->dev, irq, my_isr, IRQF_TRIGGER_FALLING, "my_key", priv);
  ```

---

## 4. 平台驱动（Platform Driver）

头文件：`<linux/platform_device.h>`

```c
struct platform_driver {
    int  (*probe)(struct platform_device *);
    int  (*remove)(struct platform_device *);
    void (*shutdown)(struct platform_device *);
    struct device_driver driver;
    const struct of_device_id *of_match_table;   // 设备树匹配表
};

int  platform_driver_register(struct platform_driver *);
void platform_driver_unregister(struct platform_driver *);
#define module_platform_driver(__platform_driver)   // 等价于 register+unregister 的模块样板
```
- **作用**：注册一个平台驱动，内核把「与 `of_match_table` 匹配的设备树节点」逐个调 `probe`。
- **场景**：几乎所有片上外设驱动的标准入口，配 `module_platform_driver(my_driver)` 即可。

```c
int platform_get_irq(struct platform_device *pdev, unsigned int index);
int platform_get_irq_byname(struct platform_device *pdev, const char *name);
struct resource *platform_get_resource(struct platform_device *pdev,
                                       unsigned int type, unsigned int index);
struct resource *platform_get_resource_byname(struct platform_device *pdev,
                                              unsigned int type, const char *name);
```
- **作用**：从设备树节点的 `interrupts` / `reg` 属性取 IRQ 号 / 内存资源。
- **`platform_get_irq` 失败返回负值**（不是 0），必须判 `< 0`。
- **注意**：5.0.17 没有 `devm_platform_get_irq`，取得 IRQ 后用 `devm_request_irq(&pdev->dev, irq, ...)` 托管。
- **场景**：
  ```c
  irq = platform_get_irq(pdev, 0);
  res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
  base = devm_ioremap_resource(&pdev->dev, res);   // 一步拿到映射后的虚拟地址
  ```

### of_device_id 匹配表

```c
static const struct of_device_id my_of_match[] = {
    { .compatible = "vendor,my-dev" },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, my_of_match);
```
- **`compatible` 字符串必须与设备树节点的 `compatible` 完全一致**（含厂商前缀）。

---

## 5. 设备树（Device Tree）读取

头文件：`<linux/of.h>`、`<linux/of_address.h>`、`<linux/of_irq.h>`

```c
int  of_property_read_u32(const struct device_node *np, const char *propname, u32 *out_value);
int  of_property_read_u64(const struct device_node *np, const char *propname, u64 *out_value);
int  of_property_read_string(const struct device_node *np, const char *propname, const char **out_string);
bool of_property_read_bool(const struct device_node *np, const char *propname);
int  of_property_read_u32_array(const struct device_node *np, const char *propname, u32 *out_values, size_t sz);
int  of_property_read_u32_index(const struct device_node *np, const char *propname, unsigned int index, u32 *out_value);

struct device_node *of_find_node_by_path(const char *path);
struct device_node *of_parse_phandle(const struct device_node *np, const char *phandle_name, int index);
```
- **作用**：在 `probe` 里读取节点属性值。全部返回 `0` 表示成功，负数表示属性缺失/类型不符。
- **`of_property_read_bool`** 返回 `bool`——属性**存在**即为真（不用赋值）。场景：开关型配置 `my-feature;`。
- **`of_parse_phandle`**：解析 `phandle` 引用（如 `gpios = <&ctrl 0 0>` 里的 `&ctrl`），拿到被引用节点的 `device_node *`，可用于跨节点取资源。
- **场景**：
  ```c
  of_property_read_u32(np, "reg-offset", &off);
  if (of_property_read_bool(np, "inverted"))
      ...
  ```

### 在你的 overlay 里加节点（复刻你 my_gpio_keys 的场景）

```dts
my_gpio_keys {
    compatible = "my,gpio-keys";
    status = "okay";
    key0 {
        label = "my_key0";
        linux,code = <11>;          // KEY_0（裸数字，因为 build 时没加 dt-bindings include）
        gpios = <&sys_mci 0 0>;     // phandle 指向真实 GPIO 控制器；vexpress 上 sys_mci 无 to_irq
        debounce-interval-ms = <30>;
    };
};
```
- **实战结论**：vexpress-a9 上 `gpios` 只能让驱动 `probe` 绑定成功，但拿不到 IRQ、也无物理按键，**产生不了事件**。要真测「ISR + 去抖」，走 `gpio-mockup` 模块 + 给驱动加「模块参数指定 GPIO 号」分支。

---

## 6. 内存分配

头文件：`<linux/slab.h>`、`<linux/vmalloc.h>`、`<linux/device.h>`

```c
void *kmalloc(size_t size, gfp_t flags);
void *kzalloc(size_t size, gfp_t flags);             // = kmalloc + 清零
void *kcalloc(size_t n, size_t size, gfp_t flags);  // 数组版 kzalloc
void  kfree(const void *ptr);

void *devm_kzalloc(struct device *dev, size_t size, gfp_t flags);  // 5.0.17 在 <linux/device.h>
void  devm_kfree(struct device *dev, void *ptr);
```
- **`flags`**：进程上下文用 `GFP_KERNEL`；中断上下文（ISR）用 `GFP_ATOMIC`。
- **`kzalloc`**：小内存（几 KB 以内）首选，零初始化避免脏数据。
- **`devm_kzalloc`**：绑定设备生命周期，probe 失败/卸载自动释放，`priv` 结构体首选。

```c
void *vmalloc(unsigned long size);   // 大块、可非连续；不能用于 DMA
void  vfree(void *addr);

// DMA 一致性内存（外设能直接访问，无 cache 同步问题）
void *dma_alloc_coherent(struct device *dev, size_t size, dma_addr_t *handle, gfp_t flags);
void  dma_free_coherent(struct device *dev, size_t size, void *cpu_addr, dma_addr_t handle);

// 页分配
unsigned long __get_free_pages(gfp_t flags, unsigned int order);
void free_pages(unsigned long addr, unsigned int order);
```
- **场景**：`kzalloc` 给驱动私有数据；`vmalloc` 给大缓冲（如帧缓冲软件副本）；`dma_alloc_coherent` 给外设 DMA 描述符/缓冲；帧缓冲显示用 `devm_ioremap_resource` 映射寄存器、像素走 CMA（见你 LCD 显示那条线）。

---

## 7. 调试打印

头文件：`<linux/printk.h>`、`<linux/device.h>`

### 7.1 通用 printk 级

```c
pr_emerg / pr_alert / pr_crit / pr_err / pr_warn / pr_notice / pr_info / pr_debug("fmt", ...);
pr_cont("fmt", ...);   // 续接到上一行（不带头）
```
- 级别宏：`KERN_ERR` 等；`pr_*` 已自带级别。
- **`pr_debug` / `pr_devel`** 默认**不输出**，需满足：定义 `DEBUG` 宏，或开启 `CONFIG_DYNAMIC_DEBUG` 后 `echo -n 'file xxx.c +p' > /sys/kernel/debug/dynamic_debug/control`。
- 文件顶部可定义 `#define pr_fmt(fmt) "%s:%d: " fmt, __func__, __LINE__` 自动带函数行号。

### 7.2 带 device 的打印（首选）

```c
dev_emerg / dev_alert / dev_crit / dev_err / dev_warn / dev_notice / dev_info / dev_dbg(dev, "fmt", ...);
```
- **作用**：自动带出 `device` 名（如 `my_gpio_keys my_gpio_keys: ...`），定位是哪个设备打的，比 `pr_*` 清晰。
- **`dev_dbg`** 同样是动态调试，受 `DEBUG`/`CONFIG_DYNAMIC_DEBUG` 控制；`dev_info` 默认可见，适合打 probe 成功、版本信息。
- **`dev_err_probe(dev, err, "fmt", ...)`**：**5.11 才引入，5.0.17 没有**；该版本请用 `dev_err(dev, "xxx failed: %d\n", ret); return ret;`。

### 7.3 实战建议

```c
// probe 里：
dev_info(&pdev->dev, "probe ok, irq=%d\n", irq);
if (ret) {
    dev_err(&pdev->dev, "gpiod_get failed: %d\n", ret);
    return ret;
}
// 调试临时打开：
dev_dbg(&pdev->dev, "val=%d\n", v);   // 需开 dynamic_debug
```
- 跑起来后用 `dmesg | tail` 看；在 QEMU 串口控制台直接可见。

---

## 8. 一张速查表（写驱动时对照）

| 想做啥 | 调哪个 | devm 版 |
|---|---|---|
| 申请设备号 | `alloc_chrdev_region` | — |
| 注册 cdev | `cdev_add` | 无（用 `devm_add_action_or_reset` 托管） |
| 建 /dev 节点 | `class_create`+`device_create` | — |
| 建 input 设备 | `input_allocate_device`+`input_register_device` | — |
| 取 GPIO | `gpiod_get` | `devm_gpiod_get` |
| GPIO→IRQ | `gpiod_to_irq` | —（再配 `devm_request_irq`） |
| 平台 IRQ | `platform_get_irq` | 无（配 `devm_request_irq`） |
| 平台内存 | `platform_get_resource` | `devm_ioremap_resource` |
| 读 DT 属性 | `of_property_read_u32/string/bool` | — |
| 分配内存 | `kzalloc` | `devm_kzalloc` |
| 打错误 | `dev_err(dev, ...)` | — |

> 校验依据：以上签名均对照本地 `file/linux-5.0.17` 源码（`include/linux/{device,cdev,gpio/consumer,platform_device,of,slab,io,interrupt,input}.h`）核实；涉及版本差异处已在文中标注（5.0.17 无 `devm_platform_get_irq` / `devm_cdev_add` / `dev_err_probe`，`class_create` 仍为两参数）。

---

## 9. 中断与下半部（Interrupt & Bottom Half）

头文件：`<linux/interrupt.h>`、`<linux/workqueue.h>`、`irqreturn_t` 类型同文件。

### 9.1 注册中断（ISR 写法）

```c
typedef irqreturn_t (*irq_handler_t)(int irq, void *dev_id);
// ISR 返回：IRQ_NONE（不是我的中断）/ IRQ_HANDLED（已处理）/ IRQ_WAKE_THREAD（转线程化）
int  request_irq(unsigned int irq, irq_handler_t handler,
                 unsigned long flags, const char *name, void *dev_id);
void free_irq(unsigned int irq, void *dev_id);
// devm 版（推荐）：
int  devm_request_irq(struct device *dev, unsigned int irq, irq_handler_t handler,
                      unsigned long flags, const char *name, void *dev_id);
```
- **`flags`（`IRQF_*`）**：`IRQF_TRIGGER_RISING/FALLING/BOTH/LOW/HIGH`（边沿/电平触发）、`IRQF_SHARED`（多个设备共享同一 IRQ，此时 `dev_id` 必须唯一）。
- **`dev_id`**：在 `IRQF_SHARED` 下用于区分设备；非共享也建议传 `priv` 指针，ISR 里 `container_of` 取回。
- **ISR 约束**：不能睡眠（`GFP_ATOMIC`、不能 `msleep`/`mutex_lock`）、越快越好。耗时活儿丢给下半部（tasklet/workqueue）。

### 9.2 tasklet（软中断下半部，5.0.17 旧式 API）

```c
// 5.0.17 仍是三参旧式！（新版 5.7+ 改为 tasklet_setup 单参，别抄新文档）
void tasklet_init(struct tasklet_struct *t, void (*func)(unsigned long), unsigned long data);
#define DECLARE_TASKLET(name, func, data)  struct tasklet_struct name = { ..., func, data }
void tasklet_schedule(struct tasklet_struct *t);   // 标记待运行（软中断上下文）
void tasklet_kill(struct tasklet_struct *t);        // remove 里调用，等其跑完
```
- **作用**：把非紧急活儿丢到软中断（TASKLET_SOFTIRQ）里跑，同 tasklet 串行、不同 tasklet 可并行于不同 CPU。运行在**中断上下文，仍不能睡眠**。
- **场景**：ISR 里 `tasklet_schedule(&priv->tlet)`，在 `tlet_func` 里做数据搬运/上报。

### 9.3 workqueue（进程上下文下半部）

```c
// 5.0.17 仍是两参旧式！（新版改成 INIT_WORK 单参宏，别抄新文档）
#define INIT_WORK(_work, _func)            // _func: void (*)(struct work_struct *)
#define DECLARE_WORK(n, f)
bool schedule_work(struct work_struct *work);          // 扔进 system_wq
bool queue_work(struct workqueue_struct *wq, struct work_struct *work);
extern struct workqueue_struct *system_wq;             // 默认全局队列
```
- **作用**：work 函数跑在**进程上下文（kworker 线程）**，**可以睡眠**（能用 `mutex`/`msleep`）。
- **场景**：需要睡眠的下半部（如读 I2C 取值、唤醒用户态 waitqueue）用 workqueue；纯计算/快速搬运用 tasklet。
- **对比**：tasklet 快但限制多（不能睡）；workqueue 灵活但要上下文切换开销。

### 9.4 实战（ISR → tasklet → 上报）

```c
static irqreturn_t my_isr(int irq, void *id) {
    struct mydev *p = id;
    tasklet_schedule(&p->tlet);     // 紧急外的活儿丢下半部
    return IRQ_HANDLED;
}
static void my_tasklet(unsigned long data) {
    struct mydev *p = (void *)data;
    input_report_key(p->idev, p->code, 1); input_sync(p->idev);
}
// probe: tasklet_init(&p->tlet, my_tasklet, (unsigned long)p);
// remove: tasklet_kill(&p->tlet);
```

---

## 10. 定时器与延时（Timer & Delay）

头文件：`<linux/timer.h>`、`<linux/delay.h>`、`<linux/ktime.h>`

### 10.1 内核定时器（用于防抖、超时）

```c
// 5.0.17 用 timer_setup（4.15+ 引入，替代旧 init_timer + 手动挂 data）
#define timer_setup(timer, callback, flags)   // callback: void (*)(struct timer_list *)
int  mod_timer(struct timer_list *timer, unsigned long expires);   // 启动/续期（jiffies 基准）
int  del_timer(struct timer_list *timer);
int  del_timer_sync(struct timer_list *timer);   // 等可能在跑的定时器结束（更稳）
int  timer_pending(const struct timer_list *timer);
```
- **`expires`**：绝对 jiffies 值，通常 `jiffies + msecs_to_jiffies(30)`（30ms 防抖）。
- **回调里**：若需周期触发，再 `mod_timer(...)` 续期。
- **你 gpio_keys 的防抖**就用这条：`IRQF_TRIGGER_*` 中断里 `mod_timer(&p->deb, jiffies+msecs_to_jiffies(debounce))`，定时器回调里再 `input_report_key`。
- **注意**：定时器回调跑在**软中断上下文，不能睡眠**；要睡眠用 workqueue。

### 10.2 忙等延时（ISR / 自旋上下文，不睡）

```c
void mdelay(unsigned long msecs);
void udelay(unsigned long usecs);
void ndelay(unsigned long nsecs);
```
- **作用**：纯忙等，占 CPU 但可在中断上下文用。只在极短延时（≤ 几 ms）用，别在进程上下文长延时。

### 10.3 可睡眠延时（进程上下文）

```c
void msleep(unsigned int msecs);
unsigned long msleep_interruptible(unsigned int msecs);   // 可被信号打断
void ssleep(unsigned int seconds);
void usleep_range(unsigned long min, unsigned long max);  // 推荐替代 mdelay 的短睡
```
- 只能在**进程上下文**调用（不能 ISR / tasklet / 持自旋锁时）。

### 10.4 时间获取

```c
u64 ktime_get_ns(void);          // 单调递增纳秒
ktime_t ktime_get(void);
unsigned long get_jiffies_64(void);
```
- 相对计时优先用 `ktime_get_ns()`；涉及 `mod_timer` 用 jiffies。

---

## 11. 并发与同步（Locking）

头文件：`<linux/spinlock.h>`、`<linux/mutex.h>`、`<linux/atomic.h>`、`<linux/completion.h>`

### 11.1 自旋锁 spinlock（不可睡眠，ISR 可用）

```c
spinlock_t lock;
spin_lock_init(&lock);
void spin_lock(spinlock_t *lock);
void spin_unlock(spinlock_t *lock);
unsigned long spin_lock_irqsave(spinlock_t *lock, unsigned long flags); // 关本地中断+加锁
void spin_unlock_irqrestore(spinlock_t *lock, unsigned long flags);     // 恢复
void spin_lock_bh(spinlock_t *lock);   // 关软中断（bottom half）
void spin_unlock_bh(spinlock_t *lock);
```
- **场景**：保护简短临界区、ISR 与进程共享的数据（用 `irqsave` 版）。
- **绝不能**在持自旋锁时睡眠（`msleep`/`mutex`/可调度的分配）。

### 11.2 互斥锁 mutex（可睡眠，进程上下文）

```c
struct mutex m;
mutex_init(&m);
void mutex_lock(struct mutex *m);
int  mutex_lock_interruptible(struct mutex *m);
void mutex_unlock(struct mutex *m);
```
- **场景**：保护可能睡眠的较长临界区（如涉及 I2C 读写的配置）。
- **不可**在中断上下文 / 持自旋锁时使用。

### 11.3 原子变量 atomic_t

```c
atomic_t v = ATOMIC_INIT(0);
void atomic_set(atomic_t *v, int i);
int  atomic_read(atomic_t *v);
void atomic_inc(atomic_t *v);  void atomic_dec(atomic_t *v);
void atomic_add(int i, atomic_t *v);  void atomic_sub(int i, atomic_t *v);
int  atomic_cmpxchg(atomic_t *v, int old, int new);
```
- **场景**：简单计数器/标志位（如 open 引用计数、设备忙闲标志），无锁原子操作。

### 11.4 完成量 completion（等待某个事件完成）

```c
struct completion done;
init_completion(&done);
void complete(struct completion *done);          // 唤醒一个等待者
void complete_all(struct completion *done);      // 唤醒所有
unsigned long wait_for_completion_timeout(struct completion *done, unsigned long timeout);
void wait_for_completion(struct completion *done);
```
- **场景**：probe 里等固件就绪/等中断到来、线程退出时 `wait_for_completion(&thread_exit)`，线程里 `complete()`。

### 11.5 怎么选

| 需求 | 选 |
|---|---|
| ISR 与进程共享、临界区极短 | `spin_lock_irqsave` |
| 进程上下文、临界区可能睡眠 | `mutex` |
| 简单计数/标志 | `atomic_t` |
| 等一个异步事件（中断/线程）完成 | `completion` |
| 等数据就绪（多等待者、可 poll） | `waitqueue`（见第 16 章） |

---

## 12. misc 设备（最简字符设备框架）

头文件：`<linux/miscdevice.h>`

```c
struct miscdevice {
    int minor;                       // 设 MISC_DYNAMIC_MINOR 让内核分配
    const char *name;               // /dev/ 下节点名
    const struct file_operations *fops;
    struct device *parent;          // 可选
    /* ... */
};
#define MISC_DYNAMIC_MINOR  255

int  misc_register(struct miscdevice *misc);
void misc_deregister(struct miscdevice *misc);
```
- **作用**：misc 设备主设备号固定为 **10**，内核自动在 `/sys/class/misc/<name>` 和 `/dev/<name>` 建节点。**省掉** `alloc_chrdev_region` + `cdev_init/add` + `class_create` + `device_create` 全套样板。
- **场景**：只有一个字符设备的简单驱动（调试口、配置口）首选。
- **用法**：
  ```c
  static struct miscdevice my_misc = {
      .minor = MISC_DYNAMIC_MINOR, .name = "mydev", .fops = &my_fops,
  };
  misc_register(&my_misc);   // probe/init
  misc_deregister(&my_misc); // remove/exit
  ```

---

## 13. sysfs 属性文件（驱动导出参数/调试口）

头文件：`<linux/device.h>`、`<linux/sysfs.h>`

```c
// 定义属性（_show 读、_store 写；RW/RO/WO 三选一）
#define DEVICE_ATTR(_name, _mode, _show, _store)
#define DEVICE_ATTR_RW(_name)   // = __ATTR_RW(_name)，自动 dev_attr_##_name
#define DEVICE_ATTR_RO(_name)
#define DEVICE_ATTR_WO(_name)

// show/store 原型：
ssize_t my_show(struct device *dev, struct device_attribute *attr, char *buf);
ssize_t my_store(struct device *dev, struct device_attribute *attr,
                 const char *buf, size_t count);

int  device_create_file(struct device *device, const struct device_attribute *entry);
void device_remove_file(struct device *dev, const struct device_attribute *entry);
```
- **作用**：在 `/sys/devices/.../<name>` 下生成可读写属性，用户态 `cat`/`echo` 即可交互（比 `/dev` ioctl 简单）。
- **写 show**：用 `return scnprintf(buf, PAGE_SIZE, "%d\n", val);`（**别用 `sprintf` 直接写 buf**，可能越界；5.0.17 还没有 `sysfs_emit`，那是 5.10 才加）。返回值 = 写入字节数。
- **写 store**：`kstrtoint(buf, 10, &val)` 解析，`return count;`（成功）或负错误。
- **场景**：驱动开关、阈值（如防抖 ms）、运行参数导出。
  ```c
  static DEVICE_ATTR_RW(debounce_ms);
  device_create_file(dev, &dev_attr_debounce_ms);
  ```

---

## 14. procfs 属性文件（/proc 下调试口）

头文件：`<linux/proc_fs.h>`

```c
// 5.0.17 仍用 struct file_operations！（6.10+ 改成 struct proc_ops，别抄新文档）
struct proc_dir_entry *proc_create(const char *name, umode_t mode,
                                   struct proc_dir_entry *parent,
                                   const struct file_operations *proc_fops);
struct proc_dir_entry *proc_mkdir(const char *name, struct proc_dir_entry *parent);
void proc_remove(struct proc_dir_entry *entry);
void remove_proc_entry(const char *name, struct proc_dir_entry *parent);
```
- **作用**：在 `/proc` 下建只读/读写调试文件，常配 `seq_file` 输出多行。
- **场景**：驱动状态 dump、统计信息。简单驱动优先用 sysfs（第 13 章）；多行表格数据用 procfs + `seq_file`。
- **注意**：`parent` 传 `NULL` 即建在 `/proc` 根下；模块卸载务必 `proc_remove` 防悬空。

---

## 15. file_operations 完整集（用户态接口）

头文件：`<linux/fs.h>`、`<linux/uaccess.h>`

```c
struct file_operations {
    struct module *owner;
    loff_t (*llseek) (struct file *, loff_t, int);
    ssize_t (*read)  (struct file *, char __user *, size_t, loff_t *);
    ssize_t (*write) (struct file *, const char __user *, size_t, loff_t *);
    __poll_t (*poll) (struct file *, struct poll_table_struct *);
    long (*unlocked_ioctl) (struct file *, unsigned int, unsigned long);
    long (*compat_ioctl)   (struct file *, unsigned int, unsigned long); // 32 位兼容
    int  (*mmap)  (struct file *, struct vm_area_struct *);
    int  (*open)  (struct inode *, struct file *);
    int  (*release)(struct inode *, struct file *);
};
```
- **`read`**：`copy_to_user(buf, src, n)`；返回实际拷贝字节数，`*off` 推进，0 表示 EOF。
- **`write`**：`copy_from_user(dst, buf, n)`；返回 `n` 或负错误。
- **`unlocked_ioctl`**：替代老的 `ioctl`（无 BKL）；命令号用 `_IO/_IOR/_IOW/_IORW` 构造。
- **`compat_ioctl`**：32 位用户态程序跑在 64 位内核时的兼容入口，命令号要与 `unlocked_ioctl` 一致。
- **用户/内核数据拷贝**：`copy_to_user` / `copy_from_user`（`<linux/uaccess.h>`），返回值非 0 表示未拷贝字节数（失败）。
- **`filp->private_data`**：`open` 里存 `struct mydev *`，后续 read/write 取回。

---

## 16. 阻塞 IO 与 waitqueue

头文件：`<linux/wait.h>`、`<linux/poll.h>`

### 16.1 等待队列

```c
typedef struct wait_queue_head wait_queue_head_t;
void init_waitqueue_head(wait_queue_head_t *wq_head);
#define DECLARE_WAIT_QUEUE_HEAD(name) ...   // 全局静态声明

// 睡眠等待条件成立（条件为假时让出 CPU）
void wait_event(wait_queue_head_t *wq, condition);               // 不可中断
int  wait_event_interruptible(wait_queue_head_t *wq, condition); // 可被信号打断
long wait_event_timeout(wq, condition, timeout);                 // 超时返回

// 唤醒
void wake_up(wait_queue_head_t *wq);            // 唤醒所有可运行等待者
void wake_up_interruptible(wait_queue_head_t *wq);
```
- **`condition`** 是任意 C 布尔表达式（如 `!kfifo_is_empty(&q)`），被唤醒后会**重新判断**，为假继续睡。

### 16.2 poll 实现

```c
void poll_wait(struct file *filp, wait_queue_head_t *wait_address, poll_table *wait);
```
- 在 `poll` 回调里对关心的队列调用 `poll_wait`，返回 `EPOLLIN/EPOLLOUT` 等掩码：
  ```c
  static __poll_t my_poll(struct file *f, struct poll_table_struct *p) {
      struct mydev *d = f->private_data;
      poll_wait(f, &d->rd_wq, p);
      return kfifo_len(&d->fifo) ? EPOLLIN | EPOLLRDNORM : 0;
  }
  ```

### 16.3 实战（读阻塞 + 中断唤醒）

```c
// read: 没数据就睡
static ssize_t my_read(...) {
    if (wait_event_interruptible(dev->rd_wq, !kfifo_is_empty(&dev->fifo)))
        return -ERESTARTSYS;
    // 取数据 copy_to_user ...
}
// ISR / 写路径：放入数据后唤醒
kfifo_put(&dev->fifo, val);
wake_up_interruptible(&dev->rd_wq);
```

---

## 17. regmap 与 clk（外设寄存器访问 / 时钟）

头文件：`<linux/regmap.h>`、`<linux/clk.h>`、I2C 用 `<linux/i2c.h>`

### 17.1 时钟 clk

```c
struct clk *devm_clk_get(struct device *dev, const char *id);   // id 对应 DT clocks 名
int  clk_prepare_enable(struct clk *clk);    // 准备+使能（返回 0 成功）；进程上下文
void clk_disable_unprepare(struct clk *clk);
unsigned long clk_get_rate(struct clk *clk); // Hz
// 非 devm: clk_get(dev, id) / clk_put(clk)
```
- **场景**：使能外设时钟（如 PL061 的 apb_pclk）。vexpress-a9 上设备树时钟来自 FPGA，`devm_clk_get` + `clk_prepare_enable` 即可。

### 17.2 regmap（I2C/SPI 寄存器统一抽象）

```c
struct regmap *devm_regmap_init_i2c(struct i2c_client *i2c,
                                    const struct regmap_config *config);
int regmap_read(struct regmap *map, unsigned int reg, unsigned int *val);
int regmap_write(struct regmap *map, unsigned int reg, unsigned int val);
int regmap_update_bits(struct regmap *map, unsigned int reg,
                       unsigned int mask, unsigned int val);
```
- **作用**：屏蔽 I2C/SPI/MMIO 差异，统一「按寄存器地址读写」。自动处理端序、锁、cache。
- **`regmap_config` 必要字段**：`reg_bits`（通常 8/16）、`val_bits`（8/16/32）、`max_register`。
- **场景**：所有 I2C/SPI 芯片（传感器、PMIC、codec、触摸屏控制器）驱动的标准做法，比裸 `i2c_transfer` 简洁得多。
  ```c
  static const struct regmap_config my_regmap_cfg = {
      .reg_bits = 8, .val_bits = 8, .max_register = 0x7F,
  };
  regmap = devm_regmap_init_i2c(client, &my_regmap_cfg);
  regmap_read(regmap, REG_ID, &v);
  ```

---

## 18. 完整速查表（1–17 汇总）

| 分类 | 想做啥 | API | devm 版 / 备注 |
|---|---|---|---|
| 字符设备 | 申请设备号 | `alloc_chrdev_region` | — |
| 字符设备 | 释放设备号 | `unregister_chrdev_region` | 配 `alloc_chrdev_region` |
| 字符设备 | 注册 cdev | `cdev_add` | 无 devm（用 `devm_add_action_or_reset` 包 `cdev_del`） |
| 字符设备 | 注销 cdev | `cdev_del` | 配 `cdev_add` |
| 字符设备 | 最简字符设备 | `misc_register` | —（主号 10） |
| 字符设备 | 注销最简设备 | `misc_deregister` | 配 `misc_register` |
| 字符设备 | 建 /dev 节点 | `class_create`+`device_create` | —（5.0.17 `class_create` 两参） |
| 字符设备 | 删 /dev 节点 | `device_destroy`+`class_destroy` | 配 `class_create`+`device_create` |
| Input | 建/注册输入设备 | `input_allocate_device`+`input_register_device` | — |
| Input | 注销输入设备 | `input_unregister_device` | 配 `input_register_device` |
| Input | 上报按键 | `input_report_key`+`input_sync` | — |
| GPIO | 取 GPIO | `gpiod_get` | `devm_gpiod_get` |
| GPIO | 释放 GPIO | `gpiod_put` | 配 `gpiod_get`；devm 用 `devm_gpiod_get` |
| GPIO | GPIO→IRQ | `gpiod_to_irq` | 再配 `devm_request_irq` |
| 平台 | 平台 IRQ | `platform_get_irq` | 无 devm（5.0.17），配 `devm_request_irq` |
| 平台 | 平台内存 | `platform_get_resource` | `devm_ioremap_resource` |
| 设备树 | 读属性 | `of_property_read_u32/string/bool` | — |
| 内存 | 小内存 | `kzalloc` | `devm_kzalloc` |
| 内存 | 释放小内存 | `kfree` | 配 `kzalloc`；devm 用 `devm_kzalloc` |
| 内存 | 大内存 | `vmalloc` | — |
| 内存 | 释放大内存 | `vfree` | 配 `vmalloc` |
| 内存 | DMA 缓冲 | `dma_alloc_coherent` | `dma_free_coherent` |
| 打印 | 错误 | `dev_err(dev,...)` | —（5.0.17 无 `dev_err_probe`） |
| 中断 | 注册 ISR | `request_irq` | `devm_request_irq` |
| 中断 | 释放 ISR | `free_irq` | `devm_request_irq` 自动释放，无需手动 free |
| 中断 | 下半部(不睡) | `tasklet_schedule` | `tasklet_init` 三参(5.0.17) |
| 中断 | 停 tasklet | `tasklet_kill` | 配 `tasklet_init`；remove 里等其跑完 |
| 中断 | 下半部(可睡) | `schedule_work` | `INIT_WORK` 两参(5.0.17) |
| 中断 | 停 work | `cancel_work_sync` | 配 `INIT_WORK`；remove 里等其跑完 |
| 定时器 | 延时/防抖 | `timer_setup`+`mod_timer` | `del_timer_sync` 清理 |
| 延时 | 忙等 | `udelay`/`mdelay` | 中断上下文 |
| 延时 | 可睡 | `msleep`/`usleep_range` | 进程上下文 |
| 同步 | 短临界/ISR | `spin_lock_irqsave` | — |
| 同步 | 可睡临界区 | `mutex_lock` | — |
| 同步 | 计数/标志 | `atomic_*` | — |
| 同步 | 等异步完成 | `wait_for_completion`/`complete` | — |
| 阻塞 | 等数据 | `wait_event_interruptible` | `wake_up_interruptible` 唤醒 |
| 阻塞 | poll | `poll_wait` | 返回 `EPOLLIN` 等 |
| sysfs | 导出参数 | `DEVICE_ATTR_RW`+`device_create_file` | show 用 `scnprintf` |
| sysfs | 撤导出参数 | `device_remove_file` | 配 `device_create_file` |
| procfs | 调试文件 | `proc_create` | 5.0.17 用 `file_operations` |
| procfs | 删调试文件 | `proc_remove` / `remove_proc_entry` | 配 `proc_create` |
| clk | 使能时钟 | `devm_clk_get`+`clk_prepare_enable` | `clk_disable_unprepare` |
| regmap | 读写寄存器 | `devm_regmap_init_i2c`+`regmap_read/write` | — |

> 新增章节（9–18）签名均对照本地 `file/linux-5.0.17` 源码核实；重点版本坑已标注：
> - **tasklet 仍是 `tasklet_init(t,func,data)` 三参旧式**（5.7+ 才改 `tasklet_setup` 单参）；
> - **workqueue `INIT_WORK(work,func)` 仍是两参**（5.13+ 才改单参宏）；
> - **procfs `proc_create` 在 5.0.17 仍接收 `struct file_operations*`**（6.10+ 才改 `struct proc_ops*`）；
> - **sysfs show 用 `scnprintf`**（5.0.17 无 `sysfs_emit`，5.10+ 才有）；
> - `devm_platform_get_irq` / `devm_cdev_add` / `dev_err_probe` 在 5.0.17 均不存在。
