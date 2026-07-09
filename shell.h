/*
 * ============================================================================
 *  shell.h — 简易 Linux Shell 命令解释器 全局头文件
 * ============================================================================
 *  操作系统课程设计作业
 *
 *  包含: 系统头文件、宏定义、结构体、全局变量声明(extern)
 *  @author  陈婷 23013149
 * ============================================================================
 */

#ifndef SHELL_H
#define SHELL_H

/* 启用 POSIX 扩展(如 strtok_r), 必须在系统头文件之前定义 */
#define _POSIX_C_SOURCE 200809L

/* ======================== 系统头文件 ======================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <dirent.h>
#include <termios.h>
#include <pwd.h>
#include <sys/stat.h>
#include <errno.h>
#include <signal.h>

/* ======================== 宏定义 ======================== */
#define MAX_HISTORY   1000   /* 历史命令最大条数 */
#define MAX_LINE      1024   /* 单行命令最大字符数 */
#define MAX_ARGS      64     /* 单条命令最大参数个数 */
#define MAX_ALIAS     100    /* 最大别名数量 */
#define MAX_BG_MSG    64     /* 后台进程完成消息队列最大长度 */
#define MAX_ALIAS_DEPTH 10   /* 别名递归展开最大深度 */
#define MAX_JOBS      32     /* 最大作业数 */
#define HISTORY_FILE  ".myshell_history"  /* 历史命令持久化文件名 */

/* ======================== 结构体 ======================== */

/*
 * 别名条目:
 *   name  — 别名, 如 "ll"
 *   value — 原命令, 如 "ls -l"
 */
struct alias_entry {
    char name[128];
    char value[512];
};

/*
 * 后台进程完成消息(延迟打印队列):
 *   用于需求11: 后台进程完成提示不打断用户输入
 *   pid    — 已完成的子进程PID
 *   status — 退出状态(exit code 或 signal number)
 *   type   — 0=正常退出, 1=被信号终止, 2=作业(管道)完成; pid 字段存作业号
 */
struct bg_job_msg {
    pid_t pid;
    int   status;
    int   type;
};

/*
 * 作业表条目 (jobs/fg/bg):
 *   id     — 作业编号 [%1]
 *   pgid   — 进程组 ID (管道共享同一组)
 *   pids   — 组内进程 PID 列表
 *   cmd    — 命令描述
 *   stopped— 1=已停止(TSTP), 0=运行中
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
