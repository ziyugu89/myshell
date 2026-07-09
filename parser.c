/*
 * ============================================================================
 *  parser.c — 命令解析模块
 * ============================================================================
 */

#include "shell.h"
#include "parser.h"
#include "executor.h"
#include <glob.h>

static void expand_tilde(char *arg)
{
    if (arg[0] != '~')
        return;
    const char *home = getenv("HOME");
    if (!home)
        return;
    char tmp[MAX_LINE];
    if (arg[1] == '\0')
        snprintf(tmp, sizeof(tmp), "%s", home);
    else if (arg[1] == '/')
        snprintf(tmp, sizeof(tmp), "%s%s", home, arg + 1);
    else
        return;
    strncpy(arg, tmp, MAX_LINE - 1);
    arg[MAX_LINE - 1] = '\0';
}

static char *trim_seg(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t'))
        *--end = '\0';
    return s;
}

int split_cmd(const char *line, char **argv, int max_args)
{
    int argc = 0;
    static char arg_buf[MAX_ARGS][MAX_LINE];

    const char *p = line;

    while (*p && argc < max_args - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

        int ai = 0;

        while (*p && *p != ' ' && *p != '\t') {
            if (*p == '\'') {
                p++;
                while (*p && *p != '\'') {
                    if (ai < MAX_LINE - 1)
                        arg_buf[argc][ai++] = *p;
                    p++;
                }
                if (*p == '\'') p++;
                else fprintf(stderr, "myshell: 语法错误: 未闭合的单引号\n");
            } else if (*p == '"') {
                p++;
                while (*p && *p != '"') {
                    if (*p == '$') {
                        p++;
                        char varname[256];
                        int vi = 0;
                        while (*p && vi < 255 &&
                               ((*p >= 'a' && *p <= 'z') ||
                                (*p >= 'A' && *p <= 'Z') ||
                                (*p >= '0' && *p <= '9') ||
                                *p == '_'))
                            varname[vi++] = *p++;
                        varname[vi] = '\0';
                        const char *val = getenv(varname);
                        if (val) {
                            while (*val && ai < MAX_LINE - 1)
                                arg_buf[argc][ai++] = *val++;
                        }
                    } else if (*p == '\\' && *(p + 1)) {
                        if (ai < MAX_LINE - 1)
                            arg_buf[argc][ai++] = *p;
                        p++;
                        if (ai < MAX_LINE - 1)
                            arg_buf[argc][ai++] = *p;
                        p++;
                    } else {
                        if (ai < MAX_LINE - 1)
                            arg_buf[argc][ai++] = *p;
                        p++;
                    }
                }
                if (*p == '"') p++;
                else fprintf(stderr, "myshell: 语法错误: 未闭合的双引号\n");
            } else if (*p == '$') {
                p++;
                char varname[256];
                int vi = 0;
                while (*p && vi < 255 &&
                       ((*p >= 'a' && *p <= 'z') ||
                        (*p >= 'A' && *p <= 'Z') ||
                        (*p >= '0' && *p <= '9') ||
                        *p == '_'))
                    varname[vi++] = *p++;
                varname[vi] = '\0';
                if (vi > 0) {
                    const char *val = getenv(varname);
                    if (val) {
                        while (*val && ai < MAX_LINE - 1)
                            arg_buf[argc][ai++] = *val++;
                    }
                } else {
                    if (ai < MAX_LINE - 1)
                        arg_buf[argc][ai++] = '$';
                }
            } else if (*p == '\\' && *(p + 1)) {
                p++;
                if (*p == ' ' || *p == '\t') {
                    if (ai < MAX_LINE - 1)
                        arg_buf[argc][ai++] = *p;
                    p++;
                } else {
                    if (ai < MAX_LINE - 1)
                        arg_buf[argc][ai++] = '\\';
                    if (ai < MAX_LINE - 1)
                        arg_buf[argc][ai++] = *p;
                    p++;
                }
            } else {
                if (ai < MAX_LINE - 1)
                    arg_buf[argc][ai++] = *p;
                p++;
            }
        }

        arg_buf[argc][ai] = '\0';
        expand_tilde(arg_buf[argc]);
        argv[argc] = arg_buf[argc];
        argc++;
    }

    argv[argc] = NULL;
    return argc;
}

int glob_expand_argv(char **argv, int *argc)
{
    static char glob_buf[MAX_ARGS][MAX_LINE];
    char *new_argv[MAX_ARGS];
    int new_argc = 0;

    for (int i = 0; i < *argc && new_argc < MAX_ARGS - 1; i++) {
        if (strpbrk(argv[i], "*?[") == NULL) {
            strncpy(glob_buf[new_argc], argv[i], MAX_LINE - 1);
            glob_buf[new_argc][MAX_LINE - 1] = '\0';
            new_argv[new_argc] = glob_buf[new_argc];
            new_argc++;
            continue;
        }
        glob_t g;
        if (glob(argv[i], 0, NULL, &g) != 0 || g.gl_pathc == 0) {
            strncpy(glob_buf[new_argc], argv[i], MAX_LINE - 1);
            glob_buf[new_argc][MAX_LINE - 1] = '\0';
            new_argv[new_argc] = glob_buf[new_argc];
            new_argc++;
            continue;
        }
        for (size_t j = 0; j < g.gl_pathc && new_argc < MAX_ARGS - 1; j++) {
            strncpy(glob_buf[new_argc], g.gl_pathv[j], MAX_LINE - 1);
            glob_buf[new_argc][MAX_LINE - 1] = '\0';
            new_argv[new_argc] = glob_buf[new_argc];
            new_argc++;
        }
        globfree(&g);
    }

    for (int i = 0; i < new_argc; i++)
        argv[i] = new_argv[i];
    argv[new_argc] = NULL;
    *argc = new_argc;
    return 0;
}

int line_has_chain(const char *line)
{
    int in_sq = 0, in_dq = 0;
    for (const char *p = line; *p; p++) {
        if (*p == '\'' && !in_dq) in_sq = !in_sq;
        else if (*p == '"' && !in_sq) in_dq = !in_dq;
        else if (!in_sq && !in_dq) {
            if (*p == ';') return 1;
            if (*p == '&' && *(p + 1) == '&') return 1;
            if (*p == '|' && *(p + 1) == '|') return 1;
        }
    }
    return 0;
}

int line_has_heredoc(const char *line)
{
    int in_sq = 0, in_dq = 0;
    for (const char *p = line; *p; p++) {
        if (*p == '\'' && !in_dq) in_sq = !in_sq;
        else if (*p == '"' && !in_sq) in_dq = !in_dq;
        else if (!in_sq && !in_dq && *p == '<' && *(p + 1) == '<')
            return 1;
    }
    return 0;
}

int heredoc_process(char *line, int max_len)
{
    char *p = line;
    int in_sq = 0, in_dq = 0;

    while (*p) {
        if (*p == '\'' && !in_dq) { in_sq = !in_sq; p++; continue; }
        if (*p == '"' && !in_sq) { in_dq = !in_dq; p++; continue; }
        if (!in_sq && !in_dq && *p == '<' && *(p + 1) == '<') {
            char *start = p;
            p += 2;
            while (*p == ' ' || *p == '\t') p++;
            char delim[128];
            int di = 0;
            while (*p && *p != ' ' && *p != '\t' && *p != '|' &&
                   *p != ';' && *p != '>' && *p != '<' && di < 127) {
                if (*p == '\'' || *p == '"') break;
                delim[di++] = *p++;
            }
            delim[di] = '\0';
            if (di == 0) {
                printf("myshell: heredoc: 缺少界定符\n");
                return -1;
            }

            char tmp_path[MAX_LINE];
            snprintf(tmp_path, sizeof(tmp_path), "/tmp/myshell_hd_%d", getpid());

            FILE *fp = fopen(tmp_path, "w");
            if (!fp) {
                perror("myshell: heredoc");
                return -1;
            }

            printf("heredoc> ");
            fflush(stdout);
            char hline[MAX_LINE];
            while (fgets(hline, sizeof(hline), stdin)) {
                size_t len = strlen(hline);
                if (len > 0 && hline[len - 1] == '\n')
                    hline[--len] = '\0';
                if (strcmp(hline, delim) == 0)
                    break;
                fprintf(fp, "%s\n", hline);
                printf("heredoc> ");
                fflush(stdout);
            }
            fclose(fp);

            char replacement[MAX_LINE];
            snprintf(replacement, sizeof(replacement), "< %s", tmp_path);

            char tail[MAX_LINE];
            strncpy(tail, p, MAX_LINE - 1);
            tail[MAX_LINE - 1] = '\0';

            *start = '\0';
            {
                char *base = line;
                while (*base == ' ' || *base == '\t') base++;
                size_t used = strlen(base);
                if (used > 0 && base != line)
                    memmove(line, base, used + 1);
            }
            strncat(line, " ", max_len - strlen(line) - 1);
            strncat(line, replacement, max_len - strlen(line) - 1);
            if (*tail) {
                strncat(line, " ", max_len - strlen(line) - 1);
                strncat(line, tail, max_len - strlen(line) - 1);
            }
            p = line + strlen(line);
            in_sq = in_dq = 0;
            continue;
        }
        p++;
    }
    return 0;
}

static int split_chain(char *line, char **segs, chain_op_t *ops, int max)
{
    int n = 0;
    int in_sq = 0, in_dq = 0;
    segs[0] = line;

    for (char *p = line; *p && n < max - 1; p++) {
        if (*p == '\'' && !in_dq) in_sq = !in_sq;
        else if (*p == '"' && !in_sq) in_dq = !in_dq;
        else if (!in_sq && !in_dq) {
            if (*p == ';') {
                *p = '\0';
                ops[n] = CHAIN_SEMI;
                n++;
                segs[n] = p + 1;
            } else if (*p == '&' && *(p + 1) == '&') {
                *p = '\0';
                *(p + 1) = '\0';
                ops[n] = CHAIN_AND;
                n++;
                segs[n] = p + 2;
                p++;
            } else if (*p == '|' && *(p + 1) == '|') {
                *p = '\0';
                *(p + 1) = '\0';
                ops[n] = CHAIN_OR;
                n++;
                segs[n] = p + 2;
                p++;
            }
        }
    }
    return n + 1;
}

int chain_exec(char *line)
{
    char local[MAX_LINE];
    strncpy(local, line, MAX_LINE - 1);
    local[MAX_LINE - 1] = '\0';

    char *segs[MAX_ARGS];
    chain_op_t ops[MAX_ARGS];
    int nsegs = split_chain(local, segs, ops, MAX_ARGS);
    int status = 0;

    for (int i = 0; i < nsegs; i++) {
        segs[i] = trim_seg(segs[i]);
        if (*segs[i] == '\0') continue;
        if (i > 0) {
            if (ops[i - 1] == CHAIN_AND && status != 0) continue;
            if (ops[i - 1] == CHAIN_OR && status == 0) continue;
        }
        status = execute_line(segs[i]);
    }
    return status;
}
