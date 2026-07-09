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
