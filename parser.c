/*
 * ============================================================================
 *  parser.c — 命令解析模块
 * ============================================================================
 *  操作系统课程设计作业
 *
 *  【文件定位】
 *    命令处理流水线的“分词”环节，将一行字符串拆分成 argv/argc。
 *    同时负责引号处理、变量展开、~ 路径展开、glob 通配符展开。
 *
 *  【与 Linux 内核的关系】
 *    - getenv()     : 读取环境变量，内核在 execve 时传递环境块给进程
 *    - glob()       : POSIX 库函数，底层调用内核 getdents64 + 模式匹配
 *    - globfree()   : 释放 glob 分配的内存，纯用户态
 *    本模块不涉及直接系统调用，所有展开逻辑都由 Shell 自己实现
 *
 *  【核心功能】
 *    split_cmd()       — 引号感知分词，支持单/双引号、$VAR、\转义、~展开
 *    glob_expand_argv() — 对含 * ? [ 的参数做通配符展开
 *    expand_tilde()     — 将 ~ 或 ~/path 展开为家目录
 *
 *  【引号语义规则】
 *    单引号 '' : 原样保留，不展开 $VAR
 *    双引号 "" : 展开 $VAR，保留 \n/\t 等字面序列
 *    无引号    : 展开 $VAR，\空格 转义为字面空格
 *
 *  @author  操作系统课程设计
 * ============================================================================
 */

#include "shell.h"
#include "parser.h"
#include "executor.h"
#include <glob.h>

/*
 * expand_tilde — 将 ~ 或 ~/path 展开为家目录路径
 * 功能: 实现 Shell 的 ~ 路径展开。
 * 【系统调用】getenv("HOME") → 读取环境变量，HOME 由登录时内核/pam 设置
 * 【纯用户态实现】不涉及内核系统调用，只是字符串替换
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
 * split_cmd — 引号感知的命令分词
 * 功能: 把一行命令拆分为 argv/argc，支持单双引号、变量展开、反斜杠转义、
 *       ~ 路径展开。
 * 【与内核的关系】
 *   - getenv() → 读取环境变量，内核在 execve 系统调用时将环境块传给进程
 *   其他全部是纯用户态字符串处理逻辑，Shell 自己实现
 * 【实现流程】
 *   逐字符扫描，根据引号状态决定处理方式：
 *   - 单引号内: 原样复制，不展开 $VAR
 *   - 双引号内: 展开 $VAR，反斜杠保留字面
 *   - 无引号  : 展开 $VAR，\空格 转义为字面空格
 *   每个参数完成后调用 expand_tilde 处理 ~ 前缀
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
 * 【与内核的关系】
 *   - glob()   → POSIX 库函数，底层调用内核 getdents64 遍历目录并匹配模式
 *   - globfree() → 释放 glob 内部分配的内存
 *   如果 glob 无匹配或失败，保留原字符串不展开
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


