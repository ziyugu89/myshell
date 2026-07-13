/*
 * ============================================================================
 *  redirect.c — 重定向处理模块
 * ============================================================================
 *  操作系统课程设计作业
 *
 *  【文件定位】
 *    重定向解析与应用层，负责识别和应用 I/O 重定向。
 *    支持: < (输入) > (输出) >> (追加) 2> (错误) 2>> (错误追加) 2>&1 (合并)
 *
 *  【与 Linux 内核的关系 — 核心系统调用详解】
 *    - open()   : 内核 sys_open，打开文件获取 fd
 *      O_RDONLY: 只读打开(输入重定向)
 *      O_WRONLY|O_CREAT|O_TRUNC : 写+创建+截断(输出重定向 >)
 *      O_WRONLY|O_CREAT|O_APPEND: 写+创建+追加(输出重定向 >>)
 *    - dup2()   : 内核 sys_dup2，复制文件描述符到指定编号
 *      dup2(fd, STDIN_FILENO)  : 让 stdin 指向新文件
 *      dup2(fd, STDOUT_FILENO) : 让 stdout 指向新文件
 *      dup2(fd, STDERR_FILENO) : 让 stderr 指向新文件
 *    - dup()    : 内核 sys_dup，保存原始 fd(用于后续恢复)
 *    - close()  : 内核 sys_close，关闭不再使用的 fd
 *    - fflush() : 刷新用户态缓冲区，确保 dup2 前数据已写入内核
 *
 *  【重定向原理】
 *    Shell 通过 dup2 修改进程的 fd 表，让 stdin/stdout/stderr 指向新文件。
 *    内核的 fd 表是进程级结构，修改后该进程的所有 write/read 都对新文件生效。
 *    内置命令需要 dup 保存原 fd，执行后 dup2 恢复，否则 Shell 自身的 I/O 会被破坏。
 *
 *  @author  操作系统课程设计
 * ============================================================================
 */

#include "shell.h"
#include "redirect.h"

/*
 * redirect_init — 初始化重定向结构体
 * 功能: 将 redirect_t 所有字段清零/置 NULL。
 * 实现: 直接赋值 NULL 和 0。
 */
void redirect_init(redirect_t *r)
{
    r->in_file = NULL;
    r->out_file = NULL;
    r->err_file = NULL;
    r->append_out = 0;
    r->append_err = 0;
    r->err_to_out = 0;
}

/*
 * remove_argv_at — 从 argv 中移除连续 count 个元素
 * 功能: 解析重定向后把操作符和文件名从参数列表中删除。
 * 实现: 从 idx 开始用后序元素覆盖,更新 argc 并在末尾置 NULL。
 */
static void remove_argv_at(char **argv, int *argc, int idx, int count)
{
    for (int j = idx; j + count < *argc; j++)
        argv[j] = argv[j + count];
    *argc -= count;
    argv[*argc] = NULL;
}

/*
 * redirect_parse — 从 argv 中解析重定向操作符
 * 功能: 识别 < > >> 2> 2>> 2>&1，提取文件名并从 argv 中移除。
 * 纯用户态字符串解析，不涉及内核系统调用。
 * 支持空格/无空格两种写法: ">file" 和 "> file"
 */
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

/*
 * redirect_apply — 在当前进程应用重定向
 * 功能: 把 stdin/stdout/stderr 重新绑定到指定文件或彼此。
 * 【系统调用详解】
 *   - open() → 内核 sys_open，打开目标文件获取 fd
 *     输入重定向: O_RDONLY (只读)
 *     输出重定向 >: O_WRONLY|O_CREAT|O_TRUNC (写+创建+截断)
 *     输出重定向 >>: O_WRONLY|O_CREAT|O_APPEND (写+创建+追加)
 *   - dup2(fd, STDIN/STDOUT/STDERR_FILENO) → 内核 sys_dup2
 *     将新 fd 复制到标准 fd 编号，内核更新进程 fd 表
 *   - close(fd) → 内核 sys_close，关闭临时 fd(已通过 dup2 复制)
 *   - 2>&1: dup2(STDOUT_FILENO, STDERR_FILENO)
 *     让 stderr 指向与 stdout 相同的文件
 */
int redirect_apply(const redirect_t *r)
{
    if (r->in_file) {
        /* 【系统调用】open() → 内核 sys_open(O_RDONLY) */
        int fd = open(r->in_file, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "myshell: %s: %s\n", r->in_file, strerror(errno));
            return -1;
        }
        /* 【系统调用】dup2() → 内核 sys_dup2，让 stdin 指向文件 */
        dup2(fd, STDIN_FILENO);
        close(fd);  /* 内核 sys_close */
    }
    if (r->out_file) {
        /* 【系统调用】open() → 内核 sys_open(O_WRONLY|O_CREAT|O_TRUNC/O_APPEND) */
        int flags = O_WRONLY | O_CREAT;
        flags |= r->append_out ? O_APPEND : O_TRUNC;
        int fd = open(r->out_file, flags, 0644);
        if (fd < 0) {
            fprintf(stderr, "myshell: %s: %s\n", r->out_file, strerror(errno));
            return -1;
        }
        /* 【系统调用】dup2() → 内核 sys_dup2，让 stdout 指向文件 */
        dup2(fd, STDOUT_FILENO);
        close(fd);  /* 内核 sys_close */
    }
    if (r->err_file) {
        /* 【系统调用】open() → 内核 sys_open(O_WRONLY|O_CREAT|O_TRUNC/O_APPEND) */
        int flags = O_WRONLY | O_CREAT;
        flags |= r->append_err ? O_APPEND : O_TRUNC;
        int fd = open(r->err_file, flags, 0644);
        if (fd < 0) {
            fprintf(stderr, "myshell: %s: %s\n", r->err_file, strerror(errno));
            return -1;
        }
        /* 【系统调用】dup2() → 内核 sys_dup2，让 stderr 指向文件 */
        dup2(fd, STDERR_FILENO);
        close(fd);  /* 内核 sys_close */
    }
    /* 2>&1: 让 stderr 指向与 stdout 相同的文件
     * 【系统调用】dup2(STDOUT_FILENO, STDERR_FILENO) → 内核 sys_dup2 */
    if (r->err_to_out)
        dup2(STDOUT_FILENO, STDERR_FILENO);
    return 0;
}

/*
 * redirect_apply_with_save — 保存原 fd 后应用重定向
 * 功能: 供内置命令使用，保证重定向不影响 Shell 父进程后续 I/O。
 * 【系统调用】dup() → 内核 sys_dup，复制原 fd 到新编号(保存备份)
 *   之后调用 redirect_apply 执行实际重定向
 */
int redirect_apply_with_save(const redirect_t *r,
                             int *saved_in, int *saved_out, int *saved_err)
{
    /* 【系统调用】dup() → 内核 sys_dup，保存原始 fd 以备恢复 */
    *saved_in = *saved_out = *saved_err = -1;
    if (r->in_file)  *saved_in  = dup(STDIN_FILENO);   /* 内核 sys_dup */
    if (r->out_file || r->err_to_out) *saved_out = dup(STDOUT_FILENO);
    if (r->err_file) *saved_err = dup(STDERR_FILENO);
    return redirect_apply(r);
}

/*
 * redirect_restore — 恢复之前保存的文件描述符
 * 功能: 内置命令执行完毕后恢复原始 stdin/stdout/stderr。
 * 【系统调用】
 *   - dup2(saved_fd, 标准fd) → 内核 sys_dup2，恢复原始 fd 指向
 *   - close(saved_fd) → 内核 sys_close，释放保存用的 fd
 *   - fflush() → 刷新用户态缓冲区，确保数据在 dup2 前已写入内核
 */
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
