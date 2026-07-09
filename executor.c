/*
 * ============================================================================
 *  executor.c — 命令执行模块
 * ============================================================================
 */

#include "shell.h"
#include "executor.h"
#include "parser.h"
#include "redirect.h"
#include "builtin.h"
#include "background.h"

static int line_has_pipe(const char *line)
{
    int in_sq = 0, in_dq = 0;
    for (const char *p = line; *p; p++) {
        if (*p == '\'' && !in_dq) in_sq = !in_sq;
        else if (*p == '"' && !in_sq) in_dq = !in_dq;
        else if (!in_sq && !in_dq && *p == '|') {
            if (*(p + 1) != '|')
                return 1;
            p++;
        }
    }
    return 0;
}

static void child_restore_signals(void)
{
    signal(SIGINT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
}

static int wait_status_to_code(int status)
{
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

static int run_simple(char **argv, int argc, int bg, redirect_t *rd)
{
    int saved_in = -1, saved_out = -1, saved_err = -1;
    int need_restore = 0;
    int status = 0;

    if (rd->in_file || rd->out_file || rd->err_file || rd->err_to_out) {
        if (redirect_apply_with_save(rd, &saved_in, &saved_out, &saved_err) < 0)
            return 1;
        need_restore = 1;
    }

    int bi = builtin_cmd(argv, argc, bg);
    if (bi >= 0) {
        status = bi;
        if (need_restore)
            redirect_restore(saved_in, saved_out, saved_err);
        return status;
    }

    if (need_restore)
        redirect_restore(saved_in, saved_out, saved_err);

    pid_t pid = fork();
    if (pid < 0) {
        perror("myshell: fork");
        return 1;
    }

    if (pid == 0) {
        child_restore_signals();
        if (redirect_apply(rd) < 0)
            exit(1);
        execvp(argv[0], argv);
        fprintf(stderr, "myshell: %s: 命令未找到\n", argv[0]);
        exit(127);
    }

    if (bg) {
        setpgid(pid, pid);
        pid_t jpids[1] = { pid };
        char desc[MAX_LINE];
        snprintf(desc, sizeof(desc), "%s", argv[0]);
        for (int k = 1; k < argc && k < 4; k++) {
            strncat(desc, " ", MAX_LINE - strlen(desc) - 1);
            strncat(desc, argv[k], MAX_LINE - strlen(desc) - 1);
        }
        job_add(pid, jpids, 1, desc, 0);
        printf("[作业 %d 已启动] %s\n", job_table[job_count - 1].id, desc);
        return 0;
    }

    setpgid(pid, pid);
    if (isatty(STDIN_FILENO))
        tcsetpgrp(STDIN_FILENO, pid);

    int wst;
    waitpid(pid, &wst, WUNTRACED);
    if (WIFSTOPPED(wst)) {
        pid_t pids[1] = { pid };
        job_add(pid, pids, 1, argv[0], 1);
        printf("\n[作业 %d 已停止]\n", job_table[job_count - 1].id);
    }

    if (isatty(STDIN_FILENO))
        tcsetpgrp(STDIN_FILENO, getpgrp());

    return wait_status_to_code(wst);
}

static int pipe_exec(char *line, int bg)
{
    char *cmd_segs[MAX_ARGS];
    int num_cmds = 0;
    char *p = line;
    cmd_segs[num_cmds++] = p;

    int in_sq = 0, in_dq = 0;
    for (char *q = p; *q; q++) {
        if (*q == '\'' && !in_dq) in_sq = !in_sq;
        else if (*q == '"' && !in_sq) in_dq = !in_dq;
        else if (*q == '|' && !in_sq && !in_dq) {
            if (*(q + 1) == '|') { q++; continue; }
            *q = '\0';
            cmd_segs[num_cmds++] = q + 1;
            if (num_cmds >= MAX_ARGS) {
                printf("myshell: 语法错误: 管道命令过多\n");
                return 1;
            }
        }
    }

    for (int i = 0; i < num_cmds; i++) {
        char *tmp[MAX_ARGS];
        if (split_cmd(cmd_segs[i], tmp, MAX_ARGS) == 0) {
            printf("myshell: 语法错误: 管道两侧必须有命令\n");
            return 1;
        }
    }

    int pipes[MAX_ARGS - 1][2];
    for (int i = 0; i < num_cmds - 1; i++) {
        if (pipe(pipes[i]) < 0) {
            perror("myshell: pipe");
            for (int j = 0; j < i; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            return 1;
        }
    }

    pid_t pids[MAX_ARGS];
    pid_t pgid = 0;
    char job_desc[MAX_LINE] = "";

    for (int i = 0; i < num_cmds; i++) {
        char *seg_argv[MAX_ARGS];
        int seg_argc = split_cmd(cmd_segs[i], seg_argv, MAX_ARGS);
        if (seg_argc == 0) continue;

        glob_expand_argv(seg_argv, &seg_argc);

        redirect_t rd;
        if (redirect_parse(seg_argv, &seg_argc, &rd) < 0) {
            for (int j = 0; j < num_cmds - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            for (int j = 0; j < i; j++)
                waitpid(pids[j], NULL, 0);
            return 1;
        }

        if (i == 0) {
            strncpy(job_desc, seg_argv[0], MAX_LINE - 1);
            job_desc[MAX_LINE - 1] = '\0';
        }

        pids[i] = fork();
        if (pids[i] < 0) {
            perror("myshell: fork");
            for (int j = 0; j < num_cmds - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            for (int j = 0; j < i; j++)
                waitpid(pids[j], NULL, 0);
            return 1;
        }

        if (pids[i] == 0) {
            child_restore_signals();

            if (i > 0)
                dup2(pipes[i - 1][0], STDIN_FILENO);
            if (i < num_cmds - 1)
                dup2(pipes[i][1], STDOUT_FILENO);

            if (redirect_apply(&rd) < 0)
                exit(1);

            for (int j = 0; j < num_cmds - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            int bi = builtin_cmd(seg_argv, seg_argc, 0);
            if (bi >= 0)
                exit(bi);

            execvp(seg_argv[0], seg_argv);
            fprintf(stderr, "myshell: %s: 命令未找到\n", seg_argv[0]);
            exit(127);
        }

        if (i == 0) {
            setpgid(pids[0], pids[0]);
            pgid = pids[0];
        } else {
            setpgid(pids[i], pgid);
        }
    }

    for (int i = 0; i < num_cmds - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    if (bg) {
        strncat(job_desc, " | ...", MAX_LINE - strlen(job_desc) - 1);
        job_add(pgid, pids, num_cmds, job_desc, 0);
        printf("[作业 %d 已启动] %s\n", job_table[job_count - 1].id, job_desc);
        return 0;
    }

    if (isatty(STDIN_FILENO))
        tcsetpgrp(STDIN_FILENO, pgid);

    int status = 0;
    for (int i = 0; i < num_cmds; i++) {
        int wst;
        waitpid(pids[i], &wst, WUNTRACED);
        if (WIFSTOPPED(wst)) {
            job_add(pgid, pids, num_cmds, job_desc, 1);
            printf("\n[作业 %d 已停止]\n", job_table[job_count - 1].id);
            if (isatty(STDIN_FILENO))
                tcsetpgrp(STDIN_FILENO, getpgrp());
            return 128 + WSTOPSIG(wst);
        }
        status = wait_status_to_code(wst);
    }

    if (isatty(STDIN_FILENO))
        tcsetpgrp(STDIN_FILENO, getpgrp());

    return status;
}

int execute_line(char *line)
{
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0') return 0;

    int bg = bg_run_line(line);
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0') return 0;

    if (line_has_pipe(line))
        return pipe_exec(line, bg);

    char *argv[MAX_ARGS];
    int argc = split_cmd(line, argv, MAX_ARGS);
    if (argc == 0) return 0;

    glob_expand_argv(argv, &argc);
    if (!bg)
        bg = bg_run(argv, &argc);
    if (argc == 0) return 0;

    redirect_t rd;
    if (redirect_parse(argv, &argc, &rd) < 0)
        return 1;
    if (argc == 0) return 0;

    return run_simple(argv, argc, bg, &rd);
}
