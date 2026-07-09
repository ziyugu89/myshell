/*
 * ============================================================================
 *  utils.c — 辅助函数实现
 * ============================================================================
 *  操作系统课程设计作业
 *
 *  功能: 提示符生成(print_prompt)、提示符字符串获取(get_prompt_str)
 *  @author  陈婷 23103149
 * ============================================================================
 */

#include "shell.h"
#include "utils.h"

/* ============================================================================
 *  get_prompt_str — 获取提示符字符串
 * ============================================================================
 *  将提示符格式化为字符串, 供 read_input 中刷新行时复用。
 *  需求12: root 显示 #, 普通用户显示 $
 */
void get_prompt_str(char *out, int max_len)
{
    char hostname[256], cwd[MAX_LINE];
    struct passwd *pw = getpwuid(getuid());

    if (gethostname(hostname, sizeof(hostname)) != 0)
        strcpy(hostname, "localhost");
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
    const char *pc = (getuid() == 0) ? "#" : "$";
    snprintf(out, max_len, "[%s@%s %s]%s ",
             pw ? pw->pw_name : "user", hostname, cwd, pc);
}

/* ============================================================================
 *  print_prompt — 打印提示符
 */
void print_prompt(void)
{
    char buf[512];
    get_prompt_str(buf, sizeof(buf));
    printf("%s", buf);
    fflush(stdout);
}
