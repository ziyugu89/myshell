/*
 * ============================================================================
 *  main.c — Shell 主循环与命令处理流水线
 * ============================================================================
 *  操作系统课程设计作业
 *
 *  【文件定位】
 *    Shell 的入口与主循环，也是全局变量的唯一定义点。
 *    负责：初始化进程组/信号/环境/历史 → 主循环读取命令 → 派发执行。
 *    process_command() 是命令处理流水线的核心，严格按固定顺序处理。
 *
 *  【与 Linux 内核的关系】
 *    - setpgid(0,0)  : 内核 sys_setpgid，将当前进程放入新进程组
 *    - tcsetpgrp()   : 内核 ioctl(TIOCSPGRP)，切换终端前台进程组
 *    - readlink()    : 内核 sys_readlink，读取 /proc/self/exe 获取可执行文件路径
 *    - signal()      : 内核 sys_signal，注册/忽略信号处理器
 *    - isatty()      : 内核 ioctl(TCGETS)，判断 fd 是否连接终端
 *
 *  【命令处理流水线顺序】(这是核心不变量，顺序是关键！)
 *    用户输入 → ① 历史展开(history_manage)
 *              → ② 别名展开(alias_handle)
 *              → ③ 管道检测(|) → 有则走 pipe_exec
 *              → ④ 分词(split_cmd)
 *              → ⑤ 后台检测(&)
 *              → ⑥ 重定向解析(redirect_parse)
 *              → ⑦ 派发执行(内置命令/外部命令)
 *
 *  @author  陈婷 23013149
 * ============================================================================
 */

#define _POSIX_C_SOURCE 200809L

#include <limits.h>     /* PATH_MAX: 路径最大长度，内核限制通常为 4096 */

#include "shell.h"
#include "utils.h"
#include "input.h"
#include "parser.h"
#include "executor.h"
#include "history.h"
#include "alias.h"
#include "background.h"

/* ======================== 全局变量定义 ========================
 * 【重要】main.c 是所有全局变量的唯一定义点，其他模块通过 shell.h 的 extern 声明访问
 * 这种设计避免了多处定义导致的链接错误(ODR 违规)
 */
char history_list[MAX_HISTORY][MAX_LINE]; /* 历史命令环形缓冲区 */
int  history_count = 0;                   /* 已记录的历史命令总数(包含已覆盖的) */

struct alias_entry alias_table[MAX_ALIAS]; /* 别名表(仅内存，退出丢失) */
int alias_count = 0;                       /* 当前别名数量 */

struct bg_job_msg bg_msg_queue[MAX_BG_MSG]; /* 后台完成消息延迟输出队列 */
int bg_msg_count = 0;                       /* 队列中待输出消息数 */

/*
 * process_command — 命令处理流水线
 * 功能: 按固定顺序处理用户输入的一行命令: 历史展开 → 别名展开 →
 *       单条命令执行。
 * 实现: 依次调用 history_manage、alias_handle、execute_line。
 *       若历史展开返回 should_exec, 用展开结果替换原 line 后继续处理。
 */
static void process_command(char *line)
{
    char expanded[MAX_LINE], hist_out[MAX_LINE];
    int should_exec = 0;

    history_manage(line, hist_out, &should_exec);
    if (should_exec) {
        strncpy(line, hist_out, MAX_LINE - 1);
        line[MAX_LINE - 1] = '\0';
    } else if (strcmp(line, "history") == 0 ||
               strncmp(line, "history ", 8) == 0) {
        return;
    } else if (line[0] == '!' && !should_exec) {
        return;
    }

    if (alias_handle(line, expanded, 0))
        return;

    char *cmd = expanded;
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    if (*cmd == '\0') return;

    execute_line(cmd);
}

/*
 * main — Shell 入口与主循环
 * 功能: 初始化进程组、信号、环境变量和历史, 进入交互循环读取并执行命令。
 * 实现: setpgid+tcsetpgrp 接管终端; 忽略 SIGINT/SIGTSTP/SIGTTOU/SIGTTIN;
 *       注册 SIGCHLD; load_history; 循环 reap_children/flush_bg_messages/
 *       print_prompt/read_input/process_command, 退出时 save_history。
 */
int main(void)
{
    char input_buf[MAX_LINE];

    /* ---------- 步骤 1: 进程组与终端控制 ----------
     * 【系统调用】setpgid(0, 0) → 内核 sys_setpgid
     *   将 Shell 自身放入独立进程组，这是正确支持 Ctrl+C/Ctrl+Z 的前提
     * 【系统调用】tcsetpgrp(STDIN_FILENO, getpid()) → 内核 ioctl(TIOCSPGRP)
     *   抢占终端前台控制权，确保 Shell 能接收键盘输入
     * 【系统调用】isatty() → 内核 ioctl(TCGETS)
     *   判断 stdin 是否连接终端(管道输入时不是终端，不需要设置进程组)
     */
    if (isatty(STDIN_FILENO)) {
        setpgid(0, 0);                              /* 内核 sys_setpgid */
        tcsetpgrp(STDIN_FILENO, getpid());           /* 内核 ioctl(TIOCSPGRP) */
    }

    /* ---------- 步骤 2: 设置 SHELL 环境变量 ----------
     * 【系统调用】readlink("/proc/self/exe") → 内核 sys_readlink
     *   /proc/self/exe 是内核 procfs 提供的符号链接，指向当前进程的可执行文件路径
     *   这是 Linux 特有的内核机制，位于内核的 proc 文件系统子系统
     * 【库函数】setenv() → 修改进程的环境变量空间，影响后续 fork 的子进程
     */
    char exe_path[PATH_MAX];
    ssize_t exe_len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);  /* 内核 sys_readlink */
    if (exe_len > 0) {
        exe_path[exe_len] = '\0';
        setenv("SHELL", exe_path, 1);
    } else {
        setenv("SHELL", "myshell", 1);
    }

    /* ---------- 步骤 3: 信号处理设置 ----------
     * 【系统调用】signal() → 内核 sys_signal/sys_rt_sigaction
     *   Shell 作为前台进程组领导者，必须忽略这些信号以避免被自己的子进程影响：
     *   - SIGCHLD: 注册处理器，子进程结束时内核发送此信号唤醒主循环回收
     *   - SIGINT : 忽略 Ctrl+C，防止 Shell 自身被中断
     *   - SIGTSTP: 忽略 Ctrl+Z，防止 Shell 被挂起
     *   - SIGTTOU: 忽略后台写终端信号，否则 tcsetpgrp 时 Shell 会被内核暂停！
     *   - SIGTTIN: 忽略后台读终端信号，同理
     *   【关键内核知识】信号由内核在特定事件发生时投递给进程，如子进程退出、
     *   用户按 Ctrl+C、进程试图读写终端等
     */
    signal(SIGCHLD, sigchld_handler);  /* 子进程结束信号：注册自定义处理器 */
    signal(SIGINT, SIG_IGN);           /* Ctrl+C: 忽略 */
    signal(SIGTSTP, SIG_IGN);          /* Ctrl+Z: 忽略 */
    signal(SIGTTOU, SIG_IGN);          /* 后台写终端: 忽略(关键！) */
    signal(SIGTTIN, SIG_IGN);          /* 后台读终端: 忽略 */

    load_history();  /* 从 ~/.myshell_history 加载上次会话的历史命令 */

    printf("====================================\n");
    printf("  欢迎使用 myshell 简易命令解释器\n");
    printf("  输入 'exit' 或按 Ctrl+D 退出\n");
    printf("====================================\n");

    /* ---------- 步骤 4: 主循环 ----------
     * 每次循环依次执行：
     *   1. reap_children()    — 非阻塞回收已结束的子进程 (waitpid WNOHANG)
     *   2. flush_bg_messages() — 输出后台进程完成消息(避免打断用户输入)
     *   3. print_prompt()     — 打印提示符 [user@host dir]$
     *   4. read_input()       — termios raw 模式读取用户输入
     *   5. process_command()  — 命令处理流水线(历史→别名→执行)
     */
    while (1) {
        reap_children();              /* 回收已结束子进程(内核 waitpid) */
        flush_bg_messages();          /* 输出后台完成消息 */
        print_prompt();               /* 打印提示符 */

        int len = read_input(input_buf, MAX_LINE);    /* termios raw 模式读取 */
        if (len < 0) { save_history(); printf("exit\n"); break; }  /* Ctrl+D: 保存历史后退出 */
        if (len == 0) continue;                                   /* 空行: 跳过 */

        process_command(input_buf);    /* 命令处理流水线 */
    }
    return 0;  /* Shell 正常退出，内核会回收进程资源 */
}
