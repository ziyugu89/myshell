/*
 * ============================================================================
 *  executor.c — 命令执行模块
 * ============================================================================
 *  操作系统课程设计作业
 *
 *  【文件定位】
 *    执行层，是整个 Shell 与内核交互最密切的模块。
 *    负责：进程创建、程序加载、管道连接、进程组管理、前台/后台作业控制。
 *
 *  【与 Linux 内核的关系 — 核心系统调用详解】
 *    - fork()     : 内核 sys_fork/sys_clone，创建子进程
 *      内核复制父进程的地址空间、文件描述符表、信号处理器，返回子进程 PID
 *      子进程 pid==0，父进程 pid>0，这是 Shell 执行外部命令的基础
 *    - execvp()   : 内核 sys_execve，加载并执行新程序
 *      内核用新程序替换当前进程的代码段/数据段，从 PATH 搜索可执行文件
 *      exec 成功后进程完全变成新程序，只有 PID 不变
 *    - waitpid()  : 内核 sys_wait4，等待子进程状态变化
 *      WUNTRACED: 子进程停止时也返回(支持 Ctrl+Z)
 *      WNOHANG  : 非阻塞模式，没有已结束子进程时立即返回
 *      返回的状态字由 WIFEXITED/WEXITSTATUS/WIFSIGNALED/WTERMSIG 宏解析
 *    - pipe()     : 内核 sys_pipe，创建管道(一对 fd，读端和写端)
 *      内核在内核空间创建环形缓冲区，返回两个文件描述符
 *    - dup2()     : 内核 sys_dup2，复制文件描述符到指定编号
 *      用于重定向: dup2(fd, STDIN_FILENO) 让 stdin 指向新文件
 *    - setpgid()  : 内核 sys_setpgid，设置进程组 ID
 *      每个外部命令放入独立进程组，这是 Ctrl+C/Ctrl+Z 正确工作的前提
 *    - tcsetpgrp(): 内核 ioctl(TIOCSPGRP)，切换终端前台进程组
 *      前台命令执行前将终端控制权交给子进程组，执行后收回
 *    - kill()     : 内核 sys_kill，发送信号
 *      kill(-pgid, SIGCONT) 向整个进程组发信号，用于 fg/bg 恢复停止的作业
 *    - isatty()   : 内核 ioctl(TCGETS)，判断 fd 是否连接终端
 *    - close()    : 内核 sys_close，关闭文件描述符
 *
 *  【核心功能】
 *    execute_line() — 单条命令执行入口，调度整个执行流程
 *    run_simple()   — 执行单条命令(内置或外部)，管理进程组和终端控制
 *    pipe_exec()    — 多级管道执行，创建 N-1 个管道连接 N 个子进程
 *    line_has_pipe()— 检测行中是否有引号外的管道符 |
 *
 *  @author  操作系统课程设计
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
 * 【系统调用】signal() → 内核 sys_signal
 *   将 SIGINT/SIGTSTP/SIGTTOU/SIGTTIN 重置为 SIG_DFL(默认行为)
 *   原因: Shell 父进程忽略了这些信号，fork 会继承信号处置，
 *   如果不恢复，子 bash/sh 中 Ctrl+C/Ctrl+Z 将失效
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
 * 功能: 正常退出返回 exit code，被信号终止返回 128+signal。
 * 【与内核的关系】
 *   waitpid 返回的状态字是内核编码的复合信息：
 *   - WIFEXITED(s)  : 内核标记进程是否正常退出
 *   - WEXITSTATUS(s): 内核存储的 exit() 参数(低 8 位)
 *   - WIFSIGNALED(s): 内核标记进程是否被信号杀死
 *   - WTERMSIG(s)   : 内核存储的终止信号编号
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
 * 【与内核的系统调用交互】
 *   内置命令路径: dup 保存原 fd → redirect_apply(dup2+open) → 执行 → dup2 恢复
 *   外部命令路径:
 *     1. fork()          → 内核 sys_clone，创建子进程
 *     2. 子进程内: child_restore_signals(signal) → redirect_apply(dup2+open)
 *        → execvp()      → 内核 sys_execve，加载新程序
 *     3. 父进程: setpgid()  → 内核 sys_setpgid，设置子进程组
 *        tcsetpgrp()  → 内核 ioctl(TIOCSPGRP)，交出终端控制权
 *        waitpid()    → 内核 sys_wait4，等待子进程结束
 *        tcsetpgrp()  → 收回终端控制权
 *   后台命令: setpgid 后直接返回，不 waitpid
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

    /* 【系统调用】fork() → 内核 sys_clone
     * 创建子进程: 内核复制父进程的地址空间、fd表、信号设置
     * 子进程返回 0，父进程返回子进程 PID */
    pid_t pid = fork();
    if (pid < 0) {
        perror("myshell: fork");
        return 1;
    }

    if (pid == 0) {
        /* === 子进程执行路径 === */
        child_restore_signals();     /* 恢复默认信号处理 */
        if (redirect_apply(rd) < 0)  /* 应用重定向(open+dup2) */
            exit(1);
        /* 【系统调用】execvp() → 内核 sys_execve
         * 用新程序替换当前进程的代码段/数据段
         * 从 PATH 搜索可执行文件，加载后从 _start 开始执行
         * exec 成功后这行代码之后的永远不会执行 */
        execvp(argv[0], argv);
        fprintf(stderr, "myshell: %s: 命令未找到\n", argv[0]);
        exit(127);
    }

    if (bg) {
        /* 【系统调用】setpgid() → 内核 sys_setpgid，后台命令放入独立进程组 */
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

    /* 【系统调用】setpgid(pid, pid) → 内核 sys_setpgid
     * 将子进程放入独立进程组，这是 Ctrl+C/Ctrl+Z 正确工作的前提 */
    setpgid(pid, pid);
    /* 【系统调用】tcsetpgrp() → 内核 ioctl(TIOCSPGRP)
     * 将终端前台控制权交给子进程组，这样 Ctrl+C 会发给子进程而不是 Shell */
    if (isatty(STDIN_FILENO))
        tcsetpgrp(STDIN_FILENO, pid);

    /* 【系统调用】waitpid() → 内核 sys_wait4
     * WUNTRACED: 子进程被 Ctrl+Z 停止时也返回(而不只是退出时) */
    int wst;
    waitpid(pid, &wst, WUNTRACED);
    if (WIFSTOPPED(wst)) {
        pid_t pids[1] = { pid };
        job_add(pid, pids, 1, argv[0], 1);
        printf("\n[作业 %d 已停止]\n", job_table[job_count - 1].id);
    }

    /* 【系统调用】tcsetpgrp() → 内核 ioctl(TIOCSPGRP)
     * 子进程结束后收回终端控制权，让 Shell 重新成为前台进程组 */
    if (isatty(STDIN_FILENO))
        tcsetpgrp(STDIN_FILENO, getpgrp());

    return wait_status_to_code(wst);
}

/*
 * pipe_exec — 多级管道执行
 * 功能: 创建 N-1 个管道并 fork N 个子进程，把每个命令段的 stdin/stdout
 *       通过管道连接起来，支持内置命令参与管道和每段独立重定向。
 * 【与内核的系统调用交互】
 *   - pipe()    : 内核 sys_pipe，创建管道(内核空间的环形缓冲区，返回读/写两个fd)
 *   - fork()    : 内核 sys_clone，为每段命令创建子进程
 *   - dup2()    : 内核 sys_dup2，连接管道 fd 到 stdin/stdout
 *     dup2(pipes[i-1][0], STDIN_FILENO) : 上一段管道的读端 → 当前段的 stdin
 *     dup2(pipes[i][1], STDOUT_FILENO)   : 当前段的 stdout → 下一段管道的写端
 *   - close()   : 内核 sys_close，关闭不再使用的管道 fd
 *   - execvp()  : 内核 sys_execve，加载外部命令
 *   - setpgid() : 所有子进程放入同一进程组(第一个子进程的 PID 作为 pgid)
 *   - waitpid() : 逐个等待所有子进程结束
 *   - tcsetpgrp(): 前台管道作业时交出/收回终端控制权
 *
 * 【管道原理图】
 *   cmd1 | cmd2 | cmd3
 *   cmd1.stdout → pipe[0].write → pipe[0].read → cmd2.stdin
 *   cmd2.stdout → pipe[1].write → pipe[1].read → cmd3.stdin
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

    /* 【系统调用】pipe() → 内核 sys_pipe
     * 为每对相邻命令创建一个管道，内核分配两个 fd:
     *   pipes[i][0] = 读端，pipes[i][1] = 写端
     * 内核在内核空间创建环形缓冲区作为管道数据存储区 */
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
            /* === 管道子进程执行路径 === */
            child_restore_signals();  /* 恢复默认信号 */

            /* 【系统调用】dup2() → 内核 sys_dup2
             * 连接前一段管道的读端到当前段的 stdin */
            if (i > 0)
                dup2(pipes[i - 1][0], STDIN_FILENO);
            /* 连接当前段的 stdout 到后一段管道的写端 */
            if (i < num_cmds - 1)
                dup2(pipes[i][1], STDOUT_FILENO);

            if (redirect_apply(&rd) < 0)
                exit(1);

            /* 关闭所有管道 fd(子进程已通过 dup2 复制到了标准fd) */
            for (int j = 0; j < num_cmds - 1; j++) {
                close(pipes[j][0]);   /* 内核 sys_close */
                close(pipes[j][1]);
            }

            int bi = builtin_cmd(seg_argv, seg_argc, 0);  /* 内置命令在子进程内执行 */
            if (bi >= 0)
                exit(bi);

            /* 【系统调用】execvp() → 内核 sys_execve，加载外部命令 */
            execvp(seg_argv[0], seg_argv);
            fprintf(stderr, "myshell: %s: 命令未找到\n", seg_argv[0]);
            exit(127);
        }

        /* 【系统调用】setpgid() → 内核 sys_setpgid
         * 第一个子进程: 创建新进程组(pgid = pid[0])
         * 后续子进程: 加入同一进程组(管道所有段共享一个进程组) */
        if (i == 0) {
            setpgid(pids[0], pids[0]);
            pgid = pids[0];
        } else {
            setpgid(pids[i], pgid);
        }
    }

    /* 父进程关闭所有管道 fd(内核引用计数减少，最后一个关闭者触发 EOF) */
    for (int i = 0; i < num_cmds - 1; i++) {
        close(pipes[i][0]);    /* 内核 sys_close */
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
 * 功能: 处理一行命令的后台检测、分词、glob、重定向解析，并派发执行。
 * 【实现流程】
 *   1. bg_run_line()  检测行末尾的 & (后台标志)
 *   2. line_has_pipe() 检测是否有引号外的 | (管道)
 *      → 有则走 pipe_exec() 管道路径
 *   3. split_cmd()     分词
 *   4. glob_expand_argv() 通配符展开
 *   5. bg_run()        从 argv 中再次检测 &
 *   6. redirect_parse() 解析重定向
 *   7. run_simple()    执行单条命令
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
