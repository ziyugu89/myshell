/*
 * ============================================================================
 *  utils.c — 辅助函数模块
 * ============================================================================
 *  操作系统课程设计作业
 *
 *  【文件定位】
 *    工具层，提供提示符生成与打印功能，被 main.c 和 input.c 共享。
 *
 *  【与 Linux 内核的关系 — 核心系统调用详解】
 *    - getpwuid(getuid()) : 内核 sys_getuid + 读取 /etc/passwd
 *      getuid() 获取当前用户 ID，getpwuid() 查找对应用户信息(用户名、家目录)
 *    - gethostname() : 内核 sys_gethostname
 *      获取内核存储的主机名(可通过 hostname 命令修改)
 *    - getcwd() : 内核 sys_getcwd
 *      获取当前工作目录，内核从进程的 fs_struct->pwd 读取
 *    - getuid() : 内核 sys_getuid
 *      获取实际用户 ID，判断是否为 root(显示 # 还是 $)
 *
 *  @author  陈婷 23103149
 * ============================================================================
 */

#include "shell.h"
#include "utils.h"

/*
 * get_prompt_str — 生成提示符字符串
 * 功能: 构造 [user@host dir]$/# 形式的提示符，并把家目录前缀缩写成 ~。
 * 【系统调用详解】
 *   - getuid()      → 内核 sys_getuid，获取当前用户 ID
 *   - getpwuid(uid) → 内核读取 /etc/passwd，返回用户名和家目录
 *   - gethostname() → 内核 sys_gethostname，获取主机名
 *   - getcwd()      → 内核 sys_getcwd，获取当前工作目录
 *   纯用户态字符串拼接生成提示符
 */
void get_prompt_str(char *out, int max_len)
{
    char hostname[256], cwd[MAX_LINE];
    /* 【系统调用】getuid() → 内核 sys_getuid，获取当前用户 ID
     * 【系统调用】getpwuid() → 读取 /etc/passwd，获取用户名和家目录 */
    struct passwd *pw = getpwuid(getuid());

    /* 【系统调用】gethostname() → 内核 sys_gethostname，获取主机名 */
    if (gethostname(hostname, sizeof(hostname)) != 0)
        strcpy(hostname, "localhost");
    /* 【系统调用】getcwd() → 内核 sys_getcwd，获取当前目录 */
    if (getcwd(cwd, sizeof(cwd)) == NULL)
        strcpy(cwd, "?");

    /* 将家目录路径缩写为 ~ */
    if (pw && pw->pw_dir) {
        size_t dlen = strlen(pw->pw_dir);
        if (strncmp(cwd, pw->pw_dir, dlen) == 0 &&
            (cwd[dlen] == '/' || cwd[dlen] == '\0')) {
            char tmp[MAX_LINE];
            snprintf(tmp, sizeof(tmp), "~%s", cwd + dlen);
            strncpy(cwd, tmp, sizeof(cwd) - 1);
            cwd[sizeof(cwd) - 1] = '\0';
        }
    }
    /* 【系统调用】getuid() → 内核 sys_getuid，root 显示 #，普通用户显示 $ */
    const char *pc = (getuid() == 0) ? "#" : "$";
    snprintf(out, max_len, "[%s@%s %s]%s ",
             pw ? pw->pw_name : "user", hostname, cwd, pc);
}

/*
 * print_prompt — 打印提示符
 * 功能: 向终端输出当前提示符。
 * 实现: 调用 get_prompt_str 后 printf 并 fflush。
 */
void print_prompt(void)
{
    char buf[512];
    get_prompt_str(buf, sizeof(buf));
    printf("%s", buf);
    fflush(stdout);
}
