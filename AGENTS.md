# AGENTS.md

This file provides guidance to Qoder (qoder.com) when working with code in this repository.

## 构建命令

```bash
make            # 标准编译，生成 myshell（CFLAGS: -Wall -Wextra -std=c99）
make debug      # 带 -g 调试信息编译（先 clean 再 all）
make clean      # 清理所有 .o 与 myshell
./myshell       # 运行；exit 或 Ctrl+D 退出
```

源文件列表硬编码在 Makefile 的 `SRCS` 中，新增 `.c` 文件必须手动加入该变量。通用规则 `%.o: %.c shell.h` 只声明了对 `shell.h` 的依赖，但各模块实际还隐式依赖自身头文件——修改某模块 `.h` 时需 `make clean` 重编，否则可能链接到过期目标。

## 项目架构

myshell 是一个纯 C、零第三方依赖的交互式 Shell，采用**分层流水线**架构：

- **全局契约层**：`shell.h` 集中定义所有宏（`MAX_LINE`/`MAX_ARGS`/`MAX_HISTORY` 等）、结构体（`alias_entry`/`bg_job_msg`）与 `extern` 全局变量。`main.c` 是全局变量的唯一定义点（history 环形数组、alias 表、后台消息队列）。所有模块通过包含 `shell.h` 共享状态，模块间无循环依赖。

- **终端输入层**（`input.c`）：基于 termios raw 模式逐字符读取，负责行编辑、方向键历史翻阅、Tab 补全（命令名/文件名/多级路径）。这是与终端硬件特性耦合最深的模块。

- **命令处理流水线**（`main.c` 的 `process_command`）：一条用户输入按**固定顺序**经过若干变换阶段（详见下节）。该顺序是核心不变量，调整需谨慎。

- **执行层**：内置命令在 Shell 进程内直接执行（`builtin.c`）；外部命令通过 `fork`+`execvp` 执行，带进程组与终端控制权交接（`executor.c`）；多级管道在 `pipe_exec` 中独立处理，创建 N-1 个管道并 fork N 个子进程，父进程关闭所有管道 fd 后逐个 waitpid。

- **状态持久层**：历史记录环形数组持久化到 `~/.myshell_history`（启动加载、退出覆盖写）；别名表仅在内存中，进程退出即丢失。

## 命令执行流程

`process_command(line)` 严格按以下顺序处理（顺序是关键不变量）：

1. **历史展开**（`history_manage`）：处理 `history`、`!!`、`!n`、`!?kw`；普通命令在此入历史环形数组。若展开出新命令，替换 `line` 继续向下。
2. **别名展开**（`alias_handle`）：递归展开别名（上限 `MAX_ALIAS_DEPTH=10` 防死循环）。`alias`/`unalias` 命令在此消耗并返回。
3. **管道分流**：若含 `|`（引号外），整行交给 `pipe_exec`，后者自行做分段解析与重定向，**不再回到本流程**。
4. **分词**（`split_cmd`）：引号感知的分词，双引号/无引号内做 `$VAR` 展开，单引号不展开；反斜杠转义。
5. **后台检测**（`bg_run`）：剥离末尾 `&`，置 `bg` 标志。
6. **重定向解析**（`redirect_parse`）：从 argv 中抽出 `>`/`<`/`>>` 及文件名并移出 argv。
7. **派发执行**：
   - 有重定向时：先 `dup` 保存 stdin/stdout → `dup2` 替换 → 执行内置命令 → `fflush` 后 `dup2` 恢复；若非内置则恢复 fd 后交给 `execute_external`（外部命令在子进程内重新设置重定向）。
   - 无重定向时：先尝试 `builtin_cmd`，返回 0 则走 `execute_external`。

注意：内置命令的重定向由 `process_command` 在调用 `builtin_cmd` 前后通过 dup/dup2 包裹实现，`builtin_cmd` 本身对重定向无感知。

## 内置命令扩展方式

添加一个新内置命令 `foo` 需要在**三处**保持同步（这是容易遗漏的点）：

1. **`builtin.c` 的 `builtin_cmd()`**：添加 `if (strcmp(argv[0], "foo") == 0) { ...; return 1; }` 分支。返回 1 表示已处理，返回 0 则降级为外部命令。
2. **`builtin.c` 的 `builtin_names[]` 数组**：加入 `"foo"`，使 `type` 命令能正确识别其为内置命令。
3. **`input.c` 的 `tab_complete()` 内 `builtin_names[]`**：加入 `"foo"`，使 Tab 补全能补全该命令名。

> 警告：第 2、3 处是两个**独立**的静态数组，目前内容已不一致（`input.c` 版本缺少 `mount`/`umount`）。新增内置命令时务必同时更新两处。

若新命令需要在历史/别名阶段就介入（如 `history`、`alias`），则还需在 `history_manage` 或 `alias_handle` 中处理，并在 `builtin_cmd` 中保留 `return 1` 的占位分支以避免误走外部路径。

## 关键设计决策

### 进程组与终端控制
- Shell 启动即 `setpgid(0,0)` 建立独立进程组并 `tcsetpgrp` 抢占终端前台。
- 每个**外部命令子进程**都 `setpgid(pid,pid)` 放入独立进程组，前台命令通过 `tcsetpgrp` 把终端控制权交给子进程组，`waitpid` 后再收回。这是正确支持 Ctrl+C/Ctrl+Z 的前提。
- **必须忽略 `SIGTTOU` 与 `SIGTTIN`**：否则 Shell 在 `tcsetpgrp` 切换前台进程组时会被内核暂停（Stopped），表现为执行外部命令后 Shell 卡死。这是本项目踩过的关键坑。

### 信号处理
- Shell 主进程忽略 `SIGINT`/`SIGTSTP`/`SIGTTOU`/`SIGTTIN`，注册 `SIGCHLD` 处理器。
- `SIGCHLD` 处理器用 `while(waitpid(-1,NULL,WNOHANG)>0)` 循环回收（信号不排队，防遗漏），并保存/恢复 `errno`。
- **子进程必须恢复信号默认行为**（`SIG_DFL`）：否则进入 `bash`/`sh` 等子 Shell 后 Ctrl+C/Ctrl+Z 失效。`execute_external` 与 `pipe_exec` 均已处理。

### 后台进程消息延迟输出
后台进程完成信息不立即打印，而是入队 `bg_msg_queue`，在主循环每次迭代、`print_prompt` 之前由 `flush_bg_messages` 统一输出，避免打断用户当前输入。回收采用信号处理器 + `reap_children` 主动轮询的双重保障。

### 禁止 system()
全部外部命令执行走 `fork`+`execvp`，禁止 `system()`。管道与重定向通过 `pipe()`/`dup2()`/`open()` 实现。

### 引号与变量展开语义
- 单引号：原样保留，不展开 `$VAR`
- 双引号：展开 `$VAR`，保留 `\n`/`\t` 等字面序列交由 `echo` 解释
- 无引号：展开 `$VAR`，`\空格` 转义为字面空格

### 历史环形数组
`history_list[MAX_HISTORY]` 用 `count % MAX_HISTORY` 取模索引，支持 `!n` O(1) 定位；持久化时只写最近 `MAX_HISTORY` 条。

## 平台约束

- **仅限 Linux / WSL**：依赖 POSIX（termios、fork、execvp）、`/proc/self/exe`、`/proc/mounts`，无法在 Windows 原生环境编译运行。
- 编译器：gcc，标准 c99，需 POSIX 扩展（`_POSIX_C_SOURCE 200809L`，`shell.h` 顶部定义）。
- `mount.c` 额外需 `_GNU_SOURCE`（`mount`/`umount` 系统调用声明）。
- 内置 `mount`/`umount` 命令需要 root 权限（`geteuid()==0`）。
- 历史文件路径：`~/.myshell_history`，依赖 `HOME` 环境变量。
- 无第三方库依赖。
