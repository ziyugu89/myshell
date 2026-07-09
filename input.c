/*
 * ============================================================================
 *  input.c — 输入处理模块
 * ============================================================================
 *  操作系统课程设计作业
 *
 *  功能: read_input(termios raw 模式逐字符读取)、
 *         tab_complete(Tab 补全逻辑)、refresh_line(刷新当前输入行)
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

/* ============================================================================
 *  refresh_line — 刷新当前输入行的显示
 * ============================================================================
 *  辅助函数: 方向键移动光标、历史翻阅后重绘整行
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

/* ============================================================================
 *  read_input — termios raw 模式逐字符读取 (增强版)
 * ============================================================================
 *  需求8: 上下方向键历史翻阅, 左右方向键光标移动
 *  方向键编码: 上\x1b[A 下\x1b[B 右\x1b[C 左\x1b[D
 */
int read_input(char *buf, int max_len)
{
    struct termios orig, raw;
    int len = 0, ch;
    cursor_pos = 0;

    /* 非终端输入: 直接 fgets */
    if (!isatty(STDIN_FILENO)) {
        if (fgets(buf, max_len, stdin) == NULL) return -1;
        len = (int)strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') buf[--len] = '\0';
        if (len >= max_len - 1)
            fprintf(stderr, "\nmyshell: 输入过长，已截断\n");
        return len;
    }

    if (tcgetattr(STDIN_FILENO, &orig) < 0) {
        if (fgets(buf, max_len, stdin) == NULL) return -1;
        len = (int)strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') buf[--len] = '\0';
        return len;
    }

    /* 设置 raw 模式: 关闭行缓冲和回显 */
    raw = orig;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

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
        if (ch == '\r' || ch == '\n') { /* 回车 */
            printf("\n");
            buf[len] = '\0';
            tcsetattr(STDIN_FILENO, TCSANOW, &orig);
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

/* ============================================================================
 *  is_directory — 判断路径是否为目录(Tab补全内部使用)
 */
static int is_directory(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

/* ============================================================================
 *  tab_complete — Tab 补全逻辑 (增强版: 多级路径, 多列候选)
 * ============================================================================
 *  修复:
 *    1. cursor 指针现在正确更新 (原来 (void)cursor 直接丢弃)
 *    2. 内置命令(cd/echo/type 等)加入补全候选
 *    3. 多匹配时: 先补全公共前缀, 无扩展则直接列出候选(无需双击)
 *    4. 当前目录搜索也做去重
 */
void tab_complete(const char *prefix, char *buf, int *len, int *cursor)
{
    char candidates[512][256];
    int cc = 0;
    int completing_cmd = (strchr(buf, ' ') == NULL);
    const char *search_prefix = prefix;

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
