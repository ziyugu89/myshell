/*
 * ============================================================================
 *  redirect.c — 重定向处理模块
 * ============================================================================
 */

#include "shell.h"
#include "redirect.h"

void redirect_init(redirect_t *r)
{
    r->in_file = NULL;
    r->out_file = NULL;
    r->err_file = NULL;
    r->append_out = 0;
    r->append_err = 0;
    r->err_to_out = 0;
}

static void remove_argv_at(char **argv, int *argc, int idx, int count)
{
    for (int j = idx; j + count < *argc; j++)
        argv[j] = argv[j + count];
    *argc -= count;
    argv[*argc] = NULL;
}

int redirect_parse(char **argv, int *argc, redirect_t *r)
{
    redirect_init(r);
    int i = 0;
    while (i < *argc) {
        if (strcmp(argv[i], ">>") == 0) {
            if (i + 1 >= *argc) {
                printf("myshell: '>>' 后缺少文件名\n");
                return -1;
            }
            r->out_file = argv[i + 1];
            r->append_out = 1;
            remove_argv_at(argv, argc, i, 2);
            continue;
        }
        if (strcmp(argv[i], ">") == 0) {
            if (i + 1 >= *argc) {
                printf("myshell: '>' 后缺少文件名\n");
                return -1;
            }
            r->out_file = argv[i + 1];
            r->append_out = 0;
            remove_argv_at(argv, argc, i, 2);
            continue;
        }
        if (strcmp(argv[i], "2>>") == 0) {
            if (i + 1 >= *argc) {
                printf("myshell: '2>>' 后缺少文件名\n");
                return -1;
            }
            r->err_file = argv[i + 1];
            r->append_err = 1;
            remove_argv_at(argv, argc, i, 2);
            continue;
        }
        if (strcmp(argv[i], "2>") == 0) {
            if (i + 1 >= *argc) {
                printf("myshell: '2>' 后缺少文件名\n");
                return -1;
            }
            r->err_file = argv[i + 1];
            r->append_err = 0;
            remove_argv_at(argv, argc, i, 2);
            continue;
        }
        if (strncmp(argv[i], "2>>", 3) == 0 && argv[i][3] != '\0') {
            r->err_file = argv[i] + 3;
            r->append_err = 1;
            remove_argv_at(argv, argc, i, 1);
            continue;
        }
        if (strncmp(argv[i], "2>", 2) == 0 && argv[i][2] != '\0') {
            r->err_file = argv[i] + 2;
            r->append_err = 0;
            remove_argv_at(argv, argc, i, 1);
            continue;
        }
        if (strcmp(argv[i], "2>&1") == 0) {
            r->err_to_out = 1;
            remove_argv_at(argv, argc, i, 1);
            continue;
        }
        if (strcmp(argv[i], "<") == 0) {
            if (i + 1 >= *argc) {
                printf("myshell: '<' 后缺少文件名\n");
                return -1;
            }
            r->in_file = argv[i + 1];
            remove_argv_at(argv, argc, i, 2);
            continue;
        }
        i++;
    }
    return 0;
}

int redirect_apply(const redirect_t *r)
{
    if (r->in_file) {
        int fd = open(r->in_file, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "myshell: %s: %s\n", r->in_file, strerror(errno));
            return -1;
        }
        dup2(fd, STDIN_FILENO);
        close(fd);
    }
    if (r->out_file) {
        int flags = O_WRONLY | O_CREAT;
        flags |= r->append_out ? O_APPEND : O_TRUNC;
        int fd = open(r->out_file, flags, 0644);
        if (fd < 0) {
            fprintf(stderr, "myshell: %s: %s\n", r->out_file, strerror(errno));
            return -1;
        }
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }
    if (r->err_file) {
        int flags = O_WRONLY | O_CREAT;
        flags |= r->append_err ? O_APPEND : O_TRUNC;
        int fd = open(r->err_file, flags, 0644);
        if (fd < 0) {
            fprintf(stderr, "myshell: %s: %s\n", r->err_file, strerror(errno));
            return -1;
        }
        dup2(fd, STDERR_FILENO);
        close(fd);
    }
    if (r->err_to_out)
        dup2(STDOUT_FILENO, STDERR_FILENO);
    return 0;
}

int redirect_apply_with_save(const redirect_t *r,
                             int *saved_in, int *saved_out, int *saved_err)
{
    *saved_in = *saved_out = *saved_err = -1;
    if (r->in_file)  *saved_in  = dup(STDIN_FILENO);
    if (r->out_file || r->err_to_out) *saved_out = dup(STDOUT_FILENO);
    if (r->err_file) *saved_err = dup(STDERR_FILENO);
    return redirect_apply(r);
}

void redirect_restore(int saved_in, int saved_out, int saved_err)
{
    fflush(stdout);
    fflush(stderr);
    if (saved_in >= 0) {
        dup2(saved_in, STDIN_FILENO);
        close(saved_in);
    }
    if (saved_out >= 0) {
        dup2(saved_out, STDOUT_FILENO);
        close(saved_out);
    }
    if (saved_err >= 0) {
        dup2(saved_err, STDERR_FILENO);
        close(saved_err);
    }
}
