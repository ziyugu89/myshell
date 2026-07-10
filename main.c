/*
 * ============================================================================
 *  main.c — Shell 主循环
 * ============================================================================
 */

#define _POSIX_C_SOURCE 200809L

#include <limits.h>

#include "shell.h"
#include "utils.h"
#include "input.h"
#include "parser.h"
#include "executor.h"
#include "history.h"
#include "alias.h"
#include "background.h"

char history_list[MAX_HISTORY][MAX_LINE];
int  history_count = 0;

struct alias_entry alias_table[MAX_ALIAS];
int alias_count = 0;

struct bg_job_msg bg_msg_queue[MAX_BG_MSG];
int bg_msg_count = 0;

/*
 * process_command — 命令处理流水线
 * 功能: 按固定顺序处理用户输入的一行命令: 历史展开 → 别名展开 →
 *       Heredoc 收集 → 复合命令拆分 → 单条命令执行。
 * 实现: 依次调用 history_manage、alias_handle、line_has_heredoc/
 *       heredoc_process、line_has_chain/chain_exec 或 execute_line。
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

    if (line_has_heredoc(cmd)) {
        if (heredoc_process(cmd, MAX_LINE) < 0)
            return;
    }

    if (line_has_chain(cmd))
        chain_exec(cmd);
    else
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

    if (isatty(STDIN_FILENO)) {
        setpgid(0, 0);
        tcsetpgrp(STDIN_FILENO, getpid());
    }

    char exe_path[PATH_MAX];
    ssize_t exe_len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (exe_len > 0) {
        exe_path[exe_len] = '\0';
        setenv("SHELL", exe_path, 1);
    } else {
        setenv("SHELL", "myshell", 1);
    }

    signal(SIGCHLD, sigchld_handler);
    signal(SIGINT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);

    load_history();

    printf("====================================\n");
    printf("  欢迎使用 myshell 简易命令解释器\n");
    printf("  输入 'exit' 或按 Ctrl+D 退出\n");
    printf("====================================\n");

    while (1) {
        reap_children();
        flush_bg_messages();
        print_prompt();

        int len = read_input(input_buf, MAX_LINE);
        if (len < 0) { save_history(); printf("exit\n"); break; }
        if (len == 0) continue;

        process_command(input_buf);
    }
    return 0;
}
