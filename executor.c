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

/*
 * line_has_pipe — 检测行中是否存在管道符 |
 * 功能: 判断一行命令是否需要走 pipe_exec 管道执行路径。
 * 实现: 遍历字符串,维护单双引号状态,仅在引号外识别单独的 '|'。
 */
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

/*
 * child_restore_signals — 子进程恢复默认信号行为
 * 功能: 让外部命令子进程对 Ctrl+C/Ctrl+Z 等信号恢复默认响应。
 * 实现: 将 SIGINT/SIGTSTP/SIGTTOU/SIGTTIN 重置为 SIG_DFL。
 */
static void child_restore_signals(void)
{
    signal(SIGINT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
}

/*
 * wait_status_to_code — 将 waitpid 状态转换为退出码
 * 功能: 正常退出返回 exit code,被信号终止返回 128+signal。
 * 实现: 使用 WIFEXITED/WEXITSTATUS/WIFSIGNALED/WTERMSIG 宏判断。
 */
static int wait_status_to_code(int status)
{
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

/*
 * run_simple — 执行单条命令(内置或外部)
 * 功能: 完成重定向应用、内置命令执行、外部命令 fork/execvp、
 *       前台/后台进程组管理。
 * 实现: 有重定向时先 dup 保存原 fd,执行内置命令后恢复;外部命令 fork 后
 *       子进程 child_restore_signals+redirect_apply+execvp;父进程通过
 *       setpgid+tcsetpgrp+waitpid 管理前台作业,后台则加入作业表立即返回。
 */
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

/*
 * pipe_exec — 多级管道执行
 * 功能: 创建 N-1 个管道并 fork N 个子进程,把每个命令段的 stdin/stdout
 *       通过管道连接起来,支持内置命令参与管道和每段独立重定向。
 * 实现: 按引号外的 '|' 拆分命令段;pipe 创建管道;逐段 fork,子进程 dup2
 *       连接前后管道、应用重定向,然后 builtin_cmd/execvp;父进程关闭所有
 *       管道 fd 后逐个 waitpid,支持前台 tcsetpgrp 切换和后台作业加入。
 */
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

/*
 * execute_line — 单条命令执行入口
 * 功能: 处理一行命令的后台检测、分词、glob、重定向解析,并派发执行。
 * 实现: 调用 bg_run_line 识别 &,含管道则走 pipe_exec,否则 split_cmd →
 *       glob_expand_argv → bg_run → redirect_parse → run_simple。
 */
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
