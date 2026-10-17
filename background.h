/*
 * ============================================================================
 *  background.h — 后台进程与作业控制
 * ============================================================================
 */

#ifndef BACKGROUND_H
#define BACKGROUND_H

int bg_run(char **argv, int *argc);
int bg_run_line(char *line);

void reap_children(void);
void flush_bg_messages(void);
void sigchld_handler(int sig);

void job_add(pid_t pgid, const pid_t *pids, int npids, const char *cmd, int stopped);
struct job_entry *job_find_by_id(int id);
struct job_entry *job_find_by_pgid(pid_t pgid);
void job_mark_stopped(pid_t pid);
void job_remove_by_pgid(pid_t pgid);

int builtin_jobs(char **argv, int argc);
int builtin_fg(char **argv, int argc);
int builtin_bg(char **argv, int argc);
int builtin_kill(char **argv, int argc);
int builtin_wait(char **argv, int argc);

#endif /* BACKGROUND_H */
