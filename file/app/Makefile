# 1. 定义交叉编译器
CC = arm-linux-gnueabihf-gcc

# 2. 编译选项
CFLAGS = -static -Wall -lpthread
LIBS = -lpthread  # 库链接放这里
# 3. 目录定义
BUILD_DIR = ./build
SRC = app1.c

# 4. 目标文件路径 (build/app)
TARGET = $(BUILD_DIR)/app

# 默认目标
all: $(BUILD_DIR) $(TARGET)

# 创建 build 目录
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# 编译并链接到 build 目录
$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LIBS)

# 清理目标：删除整个 build 目录
clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean

