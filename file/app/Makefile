# ===========================================================================
#  LVGL framebuffer demo - Makefile
#  默认交叉编译到 ARM Linux 开发板（板子上的 LCD 挂在 /dev/fb0）。
#  想在本地 Linux PC（也有 /dev/fb0）编译运行：make CC=gcc
# ===========================================================================

# 1) 工具链（命令行可覆盖：make CC=gcc）
CC := arm-linux-gnueabihf-gcc

# 2) 路径
APP_DIR  := .
LVGL_DIR := ../lvgl

# 3) 源文件
APP_SRCS := main.c lv_fb_drv.c network_demo.c flip_demo.c camera_demo.c
#    收集 LVGL src 下所有 .c（core/draw/widgets/extra/font/misc/hal ...）
LVGL_SRCS := $(shell find $(LVGL_DIR)/src -name '*.c')
#    收集内置 demos 下所有 .c（Widgets/Benchmark/Keypad/Stress/Music）
DEMO_SRCS := $(shell find $(LVGL_DIR)/demos -name '*.c')
SRCS := $(APP_SRCS) $(LVGL_SRCS) $(DEMO_SRCS)

# 4) 输出
BUILD_DIR := build
BUILD_DIR_OUT := build/out
BUILD_DIR_BIN := build/bin
TARGET    := $(BUILD_DIR_BIN)/lvgl_demo

# 5) 编译选项
CFLAGS := -Wall -O2 -std=gnu99 -MMD -MP
CFLAGS += -I$(LVGL_DIR) -I$(LVGL_DIR)/demos -I$(APP_DIR)
# LVGL FreeType 集成头 lv_freetype.h 所在目录（main.c 用 #include "lv_freetype.h" 引用）
CFLAGS += -I$(LVGL_DIR)/src/extra/libs/freetype
# TJpgDec 头文件 tjpgd.h 所在目录（camera_demo.c 用 #include "tjpgd.h" 引用，
# 该目录同时含 tjpgdcnf.h；启用开关 LV_USE_SJPG 见 lv_conf.h）
CFLAGS += -I$(LVGL_DIR)/src/extra/libs/sjpg
# FreeType 公共头（armhf 预编译包，LVGL lv_freetype 集成需要 ft2build.h / freetype/*.h）
#   路径相对本 Makefile（file/app/ -> ../../ = linux/ -> freetype/...）。务必从 app 目录 make。
CFLAGS += -I../../freetype/freetype_armhf/usr/include/freetype2
CFLAGS += -DLV_CONF_INCLUDE_SIMPLE

# 目标机 rootfs 精简、缺动态链接器/库时，用 -static 编出自带库的静态二进制。
# ARM 版 libfreetype.a 来自 freetype_armhf 预编译包（相对路径见上，务必从 app 目录 make）。
# 静态链接顺序敏感：被依赖的库必须放在“引用它的库”之后。依赖链：
#   libfreetype -> libbrotlidec -> libbrotlicommon （woff2/brotli 支持，硬依赖）
#   libfreetype -> libpng16 -> libz                 （png/zlib 支持）
#   libpng16/libfreetype 用到的数学函数(pow/floor) 来自 libm，故 -lm 必须放最后。
# 同目录已放齐：libfreetype.a / libpng16.a / libz.a / libbrotlidec.a / libbrotlicommon.a
LIBS := -static -L../../freetype/freetype_armhf/usr/lib/arm-linux-gnueabihf -lpthread -lfreetype -lpng16 -lz -lbrotlidec -lbrotlicommon -lm

# ---------------------------------------------------------------------------
#  为每个源文件生成一条规则：目标 = build/<路径中-斜杠换下划线>.o（扁平存放，不污染源码树）
# ---------------------------------------------------------------------------
OBJS :=

define compile_rule
OBJS += $(BUILD_DIR_OUT)/$(subst /,__,$(1)).o
DEPS += $(BUILD_DIR_OUT)/$(subst /,__,$(1)).d
# 依赖源文件 + 本 Makefile：改 .c / .h / lv_conf.h / 编译选项 / 此文件时自动重编
$(BUILD_DIR_OUT)/$(subst /,__,$(1)).o: $(1) $(MAKEFILE_LIST)
	@mkdir -p $$(dir $$@)
	$$(CC) $$(CFLAGS) -c $$< -o $$@
endef

$(foreach s,$(SRCS),$(eval $(call compile_rule,$(s))))

# 自动依赖：gcc -MMD 生成的 .d 文件（含每个 .c 实际 include 的所有头文件，
# 含 lv_conf.h）。改头文件/配置后直接 make 即可精确重编，无需 make clean。
-include $(DEPS)

# ---------------------------------------------------------------------------
#  目标
# ---------------------------------------------------------------------------
.PHONY: all clean

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# 输出 bin 目录：order-only 依赖 build，保证 build 先于 build/bin 存在
$(BUILD_DIR_BIN): | $(BUILD_DIR)
	mkdir -p $@

# 链接目标：order-only 依赖 build/bin，保证链接前目录一定已建好（并行 make 也安全）
$(TARGET): $(OBJS) | $(BUILD_DIR_BIN)
	$(CC) $(OBJS) -o $@ $(LIBS)
	@chmod +x $@
	@echo ">> 已生成: $@"

clean:
	rm -rf $(BUILD_DIR)
