/*
 * ============================================================================
 *  background.c — 后台进程与作业控制模块
 * ============================================================================
 *  操作系统课程设计作业
 *
 *  【文件定位】
 *    后台作业管理层，实现 jobs/fg/bg 命令和子进程回收机制。
 *    管理作业表(job_table)和后台完成消息队列(bg_msg_queue)。
 *
 *  【与 Linux 内核的关系 — 核心系统调用详解】
 *    - waitpid(-1, &status, WNOHANG) : 内核 sys_wait4
 *      非阻塞回收所有已结束的子进程，WNOHANG 避免阻塞
 *      信号不排队，用 while 循环防漏收
 *    - kill(-pgid, SIGCONT) : 内核 sys_kill
 *      负 PID 表示向整个进程组发信号，用于恢复停止的作业
 *    - tcsetpgrp() : 内核 ioctl(TIOCSPGRP)
 *      fg 命令时将终端控制权交给作业进程组
 *    - waitpid(pid, &status, 0) : 内核 sys_wait4
 *      fg 命令阻塞等待作业所有进程结束
 *    - WIFEXITED/WEXITSTATUS/WIFSIGNALED/WTERMSIG : 内核状态字解析宏
 *
 *  【核心功能】
 *    sigchld_handler()  — SIGCHLD 信号处理器(空函数体，仅用于唤醒)
 *    reap_children()    — 非阻塞回收所有已结束子进程
 *    flush_bg_messages()— 延迟输出后台完成消息
 *    bg_run/bg_run_line — 检测后台标志 &
 *    builtin_jobs/fg/bg/kill/wait — 作业控制内置命令实现
 *
 *  @author  操作系统课程设计
 * ============================================================================
 */

#include "shell.h"
#include "background.h"

struct job_entry job_table[MAX_JOBS];
int job_count = 0;
int job_next_id = 1;

/*
 * sigchld_handler — SIGCHLD 信号处理器
 * 功能: 子进程结束时内核发送 SIGCHLD 信号唤醒主循环。
 * 【与内核的关系】
 *   内核在子进程退出时向其父进程发送 SIGCHLD 信号
 *   这里只是空函数体，实际回收由 reap_children 完成
 *   信号的作用是中断可能阻塞的 getchar/waitpid
 */
void sigchld_handler(int sig)
{
    (void)sig;
}

/*
 * job_find_by_id — 按作业编号查找作业
 * 功能: fg/bg 命令通过 %N 定位作业。
 * 实现: 线性遍历 job_table。
 */
struct job_entry *job_find_by_id(int id)
{
    for (int i = 0; i < job_count; i++)
        if (job_table[i].id == id)
            return &job_table[i];
    return NULL;
}

/*
 * job_find_by_pgid — 按进程组 ID 查找作业
 * 功能: 通过进程组定位整个管道作业。
 * 实现: 线性遍历 job_table 比较 pgid。
 */
struct job_entry *job_find_by_pgid(pid_t pgid)
{
    for (int i = 0; i < job_count; i++)
        if (job_table[i].pgid == pgid)
            return &job_table[i];
    return NULL;
}

/*
 * job_add — 向作业表添加新作业
 * 功能: 后台进程或停止的前台作业加入管理。
 * 实现: 在 job_table 末尾填充 id/pgid/pids/cmd/stopped,并递增 job_next_id。
 */
void job_add(pid_t pgid, const pid_t *pids, int npids, const char *cmd, int stopped)
{
    if (job_count >= MAX_JOBS) return;
    struct job_entry *j = &job_table[job_count++];
    j->id = job_next_id++;
    j->pgid = pgid;
    j->npids = npids;
    for (int i = 0; i < npids; i++)
        j->pids[i] = pids[i];
    strncpy(j->cmd, cmd, MAX_LINE - 1);
    j->cmd[MAX_LINE - 1] = '\0';
    j->stopped = stopped;
}

/*
 * job_mark_stopped — 将包含指定 PID 的作业标记为已停止
 * 功能: 前台作业被 Ctrl+Z 停止时更新作业状态。
 * 实现: 遍历作业表找到 pid 所属作业,设置 stopped=1。
 */
void job_mark_stopped(pid_t pid)
{
    for (int i = 0; i < job_count; i++) {
        for (int k = 0; k < job_table[i].npids; k++) {
            if (job_table[i].pids[k] == pid) {
                job_table[i].stopped = 1;
                return;
            }
        }
    }
}

/*
 * job_remove_by_pgid — 按进程组删除已完成作业
 * 功能: 作业所有进程结束后清理作业表。
 * 实现: 找到匹配 pgid 的作业,用末尾作业覆盖并减少 job_count。
 */
void job_remove_by_pgid(pid_t pgid)
{
    for (int i = 0; i < job_count; i++) {
        if (job_table[i].pgid == pgid) {
            job_table[i] = job_table[job_count - 1];
            job_count--;
            return;
        }
    }
}

/*
 * job_find_by_pid — 按进程 PID 查找所属作业
 * 功能: reap_children 回收子进程时判断其是否属于某个作业。
 * 实现: 遍历每个作业的 pids 数组。
 */
static struct job_entry *job_find_by_pid(pid_t pid)
{
    for (int i = 0; i < job_count; i++) {
        for (int k = 0; k < job_table[i].npids; k++) {
            if (job_table[i].pids[k] == pid)
                return &job_table[i];
        }
    }
    return NULL;
}

/*
 * reap_children — 非阻塞回收所有已结束子进程
 * 功能: 回收后台/停止后终止的子进程，生成完成消息。
 * 【系统调用】waitpid(-1, &status, WNOHANG) → 内核 sys_wait4
 *   -1 表示等待任意子进程
 *   WNOHANG 表示非阻塞，没有已结束子进程时立即返回 0
 *   while 循环: 因为信号不排队(多个子进程同时结束只发一个 SIGCHLD)，
 *   必须循环回收直到没有更多已结束子进程
 * 【状态解析】
 *   WIFEXITED(status)  : 内核标记是否正常退出
 *   WEXITSTATUS(status): 内核存储的退出码
 *   WIFSIGNALED(status): 内核标记是否被信号杀死
 *   WTERMSIG(status)   : 内核存储的信号编号
 */
void reap_children(void)
{
    int status;
    pid_t pid;
    /* 【系统调用】waitpid() → 内核 sys_wait4，非阻塞回收 */
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        struct job_entry *job = job_find_by_pid(pid);
        if (job) {
            int alive = 0;
            for (int k = 0; k < job->npids; k++) {
                if (job->pids[k] == pid)
                    job->pids[k] = 0;
                if (job->pids[k] != 0)
                    alive++;
            }
            if (alive == 0 && bg_msg_count < MAX_BG_MSG) {
                int idx = bg_msg_count++;
                bg_msg_queue[idx].pid = job->id;
                bg_msg_queue[idx].type = 2;
                if (WIFEXITED(status))
                    bg_msg_queue[idx].status = WEXITSTATUS(status);
                else if (WIFSIGNALED(status))
                    bg_msg_queue[idx].status = WTERMSIG(status);
                else
                    bg_msg_queue[idx].status = 0;
                job_remove_by_pgid(job->pgid);
            }
        } else if (bg_msg_count < MAX_BG_MSG) {
            int idx = bg_msg_count++;
            bg_msg_queue[idx].pid = pid;
            if (WIFEXITED(status)) {
                bg_msg_queue[idx].status = WEXITSTATUS(status);
                bg_msg_queue[idx].type = 0;
            } else if (WIFSIGNALED(status)) {
                bg_msg_queue[idx].status = WTERMSIG(status);
                bg_msg_queue[idx].type = 1;
            }
        }
    }
}

/*
 * flush_bg_messages — 统一输出后台完成消息
 * 功能: 避免后台进程结束提示打断用户当前输入。
 * 实现: 主循环每次打印提示符前遍历 bg_msg_queue,根据 type 输出完成/终止信息,
 *       然后清空队列。
 */
void flush_bg_messages(void)
{
    for (int i = 0; i < bg_msg_count; i++) {
        if (bg_msg_queue[i].type == 2)
            printf("\n[作业 %d 已完成, 退出码 %d]\n",
                   (int)bg_msg_queue[i].pid, bg_msg_queue[i].status);
        else if (bg_msg_queue[i].type == 0)
            printf("\n[后台进程 %d 已完成, 退出码 %d]\n",
                   (int)bg_msg_queue[i].pid, bg_msg_queue[i].status);
        else
            printf("\n[后台进程 %d 被信号 %d 终止]\n",
                   (int)bg_msg_queue[i].pid, bg_msg_queue[i].status);
    }
    bg_msg_count = 0;
}

/*
 * bg_run — 从分词后的 argv 中检测后台标志 &
 * 功能: 识别并移除 argv 末尾的 &。
 * 实现: 检查 argv[*argc-1] 是否为 "&",是则 argc-- 并返回 1。
 */
int bg_run(char **argv, int *argc)
{
    if (*argc > 0 && strcmp(argv[*argc - 1], "&") == 0) {
        (*argc)--;
        argv[*argc] = NULL;
        return 1;
    }
    return 0;
}

/*
 * bg_run_line — 从原始行末尾检测后台标志 &
 * 功能: 在分词前快速判断是否为后台命令,避免把 && 误判为 &。
 * 实现: 跳过尾部空白,检查最后一个非空字符是否为 &,且前一个字符不是 &。
 */
int bg_run_line(char *line)
{
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t'))
        line[--len] = '\0';
    if (len >= 1 && line[len - 1] == '&') {
        if (len >= 2 && line[len - 2] == '&')
            return 0;
        line[len - 1] = '\0';
        while (len > 1 && (line[len - 2] == ' ' || line[len - 2] == '\t'))
            line[--len - 1] = '\0';
        return 1;
    }
    return 0;
}

/*
 * parse_job_id — 解析 fg/bg 的作业编号参数
 * 功能: 支持 %N 或 N 形式。
 * 实现: 可选跳过前导 %,atoi 转换并校验正整数。
 */
static int parse_job_id(const char *arg, int *id)
{
    if (!arg) return -1;
    if (arg[0] == '%') arg++;
    if (*arg == '\0') return -1;
    *id = atoi(arg);
    return (*id > 0) ? 0 : -1;
}

/*
 * builtin_jobs — 实现 jobs 内置命令
 * 功能: 输出当前作业表中所有作业的状态。
 * 实现: 遍历 job_table,打印 [id] 运行中/已停止 命令 (pgid)。
 */
int builtin_jobs(char **argv, int argc)
{
    (void)argv; (void)argc;
    if (job_count == 0) {
        printf("myshell: jobs: 当前没有作业\n");
        return 0;
    }
    for (int i = 0; i < job_count; i++) {
        printf("[%d] %s %s (pgid %d)\n",
               job_table[i].id,
               job_table[i].stopped ? "已停止" : "运行中",
               job_table[i].cmd,
               (int)job_table[i].pgid);
    }
    return 0;
}

/*
 * builtin_fg — 实现 fg 内置命令
 * 功能: 将后台/停止作业调到前台运行并等待结束。
 * 【系统调用】
 *   - kill(-pgid, SIGCONT) → 内核 sys_kill，向进程组发送 SIGCONT 恢复运行
 *   - tcsetpgrp() → 内核 ioctl(TIOCSPGRP)，将终端控制权交给作业进程组
 *   - waitpid() → 内核 sys_wait4，阻塞等待作业所有进程结束
 *   - tcsetpgrp(getpgrp()) → 收回终端控制权
 */
int builtin_fg(char **argv, int argc)
{
    int id = 0;
    struct job_entry *job = NULL;

    if (argc < 2) {
        if (job_count == 0) {
            printf("myshell: fg: 当前没有作业\n");
            return 1;
        }
        job = &job_table[job_count - 1];
    } else {
        if (parse_job_id(argv[1], &id) < 0 ||
            !(job = job_find_by_id(id))) {
            printf("myshell: fg: %s: 无此作业\n", argv[1]);
            return 1;
        }
    }

    /* 【系统调用】kill(-pgid, SIGCONT) → 内核 sys_kill
     * 负 PID 表示向整个进程组发信号，恢复被 Ctrl+Z 停止的作业 */
    if (job->stopped) {
        kill(-job->pgid, SIGCONT);
        job->stopped = 0;
    }

    /* 【系统调用】tcsetpgrp() → 内核 ioctl(TIOCSPGRP)，交出终端控制权 */
    if (isatty(STDIN_FILENO))
        tcsetpgrp(STDIN_FILENO, job->pgid);

    int status = 0;
    for (int k = 0; k < job->npids; k++) {
        /* 【系统调用】waitpid() → 内核 sys_wait4，阻塞等待 */
        if (job->pids[k] > 0)
            waitpid(job->pids[k], &status, 0);
    }

    /* 【系统调用】tcsetpgrp(getpgrp()) → 内核 ioctl(TIOCSPGRP)，收回终端 */
    if (isatty(STDIN_FILENO))
        tcsetpgrp(STDIN_FILENO, getpgrp());

    job_remove_by_pgid(job->pgid);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

/*
 * builtin_bg — 实现 bg 内置命令
 * 功能: 让已停止的作业在后台继续运行。
 * 【系统调用】kill(-pgid, SIGCONT) → 内核 sys_kill
 *   向整个进程组发送 SIGCONT 信号，内核将停止状态的进程恢复运行
 *   与 fg 不同，bg 不交出终端控制权，也不等待作业结束
 */
int builtin_bg(char **argv, int argc)
{
    int id = 0;
    struct job_entry *job = NULL;

    if (argc < 2) {
        for (int i = job_count - 1; i >= 0; i--) {
            if (job_table[i].stopped) {
                job = &job_table[i];
                break;
            }
        }
        if (!job) {
            printf("myshell: bg: 当前没有已停止的作业\n");
            return 1;
        }
    } else {
        if (parse_job_id(argv[1], &id) < 0 ||
            !(job = job_find_by_id(id))) {
            printf("myshell: bg: %s: 无此作业\n", argv[1]);
            return 1;
        }
    }

    if (!job->stopped) {
        printf("myshell: bg: 作业 %d 已在运行\n", job->id);
        return 1;
    }

    kill(-job->pgid, SIGCONT);
    job->stopped = 0;
    printf("[%d] %s\n", job->id, job->cmd);
    return 0;
}

/*
 * parse_signal — 解析 kill 的信号参数
 * 支持: -9/-KILL/-SIGKILL、数字信号、常见名称 TERM/INT/STOP/CONT
 */
static int parse_signal(const char *arg, int *sig)
{
    if (!arg || arg[0] != '-' || arg[1] == '\0')
        return -1;

    const char *s = arg + 1;
    if (strcmp(s, "9") == 0 || strcmp(s, "KILL") == 0 || strcmp(s, "SIGKILL") == 0)
        *sig = SIGKILL;
    else if (strcmp(s, "15") == 0 || strcmp(s, "TERM") == 0 || strcmp(s, "SIGTERM") == 0)
        *sig = SIGTERM;
    else if (strcmp(s, "2") == 0 || strcmp(s, "INT") == 0 || strcmp(s, "SIGINT") == 0)
        *sig = SIGINT;
    else if (strcmp(s, "19") == 0 || strcmp(s, "STOP") == 0 || strcmp(s, "SIGSTOP") == 0)
        *sig = SIGSTOP;
    else if (strcmp(s, "18") == 0 || strcmp(s, "CONT") == 0 || strcmp(s, "SIGCONT") == 0)
        *sig = SIGCONT;
    else {
        char *end = NULL;
        long n = strtol(s, &end, 10);
        if (!end || *end != '\0' || n < 1 || n > 64)
            return -1;
        *sig = (int)n;
    }
    return 0;
}

/*
 * wait_one_job — 阻塞等待一个作业的全部进程结束并移出作业表
 * 【系统调用】waitpid() → 内核 sys_wait4
 * 故意 wait 时不入 bg_msg_queue（与 fg 一致，避免重复提示）
 */
static int wait_one_job(struct job_entry *job)
{
    int status = 0;
    pid_t pgid = job->pgid;
    for (int k = 0; k < job->npids; k++) {
        if (job->pids[k] <= 0)
            continue;
        int st = 0;
        pid_t w;
        do {
            w = waitpid(job->pids[k], &st, 0);
        } while (w < 0 && errno == EINTR);
        if (w > 0)
            status = st;
    }
    job_remove_by_pgid(pgid);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

/*
 * builtin_kill — 向 PID 或作业进程组发送信号
 * 用法: kill [-信号] <pid|%作业号>
 * 【系统调用】kill(pid, sig) / kill(-pgid, sig) → 内核 sys_kill
 */
int builtin_kill(char **argv, int argc)
{
    int sig = SIGTERM;
    int argi = 1;

    if (argc < 2) {
        printf("myshell: kill: 用法: kill [-信号] <pid|%%作业号>\n");
        return 1;
    }

    if (argv[1][0] == '-') {
        if (parse_signal(argv[1], &sig) < 0) {
            printf("myshell: kill: %s: 无效信号\n", argv[1]);
            return 1;
        }
        argi = 2;
        if (argi >= argc) {
            printf("myshell: kill: 用法: kill [-信号] <pid|%%作业号>\n");
            return 1;
        }
    }

    const char *target = argv[argi];
    if (target[0] == '%') {
        int id = 0;
        if (parse_job_id(target, &id) < 0) {
            printf("myshell: kill: %s: 无此作业\n", target);
            return 1;
        }
        struct job_entry *job = job_find_by_id(id);
        if (!job) {
            printf("myshell: kill: %s: 无此作业\n", target);
            return 1;
        }
        if (kill(-job->pgid, sig) < 0) {
            fprintf(stderr, "myshell: kill: %s: %s\n", target, strerror(errno));
            return 1;
        }
        return 0;
    }

    char *end = NULL;
    long pid = strtol(target, &end, 10);
    if (!end || *end != '\0' || pid <= 0) {
        printf("myshell: kill: %s: 无效的 pid 或作业号\n", target);
        return 1;
    }
    if (kill((pid_t)pid, sig) < 0) {
        fprintf(stderr, "myshell: kill: %s: %s\n", target, strerror(errno));
        return 1;
    }
    return 0;
}

/*
 * builtin_wait — 阻塞等待后台作业结束
 * 用法: wait | wait %N | wait <pid>
 * 【系统调用】waitpid() → 内核 sys_wait4
 */
int builtin_wait(char **argv, int argc)
{
    if (argc < 2) {
        int last = 0;
        while (job_count > 0)
            last = wait_one_job(&job_table[0]);
        return last;
    }

    const char *target = argv[1];
    if (target[0] == '%') {
        int id = 0;
        if (parse_job_id(target, &id) < 0) {
            printf("myshell: wait: %s: 无此作业\n", target);
            return 1;
        }
        struct job_entry *job = job_find_by_id(id);
        if (!job) {
            printf("myshell: wait: %s: 无此作业\n", target);
            return 1;
        }
        return wait_one_job(job);
    }

    char *end = NULL;
    long pid = strtol(target, &end, 10);
    if (!end || *end != '\0' || pid <= 0) {
        printf("myshell: wait: %s: 无效的 pid 或作业号\n", target);
        return 1;
    }

    struct job_entry *job = job_find_by_pid((pid_t)pid);
    if (job)
        return wait_one_job(job);

    int status = 0;
    pid_t w;
    do {
        w = waitpid((pid_t)pid, &status, 0);
    } while (w < 0 && errno == EINTR);
    if (w < 0) {
        fprintf(stderr, "myshell: wait: %s: %s\n", target, strerror(errno));
        return 1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
