/*
 * ============================================================================
 *  parser.c — 命令解析模块
 * ============================================================================
 */

#include "shell.h"
#include "parser.h"
#include "executor.h"
#include <glob.h>

/*
 * expand_tilde — 将 ~ 或 ~/path 展开为家目录路径
 * 功能: 实现 Shell 的 ~ 路径展开。
 * 实现: 读取 HOME 环境变量,对 "~" 和 "~/xxx" 两种形式做字符串替换。
 */
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

/*
 * trim_seg — 去除字符串首尾空白字符
 * 功能: 复合命令拆分后清理每段命令两端的空格/制表符。
 * 实现: 移动头指针并截断尾指针,返回有效内容起始地址。
 */
static char *trim_seg(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t'))
        *--end = '\0';
    return s;
}

/*
 * split_cmd — 引号感知的命令分词
 * 功能: 把一行命令拆分为 argv/argc,支持单双引号、变量展开、反斜杠转义、
 *       ~ 路径展开。
 * 实现: 逐个字符扫描,单引号内原样复制,双引号/无引号内展开 $VAR,反斜杠
 *       转义空格等,最后调用 expand_tilde。使用静态 arg_buf 存储参数。
 */
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

/*
 * glob_expand_argv — 对 argv 中含通配符的参数做 glob 展开
 * 功能: 实现 *.txt、test?.txt 等 glob 模式展开。
 * 实现: 遍历 argv,对含 * ? [ 的参数调用 POSIX glob(),将匹配结果替换原参数;
 *       无匹配或 glob 失败则保留原字符串。
 */
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

/*
 * line_has_chain — 检测行中是否含复合命令操作符
 * 功能: 判断是否需要走 chain_exec 复合命令路径。
 * 实现: 维护引号状态,仅在引号外识别 ; / && / ||。
 */
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

/*
 * line_has_heredoc — 检测行中是否含 Heredoc 语法 <<
 * 功能: 判断是否需要先收集 Heredoc 内容。
 * 实现: 维护引号状态,仅在引号外识别 "<<" 两个连续字符。
 */
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

/*
 * heredoc_process — 收集 Heredoc 输入并替换为文件重定向
 * 功能: 实现 << DELIM 多行输入,内容作为命令 stdin。
 * 实现: 解析 DELIM;创建 /tmp/myshell_hd_<pid> 临时文件;交互读取用户输入
 *       直到单独一行 DELIM;将原命令中的 "<< DELIM" 替换为 "< 临时文件"。
 */
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

/*
 * split_chain — 把复合命令拆分成多个段并记录操作符
 * 功能: 为 chain_exec 准备命令段和 ; / && / || 关系。
 * 实现: 维护引号状态,在操作符处写入 '\0',记录 ops 数组,返回段数。
 */
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

/*
 * chain_exec — 执行复合命令(; / && / ||)
 * 功能: 按语义顺序执行多个子命令,实现短路逻辑。
 * 实现: 先 split_chain 拆分;遍历各段,根据上一段退出码决定是否跳过当前段
 *       (&& 前失败则跳,|| 前成功则跳),调用 execute_line 执行每段。
 */
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
