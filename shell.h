/*
 * ============================================================================
 *  shell.h — 简易 Linux Shell 命令解释器 全局头文件
 * ============================================================================
 *  操作系统课程设计作业
 *
 *  【文件定位】
 *    整个 Shell 的"全局契约层"，集中定义所有宏、结构体与 extern 全局变量。
 *    所有模块通过 #include "shell.h" 共享状态，模块间无循环依赖。
 *    main.c 是全局变量的唯一定义点，其他模块通过 extern 声明访问。
 *
 *  【与 Linux 内核的关系 — 各系统头文件说明】
 *    <stdio.h>    — 标准 I/O 库(fprintf/fopen/fgets 等)，内核通过 VFS 提供文件读写
 *    <stdlib.h>   — 通用工具(exit/getenv/setenv/atoi 等)，exit() 触发内核进程终止
 *    <string.h>   — 字符串操作(strcmp/strncpy/strtok_r 等)，纯用户态库函数
 *    <unistd.h>   — POSIX 系统调用入口(fork/execvp/getpid/getcwd/chdir/read/write 等)
 *                   每个函数对应一个内核 syscall 号，通过 int 0x80 或 syscall 指令陷入内核
 *    <sys/types.h>— 基本数据类型(pid_t/uid_t/size_t 等)，与内核类型定义一致
 *    <sys/wait.h> — waitpid/wait 系列，内核回收子进程资源的接口，WIFEXITED 等宏解析状态
 *    <fcntl.h>    — 文件控制(open/O_RDONLY/O_WRONLY/O_CREAT/O_TRUNC/O_APPEND 等标志)
 *                   直接对应内核 sys_open 系统调用
 *    <dirent.h>   — 目录遍历(opendir/readdir/closedir)，底层调用内核 getdents64
 *    <termios.h>  — 终端属性控制(tcgetattr/tcsetattr/ICANON/ECHO 等)
 *                   对应内核 tty 子系统的 ioctl(TCGETS/TCSETS) 系统调用
 *    <pwd.h>      — 用户信息(getpwuid)，读取 /etc/passwd 解析用户目录
 *    <sys/stat.h> — 文件状态(stat/fstat)，对应内核 sys_newstat 系统调用
 *    <errno.h>    — 错误码(errno 全局变量)，内核系统调用失败时设置
 *    <signal.h>   — 信号处理(signal/sigaction/kill/SIGINT/SIGCHLD 等)
 *                   对应内核 sys_signal/sys_kill，信号由内核投递到进程
 *
 *  @author  陈婷 23013149
 * ============================================================================
 */

#ifndef SHELL_H
#define SHELL_H

/* 启用 POSIX 扩展(如 strtok_r), 必须在系统头文件之前定义 */
#define _POSIX_C_SOURCE 200809L

/* ======================== 系统头文件 ======================== */
/* 详见文件顶部注释中各头文件与 Linux 内核的对应关系说明 */
#include <stdio.h>      /* 标准 I/O: fprintf/fopen/fgets/printf */
#include <stdlib.h>     /* 通用工具: exit/getenv/setenv/atoi */
#include <string.h>     /* 字符串: strcmp/strncpy/strtok_r/memmove */
#include <unistd.h>     /* POSIX 系统调用: fork/execvp/getpid/chdir/read/write */
#include <sys/types.h>  /* 基本类型: pid_t/uid_t */
#include <sys/wait.h>   /* 进程等待: waitpid + WIFEXITED/WEXITSTATUS 宏 */
#include <fcntl.h>      /* 文件控制: open/O_RDONLY/O_WRONLY/O_CREAT */
#include <dirent.h>     /* 目录遍历: opendir/readdir (底层 getdents64) */
#include <termios.h>    /* 终端控制: tcgetattr/tcsetattr (底层 ioctl) */
#include <pwd.h>        /* 用户信息: getpwuid (读取 /etc/passwd) */
#include <sys/stat.h>   /* 文件状态: stat (内核 sys_newstat) */
#include <errno.h>      /* 错误码: errno (内核系统调用失败时设置) */
#include <signal.h>     /* 信号: signal/sigaction/kill/SIGCHLD */

/* ======================== 宏定义 ========================
 * 这些宏控制 Shell 的各项容量上限，可根据需要调整
 */
#define MAX_HISTORY   1000   /* 历史命令最大条数 */
#define MAX_LINE      1024   /* 单行命令最大字符数 */
#define MAX_ARGS      64     /* 单条命令最大参数个数 */
#define MAX_ALIAS     100    /* 最大别名数量 */
#define MAX_BG_MSG    64     /* 后台进程完成消息队列最大长度 */
#define MAX_ALIAS_DEPTH 10   /* 别名递归展开最大深度 */
#define MAX_JOBS      32     /* 最大作业数 */
#define HISTORY_FILE  ".myshell_history"  /* 历史命令持久化文件名 */

/* ======================== 结构体 ========================
 * 以下结构体定义了 Shell 运行时的核心数据结构
 */

/*
 * 别名条目 (alias):
 *   name  — 别名, 如 "ll"
 *   value — 原命令, 如 "ls -l"
 *   【纯用户态数据结构】不涉及内核，存储在进程堆内存中，退出即丢失
 */
struct alias_entry {
    char name[128];
    char value[512];
};

/*
 * 后台进程完成消息(延迟打印队列):
 *   【与内核的关系】pid 和 status 来自 waitpid() 系统调用返回的内核状态字
 *   pid    — 已完成的子进程PID (由内核分配的唯一进程标识符)
 *   status — 退出状态(exit code 或 signal number，由内核在进程退出时填充)
 *   type   — 0=正常退出(WIFEXITED), 1=被信号终止(WIFSIGNALED), 2=作业(管道)完成
 */
struct bg_job_msg {
    pid_t pid;
    int   status;
    int   type;
};

/*
 * 作业表条目 (jobs/fg/bg):
 *   【与内核的关系】pgid 对应内核进程组概念，kill(-pgid, SIG) 可向整个进程组发信号
 *   id     — 作业编号 [%1]，Shell 自管理的逻辑 ID
 *   pgid   — 进程组 ID (管道共享同一组)，由 setpgid() 系统调用设置
 *   pids   — 组内进程 PID 列表，每个 PID 由 fork() 系统调用返回
 *   cmd    — 命令描述字符串
 *   stopped— 1=已停止(SIGTSTP), 0=运行中
 */
struct job_entry {
    int    id;
    pid_t  pgid;
    pid_t  pids[MAX_ARGS];
    int    npids;
    char   cmd[MAX_LINE];
    int    stopped;
};

/* ======================== 全局变量(extern) ======================== */
extern char history_list[MAX_HISTORY][MAX_LINE];
extern int  history_count;

extern struct alias_entry alias_table[MAX_ALIAS];
extern int alias_count;

extern struct bg_job_msg bg_msg_queue[MAX_BG_MSG];
extern int bg_msg_count;

extern struct job_entry job_table[MAX_JOBS];
extern int job_count;
extern int job_next_id;

/* 内置命令名列表: type、Tab 补全共用, NULL 终止 */
extern const char *const shell_builtin_names[];

#endif /* SHELL_H */
