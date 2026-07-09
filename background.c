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

void sigchld_handler(int sig)
{
    (void)sig;
}

struct job_entry *job_find_by_id(int id)
{
    for (int i = 0; i < job_count; i++)
        if (job_table[i].id == id)
            return &job_table[i];
    return NULL;
}

struct job_entry *job_find_by_pgid(pid_t pgid)
{
    for (int i = 0; i < job_count; i++)
        if (job_table[i].pgid == pgid)
            return &job_table[i];
    return NULL;
}

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

int bg_run(char **argv, int *argc)
{
    if (*argc > 0 && strcmp(argv[*argc - 1], "&") == 0) {
        (*argc)--;
        argv[*argc] = NULL;
        return 1;
    }
    return 0;
}

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

static int parse_job_id(const char *arg, int *id)
{
    if (!arg) return -1;
    if (arg[0] == '%') arg++;
    if (*arg == '\0') return -1;
    *id = atoi(arg);
    return (*id > 0) ? 0 : -1;
}

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
