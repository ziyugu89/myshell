# 操作系统课程设计 - myshell 简易 Linux Shell 命令解释器

## 项目简介

使用纯 C 语言（C99 + POSIX 扩展）实现的交互式 Shell 命令解释器，模拟 bash 核心功能。采用多文件模块化架构，通过 `fork` + `execvp` 执行外部命令，手动实现管道、重定向、进程组、作业控制与信号处理，**零第三方库依赖**。

本 Shell 在启动时会建立独立进程组并接管终端前台，外部命令子进程拥有独立进程组，前台作业支持 `tcsetpgrp` 切换与 `Ctrl+Z` 停止，后台作业由 `SIGCHLD` 信号与主动轮询双重回收。

---

## 环境要求

| 项目 | 要求 |
|------|------|
| 操作系统 | Ubuntu Linux / WSL（**不支持 Windows 原生编译运行**） |
| 编译器 | gcc（C99 + POSIX 扩展） |
| 依赖 | 无第三方库 |

---

## 编译与运行

```bash
# 方式 1: WSL 路径
cd /mnt/c/Users/32130/Desktop/操作系统大设计

# 方式 2: VMware 虚拟机共享文件夹路径
# (先在 VMware 设置中把 Windows 项目目录设为共享文件夹)
cd /mnt/hgfs/操作系统大设计

make              # 标准编译
make debug        # 带 -g 调试信息
make clean        # 清理 .o 与 myshell

./myshell         # 运行
exit              # 或 Ctrl+D 退出
```

---

## 注册为系统登录 Shell（可选）

Linux 系统通过 `/etc/shells` 文件维护"合法登录 Shell"列表。若要让 myshell 被系统承认（例如通过 `chsh` 切换默认 Shell），需将其绝对路径写入该文件。

### 步骤

```bash
# 一条命令完成编译、安装、注册（需 sudo）
sudo make install

# 查看注册结果
cat /etc/shells
```

### 验证

执行以下命令，若输出包含 `/usr/local/bin/myshell`，说明注册成功：

```bash
grep "myshell" /etc/shells
```

### 切换默认 Shell（可选）

```bash
chsh -s /usr/local/bin/myshell
```

重新登录后，默认 Shell 即为 myshell。

### 卸载

```bash
sudo make uninstall    # 删除二进制 + 从 /etc/shells 移除
```

**注意**：`make install` / `make uninstall` 涉及系统文件修改，需要 root 或当前用户密码权限。

---

## 功能总览

| 类别 | 功能 | 命令/语法 |
|------|------|-----------|
| 内置命令 | 目录操作 | `cd`、`pwd` |
| 内置命令 | 输出/类型/退出 | `echo`、`type`、`exit` |
| 内置命令 | 历史/别名 | `history`、`alias`、`unalias` |
| 内置命令 | 环境变量 | `export`、`unset` |
| 内置命令 | 作业控制 | `jobs`、`fg`、`bg` |
| 内置命令 | 挂载管理 | `mount`、`umount` |
| 外部命令 | 系统程序 | `ls`、`cat`、`grep` 等 |
| 历史展开 | 快速重复 | `!!`、`!n`、`!?关键词` |
| 引号/变量 | 解析与展开 | `'...'`、`"..."`、`$VAR`、`~/path` |
| 通配符 | glob 展开 | `*.txt`、`test?.txt` |
| 重定向 | stdin/stdout/stderr | `<`、`>`、`>>`、`2>`、`2>>`、`2>&1` |
| 管道 | 单级/多级 | `cmd1 \| cmd2 \| ...` |
| 后台/作业 | 不阻塞执行 | `命令 &`、`| ... &` |
| 行编辑 | 交互输入 | ↑↓←→、Tab、Ctrl+C/D/Z |

---

## 命令处理流程

用户输入一行后，myshell 按以下固定顺序处理：

```
用户输入
  → 历史展开 (history / !! / !n / !?关键词)
  → 别名展开 (递归 ≤10 层)
  → 单条命令执行:
      管道检测 → 分词 → ~/$VAR 展开 → glob 展开
      → 后台 & 检测 → 重定向解析 → 内置/外部执行
```

该顺序是核心不变量：别名展开早于分词，因此别名中可以包含多个词或管道。

---

## 内置命令详解

### `cd` — 切换当前工作目录

```bash
cd                  # 进入 $HOME
cd ~                # 进入 $HOME
cd ~/Documents      # 进入家目录子路径
cd -                # 回到上一个目录 ($OLDPWD)
cd ..               # 上级目录
cd /tmp             # 绝对路径
```

**演示说明**：`cd` 必须在 Shell 父进程内执行（不能 fork），否则子进程改目录后 Shell 当前目录不变。本实现使用 `chdir` 直接修改父进程工作目录，并用 `setenv("OLDPWD", ...)` 保存上一个目录，支持 `cd -`。

---

### `pwd` — 显示当前工作目录

```bash
pwd
```

输出当前工作目录的绝对路径。

---

### `echo` — 输出文本

```bash
echo Hello World
echo $HOME
echo "hello world"          # 双引号保留空格，内部展开变量
echo 'hello world'          # 单引号原样输出，不展开变量
echo "HOME=$HOME"           # 双引号内展开 $HOME
echo 'HOME=$HOME'           # 单引号内不展开
echo "hello\nworld"         # 转义换行
echo "col1\tcol2"           # 转义 Tab
echo -n "no newline"        # 不换行输出
```

**实现说明**：`echo` 支持 `-n`（取消末尾换行）以及 `\n`、`\t`、`\\` 转义序列。双引号与无引号参数在分词阶段已完成 `$VAR` 展开，`echo` 接收到的是展开后的字符串；单引号参数保持原样。

---

### `type` — 查询命令类型

判断顺序：**别名 → 内置 → 外部**。

```bash
type cd                     # cd 是 shell 内置命令
type ls                     # ls 是 /usr/bin/ls
type mount                  # mount 是 shell 内置命令

alias ll='ls -la'
type ll                     # ll 是别名: ls -la
```

**注意**：`type` 查询时，输入行的第一个词是 `type` 本身，因此不会被别名展开阶段替换；`type` 内部会主动扫描别名表，故能正确识别别名。

---

### `exit` — 退出 Shell

```bash
exit                        # 退出码 0
exit 1                      # 指定退出码
```

退出前会自动调用 `save_history()`，将本次会话的历史命令写入 `~/.myshell_history`。

---

### `history` — 历史命令

```bash
history                     # 显示全部历史（最多保留 MAX_HISTORY 条）
history 5                   # 显示最近 5 条
!!                          # 执行上一条历史命令
!3                          # 执行历史编号 3 的命令
!?grep                      # 执行最近包含 "grep" 的历史命令
```

**实现说明**：
- 历史记录以环形数组存储，超过最大条数后旧记录被覆盖。
- `!n` 中的 `n` 与 `history` 显示的行号一致。
- 启动时从 `~/.myshell_history` 加载，退出时覆盖写回。
- 只有普通命令会入历史；`history` 自身、`!!`、`!n`、`!?kw` 不会重复入历史。

---

### `alias` / `unalias` — 别名

```bash
alias                       # 列出全部别名
alias ll='ls -la'           # 定义别名
alias la='ls -A'
ll                          # 使用别名，等价于 ls -la
unalias ll                  # 删除别名
```

**递归展开**（最多 10 层，防止死循环）：

```bash
alias a='b'
alias b='ls -la'
a                           # → 最终执行 ls -la
```

**实现说明**：别名在命令处理流程的第二阶段展开，对整行第一个词进行匹配，并把别名替换为其值，保留后续参数。例如 `alias ll='ls -la'` 后执行 `ll /tmp`，会展开为 `ls -la /tmp`。

---

### `export` / `unset` — 环境变量

```bash
export MYVAR=hello          # 设置环境变量
export MYVAR=world          # 覆盖设置
echo $MYVAR                 # world
export                      # 列出所有环境变量
export MYVAR                # 查看单个变量
unset MYVAR                 # 删除变量
```

**实现说明**：`export VAR=value` 通过 `setenv` 设置当前进程环境变量，因此会影响后续外部命令的子进程环境。`unset` 调用 `unsetenv` 删除变量。

---

### `jobs` / `fg` / `bg` — 作业控制

```bash
sleep 100 &                 # 启动后台作业
jobs                        # 查看作业列表
fg                          # 将最近一个作业调到前台
fg %1                       # 将编号 1 的作业调到前台
# Ctrl+Z 可停止前台作业
bg                          # 恢复最近已停止作业到后台
bg %1                       # 恢复编号 1 的作业到后台
```

**演示说明**：
- 后台作业启动时会打印 `[作业 N 已启动] 命令`。
- 作业完成后，在下一次提示符前打印 `[作业 N 已完成, 退出码 X]`，避免打断当前输入。
- `fg` 会把终端前台控制权交给作业进程组，`waitpid` 结束后收回。
- `bg` 向停止的作业进程组发送 `SIGCONT` 继续运行。
- 整个管道（如 `sleep 5 | cat &`）作为一个作业加入作业表。

---

### `mount` / `umount` — 挂载管理（需 root）

```bash
sudo ./myshell              # 如需执行真实挂载/卸载，需 root 权限
mount                       # 显示当前挂载列表（读取 /proc/mounts）
mkdir -p /mnt/usb           # 挂载点目录必须事先存在

# 方式 1：使用 tmpfs（无需真实设备，适合课堂演示）
mount tmpfs /mnt/usb tmpfs
mount | grep /mnt/usb
umount /mnt/usb

# 方式 2：挂载真实块设备（如 U 盘，设备路径需根据实际环境填写）
# mount /dev/sdb1 /mnt/usb vfat
```

**演示说明**：
- 无参数 `mount` 仅读取 `/proc/mounts`（或回退到 `/etc/mtab`），任何用户均可执行。
- 带参数的 `mount` 与 `umount` 调用 Linux `mount()` / `umount()` 系统调用，需要 `geteuid() == 0`。
- 非 root 执行带参数挂载/卸载会提示：`myshell: mount/umount: 需要 root 权限`。

---

## 外部命令

通过 `fork` + `execvp` 执行 `$PATH` 中的程序，禁止 `system()`。

```bash
ls
ls -la
cat test1.txt
grep Linux test3.txt
sort test2.txt
```

外部命令子进程会恢复信号默认行为（`SIG_DFL`），以确保进入 `bash`/`sh` 等子 Shell 后 `Ctrl+C` / `Ctrl+Z` 正常工作。

---

## 引号、变量与路径展开

| 语法 | 行为 | 示例 |
|------|------|------|
| 无引号 | 空格分词；展开 `$VAR` | `echo $PATH` |
| `"..."` | 保留空格；双引号内展开 `$VAR` | `echo "a b"` |
| `'...'` | 全部原样保留，不展开 | `echo '$HOME'` |
| `~/path` | 展开为 `$HOME/path` | `cat ~/file.txt` |
| `\空格` | 转义为空格，合并为一个参数 | `echo one\ two` |

**示例**：

```bash
export NAME=myshell
echo $NAME                  # myshell
echo "NAME=$NAME"           # NAME=myshell
echo 'NAME=$NAME'           # NAME=$NAME
echo ~/test.txt             # /home/user/test.txt
echo one\ two               # one two（一个参数）
```

---

## 通配符（Glob）

```bash
ls *.txt                    # 当前目录所有 .txt
ls test?.txt                # 匹配 test1.txt、testa.txt 等
cat test*.txt
```

**实现说明**：基于 POSIX `glob()` 函数实现。无匹配时保留原样（与 bash 默认行为类似）。

---

## I/O 重定向

| 语法 | 含义 | 示例 |
|------|------|------|
| `< file` | stdin 从文件读 | `grep x < test1.txt` |
| `> file` | stdout 覆盖写 | `echo hi > out.txt` |
| `>> file` | stdout 追加 | `echo more >> out.txt` |
| `2> file` | stderr 重定向到文件 | `ls /nope 2> err.txt` |
| `2>> file` | stderr 追加 | `ls /nope 2>> err.txt` |
| `2>&1` | stderr 合并到 stdout | `ls /nope > all.txt 2>&1` |

**演示示例**：

```bash
# 覆盖写
echo "a" > /tmp/t.out
cat /tmp/t.out              # a

# 追加写
echo "b" >> /tmp/t.out
cat /tmp/t.out              # a \n b

# stderr 重定向
ls /notexist 2> /tmp/t.err
cat /tmp/t.err              # ls: cannot access '/notexist': No such file or directory

# stdout 与 stderr 合并
ls /tmp /notexist > /tmp/t.all 2>&1
cat /tmp/t.all
```

**实现说明**：
- 支持 `2>file` 连写（无空格）和 `2> file` 两种形式。
- 内置命令与外部命令均支持重定向。内置命令的重定向由 `process_command` 在调用前后通过 `dup`/`dup2` 保存与恢复实现。

---

## 管道

### 基本管道

```bash
ls | grep test
cat test3.txt | grep Linux
cat test3.txt | grep Linux | grep open
ls -la | grep txt | sort
```

### 内置命令参与管道

本 Shell 支持内置命令出现在管道左侧或中间：

```bash
echo hello | grep hel       # 内置 echo 参与管道
echo built_in | cat         # 内置 echo 的输出通过管道传给 cat
echo "a\nb" | grep a
```

### 管道与重定向组合

```bash
cat test3.txt | grep Linux > result.txt
```

### 管道后台

```bash
sleep 5 | cat &
jobs                        # 整个管道作为一个作业
```

**实现说明**：多级管道创建 `N-1` 个管道并 fork `N` 个子进程，每个子进程通过 `dup2` 连接 stdin/stdout。父进程关闭所有管道 fd 后逐个 `waitpid`。管道内每个命令段独立进行分词、glob、重定向解析。

---

## 后台运行

```bash
sleep 5 &                   # 单命令后台
echo "not blocked"          # Shell 立即返回，不等待 sleep
jobs                        # [1] 运行中 sleep 5

ls | grep test &            # 管道后台
jobs                        # 管道作业
```

**实现说明**：
- 末尾 `&` 表示后台执行。
- 后台进程拥有独立进程组，不抢占终端前台。
- 完成后在下一次提示符前显示完成信息：`[作业 N 已完成, 退出码 0]`。

---

## 行编辑与 Tab 补全

| 按键 | 功能 |
|------|------|
| ↑ / ↓ | 翻阅历史（与 `history` 编号一致） |
| ← / → | 移动光标 |
| Backspace | 删除光标前字符 |
| Tab | 补全命令名/文件名/路径 |
| Ctrl+C | 清空当前行 |
| Ctrl+D | 空行时退出 Shell |
| Ctrl+Z | 停止前台作业（配合 `bg`/`fg`） |

Tab 可补全的内置命令：`cd`、`pwd`、`echo`、`type`、`exit`、`history`、`alias`、`unalias`、`export`、`unset`、`jobs`、`fg`、`bg`、`mount`、`umount`。

---

## 提示符

```
[user@host 当前目录]$       # 普通用户
[root@host 当前目录]#       # root
```

家目录路径显示为 `~` 或 `~/子路径`。

---

## 项目文件结构

```
操作系统大设计/
├── Makefile
├── README.md
├── shell.h                  # 全局宏、结构体、extern 变量
├── main.c                   # 主循环、命令调度、全局变量定义
├── parser.c / parser.h      # 分词、glob、heredoc、复合命令
├── executor.c / executor.h  # 执行、管道、进程组控制
├── builtin.c / builtin.h    # 内置命令
├── history.c / history.h    # 历史记录
├── alias.c / alias.h        # 别名
├── redirect.c / redirect.h  # 重定向（含 stderr 合并）
├── background.c / background.h  # 后台/作业控制
├── input.c / input.h        # 行编辑、Tab 补全
├── utils.c / utils.h        # 提示符
├── mount.c / mount.h        # mount/umount
└── test*.txt / testdir/     # 测试文件
```

---

## 模块职责

| 模块 | 文件 | 职责 |
|------|------|------|
| 主控制 | main.c | 初始化、主循环、命令调度、全局变量定义 |
| 解析 | parser.c | 分词、glob |
| 执行 | executor.c | fork+execvp、管道、内置命令进管道、进程组 |
| 内置 | builtin.c | 全部内置命令 |
| 历史 | history.c | 环形缓冲、持久化、`!n` 展开 |
| 别名 | alias.c | alias/unalias、递归展开 |
| 重定向 | redirect.c | `<`、`>`、`>>`、`2>`、`2>>`、`2>&1` |
| 后台 | background.c | `&`、作业表、jobs/fg/bg、SIGCHLD 处理 |
| 输入 | input.c | raw 模式、Tab、方向键 |
| 工具 | utils.c | 提示符 |
| 挂载 | mount.c | mount/umount 系统调用 |

---

## 设计约束

- 禁止 `system()`、`popen()`、`readline`、`ncurses`。
- 管道/重定向通过 `pipe()` / `dup2()` / `open()` 实现。
- `cd` 在 Shell 父进程内调用 `chdir`。
- 每个外部命令子进程建立独立进程组，前台命令通过 `tcsetpgrp` 交接终端控制权。
- Shell 主进程忽略 `SIGINT`/`SIGTSTP`/`SIGTTOU`/`SIGTTIN`，注册 `SIGCHLD` 处理器。
- 子进程恢复信号默认行为，保证子 Shell 内 `Ctrl+C` / `Ctrl+Z` 有效。
- 仅限 Linux/WSL（依赖 termios、fork、`/proc/self/exe`、`/proc/mounts`）。

---

## 快速验收脚本

在 myshell 中依次执行以下命令，可覆盖本项目的核心功能：

```bash
# 1. 基本内置命令
pwd
export DEMO=myshell
echo $DEMO

# 2. 目录切换
cd testdir
pwd
cd -

# 3. 外部命令 + glob
ls *.txt

# 4. grep 外部命令
grep Linux test3.txt
grep -i linux test3.txt
grep -n Linux test3.txt
grep -c Linux test3.txt
grep Linux test3.txt | grep open

# 5. 重定向（覆盖、追加、stderr）
echo "a" > /tmp/t.out
echo "b" >> /tmp/t.out
cat /tmp/t.out
ls /notexist 2>/tmp/t.err
cat /tmp/t.err

# 6. 管道（含内置命令参与管道）
echo hi | grep hi
echo built_in | cat

# 7. 别名
alias ll='ls -la'
type ll
ll
unalias ll

# 8. 历史
history
!!

# 9. 后台作业
sleep 2 &
jobs
# 等待 2 秒后，下一次提示符前会显示完成信息

# 10. 挂载管理
#    无参数 mount 可查看当前挂载列表（无需 root）
mount | head -5

#    真实挂载/卸载需要 root 权限，且挂载点目录必须存在。
#    课堂演示推荐用 tmpfs（无需真实设备）：
# sudo ./myshell
# mkdir -p /mnt/usb
# mount tmpfs /mnt/usb tmpfs
# mount | grep /mnt/usb
# umount /mnt/usb
#
#    若挂载真实块设备（如 U 盘），需将 tmpfs 换成实际设备路径：
# mount /dev/sdb1 /mnt/usb vfat

# 11. 命令类型识别
type export
type ls

# 12. 退出 myshell
exit

# 13. 注册为系统登录 Shell（在 bash 中执行，非 myshell 内）
# sudo make install          # 编译 + 安装到 /usr/local/bin + 注册到 /etc/shells
# grep "myshell" /etc/shells # 验证注册成功
# chsh -s /usr/local/bin/myshell  # （可选）设为默认 Shell
#
# 卸载：
# sudo make uninstall        # 删除二进制 + 从 /etc/shells 移除
```

---

## 作者

操作系统课程设计 — 陈婷 (23013149)、胥思吉 (23013152)
