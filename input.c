/*
 * ============================================================================
 *  input.c — 输入处理模块
 * ============================================================================
 *  操作系统课程设计作业
 *
 *  【文件定位】
 *    终端输入层，是与终端硬件特性耦合最深的模块。
 *    负责：行编辑、方向键历史翻阅、Tab 补全、Ctrl+C/D 处理。
 *
 *  【与 Linux 内核的关系】
 *    - tcgetattr/tcsetattr : 内核 tty 子系统的 ioctl(TCGETS/TCSETS)
 *      用于切换终端的 raw/cooked 模式，这是 Shell 实现行编辑的基础
 *    - ICANON 标志: 内核行缓冲模式标志，关闭后内核不再等回车才返回输入
 *    - ECHO 标志  : 内核自动回显标志，关闭后 Shell 自己控制屏幕输出
 *    - isatty()   : 内核 ioctl(TCGETS) 判断文件描述符是否连接终端设备
 *    - opendir/readdir/closedir : 目录遍历，底层调用内核 getdents64 系统调用
 *    - stat()     : 内核 sys_newstat，判断文件是否为目录
 *    - getenv("PATH") : 读取环境变量，PATH 决定命令搜索路径
 *
 *  【核心功能】
 *    read_input()  — 切换终端 raw 模式，逐字符读取，支持方向键/退格/Tab/Ctrl
 *    tab_complete() — 补全命令名(内置+PATH+当前目录)或文件名/多级路径
 *    refresh_line() — ANSI 转义序列重绘输入行
 *
 *  @author  胥思吉 23013152
 * ============================================================================
 */

#include "shell.h"
#include "input.h"
#include "utils.h"
#include "history.h"

/* Tab 按压计数器: 第1次补全, 第2次列出候选 */
static int tab_pressed = 0;

/* 光标位置变量: 用于 read_input 中方向键编辑 */
static int cursor_pos = 0;

/*
 * refresh_line — 重绘当前输入行
 * 功能: 方向键移动或历史翻阅后保持屏幕显示与光标位置正确。
 * 实现: \r 回行首 → 打印提示符+缓冲区 → \033[K 清行尾 → \033[nD 左移光标
 * 【与内核的关系】纯用户态 ANSI 转义序列输出，由终端驱动解释执行
 */
static void refresh_line(const char *prompt_str, const char *buf, int len, int cpos)
{
    printf("\r%s%s", prompt_str, buf);
    printf("\033[K");
    int back = len - cpos;
    if (back > 0)
        printf("\033[%dD", back);
    fflush(stdout);
}

/*
 * read_input — termios raw 模式逐字符读取用户输入
 * 功能: 实现交互式行编辑(方向键历史、光标移动、退格、Tab、Ctrl+C/D)。
 *
 * 【与 Linux 内核的关系】
 *   - isatty(STDIN_FILENO): 内核 ioctl(TCGETS)，检查 stdin 是否连接终端设备
 *   - tcgetattr(): 内核 ioctl(TCGETS)，获取当前终端属性(struct termios)
 *   - tcsetattr(): 内核 ioctl(TCSETS)，设置终端属性
 *     关闭 ICANON: 内核不再做行缓冲，每个字符立即返回给进程
 *     关闭 ECHO  : 内核不再自动回显输入字符，由 Shell 自己控制显示
 *     VMIN=1/VTIME=0: 内核 read 至少等 1 个字符才返回(阻塞式)
 *   - getchar(): 底层通过内核 read(0, buf, 1) 读取一个字符
 *
 * 【实现流程】
 *   1. 检测是否终端，非终端回退 fgets
 *   2. 保存原始终端属性，设置 raw 模式
 *   3. 循环 getchar 读字符，根据键值处理：
 *      - 回车: 结束输入，恢复终端属性
 *      - Ctrl+C: 清空缓冲区，返回空行
 *      - Ctrl+D: 空行时返回 -1(表示 EOF/退出)
 *      - 退格: memmove 删除光标前字符
 *      - ESC[A/B/C/D: 方向键(上/下翻阅历史，左/右移动光标)
 *      - Tab: 调用 tab_complete 补全
 *      - 普通字符: 在光标位置插入
 *   4. 返回前恢复终端属性
 */
int read_input(char *buf, int max_len)
{
    struct termios orig, raw;
    int len = 0, ch;
    cursor_pos = 0;

    /* 非终端输入(如管道或文件重定向): 回退到 fgets 模式
     * 【系统调用】isatty() → 内核 ioctl(TCGETS) */
    if (!isatty(STDIN_FILENO)) {
        if (fgets(buf, max_len, stdin) == NULL) return -1;
        len = (int)strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') buf[--len] = '\0';
        if (len >= max_len - 1)
            fprintf(stderr, "\nmyshell: 输入过长，已截断\n");
        return len;
    }

    /* 【系统调用】tcgetattr() → 内核 ioctl(TCGETS)
     * 获取当前终端属性，保存为 orig 以便后续恢复 */
    if (tcgetattr(STDIN_FILENO, &orig) < 0) {
        if (fgets(buf, max_len, stdin) == NULL) return -1;
        len = (int)strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') buf[--len] = '\0';
        return len;
    }

    /* 【系统调用】tcsetattr() → 内核 ioctl(TCSETS)
     * 设置 raw 模式:
     *   关闭 ICANON: 内核不做行缓冲，每字符立即返回
     *   关闭 ECHO  : 内核不自动回显，由 Shell 自己输出
     *   VMIN=1     : 内核 read 最少返回 1 个字符
     *   VTIME=0    : 无超时，无限阻塞等待输入
     */
    raw = orig;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);  /* 内核 ioctl(TCSETS) */

    buf[0] = '\0';
    char saved_line[MAX_LINE] = "";
    int saved_len = 0;
    int hist_idx = history_count;
    char prompt_buf[512];
    get_prompt_str(prompt_buf, sizeof(prompt_buf));

    while (1) {
        ch = getchar();
        if (ch == EOF || ch == 4) { /* Ctrl+D */
            if (len == 0) {
                tcsetattr(STDIN_FILENO, TCSANOW, &orig);
                printf("\n");
                return -1;
            }
            continue;
        }
        if (ch == 3) { /* Ctrl+C */
            tcsetattr(STDIN_FILENO, TCSANOW, &orig);
            printf("^C\n");
            buf[0] = '\0';
            return 0;
        }
        if (ch == '\r' || ch == '\n') { /* 回车: 结束输入 */
            printf("\n");
            buf[len] = '\0';
            tcsetattr(STDIN_FILENO, TCSANOW, &orig);  /* 恢复原始终端属性 */
            return len;
        }
        if (ch == 0x7F || ch == 0x08) { /* 退格 */
            if (cursor_pos > 0) {
                memmove(buf + cursor_pos - 1, buf + cursor_pos, len - cursor_pos + 1);
                len--;
                cursor_pos--;
                buf[len] = '\0';
                refresh_line(prompt_buf, buf, len, cursor_pos);
            }
            tab_pressed = 0;
            continue;
        }
        if (ch == 0x1b) { /* ESC 序列: 方向键 */
            int ch2 = getchar();
            if (ch2 == '[') {
                int ch3 = getchar();
                if (ch3 == 'A') { /* 上箭头: 历史上一条 */
                    int hist_start = history_start_idx();
                    if (history_count > 0 && hist_idx > hist_start) {
                        if (hist_idx == history_count) {
                            strncpy(saved_line, buf, MAX_LINE - 1);
                            saved_line[MAX_LINE - 1] = '\0';
                            saved_len = len;
                        }
                        hist_idx--;
                        const char *entry = history_get_at(hist_idx);
                        if (!entry) { putchar('\a'); fflush(stdout); tab_pressed = 0; continue; }
                        strncpy(buf, entry, max_len - 1);
                        buf[max_len - 1] = '\0';
                        len = (int)strlen(buf);
                        cursor_pos = len;
                        refresh_line(prompt_buf, buf, len, cursor_pos);
                    } else { putchar('\a'); fflush(stdout); }
                    tab_pressed = 0; continue;
                } else if (ch3 == 'B') { /* 下箭头: 历史下一条 */
                    if (hist_idx < history_count) {
                        hist_idx++;
                        if (hist_idx == history_count) {
                            strncpy(buf, saved_line, max_len - 1);
                            buf[max_len - 1] = '\0';
                            len = saved_len;
                        } else {
                            const char *entry = history_get_at(hist_idx);
                            if (!entry) { putchar('\a'); fflush(stdout); tab_pressed = 0; continue; }
                            strncpy(buf, entry, max_len - 1);
                            buf[max_len - 1] = '\0';
                            len = (int)strlen(buf);
                        }
                        cursor_pos = len;
                        refresh_line(prompt_buf, buf, len, cursor_pos);
                    } else { putchar('\a'); fflush(stdout); }
                    tab_pressed = 0; continue;
                } else if (ch3 == 'C') { /* 右箭头 */
                    if (cursor_pos < len) {
                        cursor_pos++;
                        printf("\033[1C"); fflush(stdout);
                    }
                    tab_pressed = 0; continue;
                } else if (ch3 == 'D') { /* 左箭头 */
                    if (cursor_pos > 0) {
                        cursor_pos--;
                        printf("\033[1D"); fflush(stdout);
                    }
                    tab_pressed = 0; continue;
                }
            }
            tab_pressed = 0; continue;
        }
        if (ch == '\t') { /* Tab 补全 */
            tab_pressed++;
            tab_complete(buf, buf, &len, &cursor_pos);
            fflush(stdout);
            continue;
        }
        /* 普通字符: 在光标位置插入 */
        if (len < max_len - 1) {
            if (cursor_pos < len)
                memmove(buf + cursor_pos + 1, buf + cursor_pos, len - cursor_pos);
            buf[cursor_pos] = (char)ch;
            cursor_pos++;
            len++;
            buf[len] = '\0';
            if (cursor_pos == len) {
                putchar(ch); fflush(stdout);
            } else {
                refresh_line(prompt_buf, buf, len, cursor_pos);
            }
        } else {
            fprintf(stderr, "\nmyshell: 输入过长，已截断\n");
            fflush(stderr);
        }
        tab_pressed = 0;
    }
}

/*
 * is_directory — 判断路径是否为目录
 * 功能: Tab 补全时给目录候选追加 '/'。
 * 【系统调用】stat() → 内核 sys_newstat，获取文件元信息
 *   S_ISDIR 宏检查 st_mode 中的文件类型位，由内核 VFS 层填充
 */
static int is_directory(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

/*
 * tab_complete — Tab 补全逻辑
 * 功能: 根据当前输入补全命令名、文件名或多级路径。
 * 【与内核的关系】
 *   - opendir/readdir/closedir → 内核 getdents64 系统调用，遍历目录条目
 *   - stat() → 内核 sys_newstat，判断候选是否为目录
 *   - getenv("PATH") → 读取环境变量，PATH 由内核在 execve 时传递给进程
 * 【实现逻辑】
 *   补全命令名: 内置命令(shell_builtin_names) + PATH 各目录 + 当前目录
 *   补全文件名: 解析目录部分和前缀，搜索对应目录
 *   单候选: 直接补全; 多候选: 补公共前缀或列出候选
 */
void tab_complete(const char *prefix, char *buf, int *len, int *cursor)
{
    char candidates[512][256];
    int cc = 0;
    int completing_cmd = (strchr(buf, ' ') == NULL);
    const char *search_prefix = prefix;
    /* 如果输入中没有空格 → 正在输入命令名 → 补全命令名
     * 如果输入中有空格 → 正在输入参数 → 补全文件名/路径 */

    if (completing_cmd) {
        /* ---- 补全命令名: 先加入内置命令, 再搜索 PATH 和当前目录 ---- */

        /* 将内置命令加入候选列表 (与 builtin.c 共用 shell_builtin_names) */
        for (int k = 0; shell_builtin_names[k] && cc < 500; k++) {
            if (strncmp(shell_builtin_names[k], prefix, strlen(prefix)) == 0) {
                int dup = 0;
                for (int i = 0; i < cc; i++)
                    if (strcmp(candidates[i], shell_builtin_names[k]) == 0)
                    { dup = 1; break; }
                if (!dup) {
                    strncpy(candidates[cc], shell_builtin_names[k], 255);
                    candidates[cc][255] = '\0'; cc++;
                }
            }
        }

        /* 搜索 PATH 目录 */
        const char *path_env = getenv("PATH");
        if (path_env) {
            char pc[MAX_LINE * 4];
            strncpy(pc, path_env, sizeof(pc) - 1);
            pc[sizeof(pc) - 1] = '\0';
            char *sv, *dir = strtok_r(pc, ":", &sv);
            while (dir && cc < 500) {
                DIR *dp = opendir(dir);
                if (dp) {
                    struct dirent *ent;
                    while ((ent = readdir(dp)) && cc < 500) {
                        if (ent->d_name[0] == '.') continue;
                        if (strncmp(ent->d_name, prefix, strlen(prefix)) == 0) {
                            int dup = 0;
                            for (int i = 0; i < cc; i++)
                                if (strcmp(candidates[i], ent->d_name) == 0)
                                { dup = 1; break; }
                            if (!dup) {
                                strncpy(candidates[cc], ent->d_name, 255);
                                candidates[cc][255] = '\0'; cc++;
                            }
                        }
                    }
                    closedir(dp);
                }
                dir = strtok_r(NULL, ":", &sv);
            }
        }
        /* 同时搜索当前目录 (修复4: 加入去重) */
        DIR *dp = opendir(".");
        if (dp) {
            struct dirent *ent;
            while ((ent = readdir(dp)) && cc < 500) {
                if (ent->d_name[0] == '.') continue;
                if (strncmp(ent->d_name, prefix, strlen(prefix)) == 0) {
                    int dup = 0;
                    for (int i = 0; i < cc; i++)
                        if (strcmp(candidates[i], ent->d_name) == 0)
                        { dup = 1; break; }
                    if (!dup) {
                        strncpy(candidates[cc], ent->d_name, 255);
                        candidates[cc][255] = '\0'; cc++;
                    }
                }
            }
            closedir(dp);
        }
    } else {
        /* 补全文件名/路径: 解析目录部分和前缀 */
        const char *sp = strrchr(buf, ' ');
        search_prefix = sp ? sp + 1 : buf;
        char dir_part[MAX_LINE] = ".";
        char file_prefix[256] = "";
        const char *last_slash = strrchr(search_prefix, '/');
        if (last_slash) {
            size_t dlen = (size_t)(last_slash - search_prefix + 1);
            if (dlen >= sizeof(dir_part)) dlen = sizeof(dir_part) - 1;
            strncpy(dir_part, search_prefix, dlen);
            dir_part[dlen] = '\0';
            if (dir_part[0] == '\0') strcpy(dir_part, "/");
            strncpy(file_prefix, last_slash + 1, sizeof(file_prefix) - 1);
            file_prefix[sizeof(file_prefix) - 1] = '\0';
        } else {
            strcpy(dir_part, ".");
            strncpy(file_prefix, search_prefix, sizeof(file_prefix) - 1);
            file_prefix[sizeof(file_prefix) - 1] = '\0';
        }
        DIR *dp = opendir(dir_part);
        if (dp) {
            struct dirent *ent;
            while ((ent = readdir(dp)) && cc < 500) {
                if (ent->d_name[0] == '.' && file_prefix[0] != '.') continue;
                if (strncmp(ent->d_name, file_prefix, strlen(file_prefix)) == 0) {
                    if (last_slash) {
                        char dt[MAX_LINE];
                        size_t d2 = (size_t)(last_slash - search_prefix + 1);
                        if (d2 >= sizeof(dt)) d2 = sizeof(dt) - 1;
                        strncpy(dt, search_prefix, d2);
                        dt[d2] = '\0';
                        char full_path[MAX_LINE];
                        snprintf(full_path, sizeof(full_path), "%s%s", dt, ent->d_name);
                        strncpy(candidates[cc], full_path, 255);
                        candidates[cc][255] = '\0';
                    } else {
                        strncpy(candidates[cc], ent->d_name, 255);
                        candidates[cc][255] = '\0';
                    }
                    /* 检查是否为目录, 是则追加 '/' */
                    char ck[MAX_LINE];
                    if (last_slash) strncpy(ck, candidates[cc], MAX_LINE - 1);
                    else {
                        snprintf(ck, sizeof(ck), "%s/", dir_part);
                        size_t ck_used = strlen(ck);
                        strncat(ck, ent->d_name, sizeof(ck) - ck_used - 1);
                    }
                    ck[MAX_LINE - 1] = '\0';
                    if (is_directory(ck)) {
                        size_t cl = strlen(candidates[cc]);
                        if (cl < 254 && candidates[cc][cl - 1] != '/') {
                            candidates[cc][cl] = '/';
                            candidates[cc][cl + 1] = '\0';
                        }
                    }
                    cc++;
                }
            }
            closedir(dp);
        }
    }

    if (cc == 0) { putchar('\a'); fflush(stdout); }
    else if (cc == 1) {
        /* 唯一候选: 直接补全 */
        const char *c = candidates[0];
        size_t plen = strlen(search_prefix), clen = strlen(c);
        if (clen > plen) {
            for (size_t i = plen; i < clen && *len < MAX_LINE - 2; i++) {
                buf[*len] = c[i]; (*len)++; putchar(c[i]);
            }
            if (completing_cmd) { buf[*len]=' '; (*len)++; putchar(' '); }
            else if (c[clen-1] != '/') { buf[*len]=' '; (*len)++; putchar(' '); }
            buf[*len] = '\0';
        } else if (clen == plen && completing_cmd) {
            buf[*len]=' '; (*len)++; putchar(' '); buf[*len] = '\0';
        }
        /* 修复1: 同步光标位置 */
        *cursor = *len;
        tab_pressed = 0;
    } else {
        /* 多候选: 先尝试补全公共前缀 */
        size_t plen = strlen(search_prefix);
        int common = (int)strlen(candidates[0]);
        for (int i = 1; i < cc; i++) {
            int j = (int)plen;
            while (candidates[0][j] && candidates[i][j] &&
                   candidates[0][j] == candidates[i][j]) j++;
            if (j < common) common = j;
        }
        if ((size_t)common > plen) {
            /* 有公共扩展: 补全公共前缀部分 */
            for (int i = (int)plen; i < common && *len < MAX_LINE-1; i++) {
                buf[*len] = candidates[0][i]; (*len)++;
                putchar(candidates[0][i]);
            }
            buf[*len] = '\0';
            *cursor = *len;
        } else {
            /* 修复3: 无公共扩展时, 直接列出候选(无需双击Tab) */
            printf("\n");
            for (int i = 0; i < cc; i++) {
                printf("%-20s", candidates[i]);
                if ((i+1) % 4 == 0) printf("\n");
            }
            if (cc % 4 != 0) printf("\n");
            print_prompt(); printf("%s", buf);
            *cursor = *len;
        }
        tab_pressed = 0;
        fflush(stdout);
    }
}
