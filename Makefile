# Makefile — myshell 简易 Linux Shell 命令解释器 (多文件版)
# 操作系统课程设计作业

CC      = gcc
CFLAGS  = -Wall -Wextra -std=c99
TARGET  = myshell
SRCS    = main.c parser.c executor.c builtin.c history.c alias.c redirect.c background.c input.c utils.c mount.c
OBJS    = $(SRCS:.c=.o)

.PHONY: all clean debug install uninstall

# 安装路径
INSTALL_DIR  = /usr/local/bin
INSTALL_PATH = $(INSTALL_DIR)/$(TARGET)

# 默认目标: 编译生成 myshell
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# 通用编译规则: 每个 .c 依赖 shell.h 及对应模块头文件
%.o: %.c shell.h
	$(CC) $(CFLAGS) -c $< -o $@

# 调试版本: 带 -g 调试信息
debug: CFLAGS += -g
debug: clean all

# 安装: 复制二进制到系统路径并注册到 /etc/shells (需 sudo)
install: all
	@echo "正在安装 myshell 到 $(INSTALL_PATH) ..."
	install -d $(INSTALL_DIR)
	install -m 755 $(TARGET) $(INSTALL_PATH)
	@grep -qx '$(INSTALL_PATH)' /etc/shells 2>/dev/null || \
		(echo '$(INSTALL_PATH)' >> /etc/shells && echo "已注册到 /etc/shells")
	@echo "安装完成！可用 'chsh -s $(INSTALL_PATH)' 设为默认 Shell"

# 卸载: 从系统路径移除并取消注册
uninstall:
	rm -f $(INSTALL_PATH)
	@sed -i '\|^$(INSTALL_PATH)$$|d' /etc/shells 2>/dev/null || true
	@echo "已卸载 myshell"

# 清理编译产物
clean:
	rm -f $(OBJS) $(TARGET)
