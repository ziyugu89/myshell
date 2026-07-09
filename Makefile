# Makefile — myshell 简易 Linux Shell 命令解释器 (多文件版)
# 操作系统课程设计作业

CC      = gcc
CFLAGS  = -Wall -Wextra -std=c99
TARGET  = myshell
SRCS    = main.c parser.c executor.c builtin.c history.c alias.c redirect.c background.c input.c utils.c mount.c
OBJS    = $(SRCS:.c=.o)

.PHONY: all clean debug

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

# 清理编译产物
clean:
	rm -f $(OBJS) $(TARGET)
