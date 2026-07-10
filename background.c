/*
 * ============================================================================
 *  background.c — 后台进程与作业控制
 * ============================================================================
 */

#include "shell.h"
#include "background.h"

struct job_entry job_table[MAX_JOBS];
int job_count = 0;
int job_next_id = 1;

/*
 * sigchld_handler — SIGCHLD 信号处理器
 * 功能: 子进程结束时唤醒主循环进行回收。
 * 实现: 空函数体,仅用于中断可能阻塞的 waitpid 轮询;实际回收由 reap_children 完成。
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
 * 功能: 回收后台/停止后终止的子进程,生成完成消息。
 * 实现: 循环 waitpid(-1, &status, WNOHANG);若属于作业且全部结束则入队作业完成
 *       消息,否则入队普通后台进程消息;信号+轮询双重保障防漏收。
 */
void reap_children(void)
{
    int status;
    pid_t pid;
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
 * 实现: 解析作业号找到 job;若已停止则 SIGCONT;tcsetpgrp 交出终端;
 *       waitpid 等待每个子进程;结束后收回终端并移除作业。
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

    if (job->stopped) {
        kill(-job->pgid, SIGCONT);
        job->stopped = 0;
    }

    if (isatty(STDIN_FILENO))
        tcsetpgrp(STDIN_FILENO, job->pgid);

    int status = 0;
    for (int k = 0; k < job->npids; k++) {
        if (job->pids[k] > 0)
            waitpid(job->pids[k], &status, 0);
    }

    if (isatty(STDIN_FILENO))
        tcsetpgrp(STDIN_FILENO, getpgrp());

    job_remove_by_pgid(job->pgid);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

/*
 * builtin_bg — 实现 bg 内置命令
 * 功能: 让已停止的作业在后台继续运行。
 * 实现: 解析作业号找到已停止 job,向其进程组发送 SIGCONT,标记为运行中。
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
