/*
 * ============================================================================
 *  myshell.c — 简易 Linux Shell 命令解释器 (增强版)
 * ============================================================================
 *  操作系统课程设计作业
 *
 *  编译: make all 或 gcc myshell.c -o myshell -Wall -Wextra
 *  运行: ./myshell
 *
 *  功能清单:
 *    1. 基础内置命令: cd, echo, type, exit, history, alias, unalias
 *    2. 外部命令: ls, cat, grep 等通过 fork+execvp 调用
 *    3. 历史命令: history, !!, !n, !?keyword, 上下方向键翻阅, 持久化
 *    4. 别名: alias, unalias, 递归多层展开(最多10层)
 *    5. Tab 命令/文件/多级路径补全, 双击Tab多列候选
 *    6. 后台运行: 命令末尾 & 符号, 完成提示不打断输入
 *    7. I/O 重定向: < 输入, > 输出, >> 追加输出
 *    8. 多级管道: cmd1 | cmd2 | cmd3 | ...
 *    9. 引号解析: 单引号(不展开), 双引号(展开$VAR)
 *   10. echo 支持转义字符 \n \t \\
 *   11. cd 支持 ~/xxx 家目录子路径
 *   12. 提示符区分 root(#)/普通用户($)
 *   13. 方向键编辑: 上下历史翻阅, 左右光标移动
 *   14. 中文错误提示
 *
 *  硬性约束:
 *    - 禁止 system(), popen(), readline, ncurses
 *    - 管道/重定向手动 dup2 实现
 *    - cd 在父进程 chdir 执行
 *    - 纯 C, 仅 POSIX API
 * ============================================================================
 */

#include "myshell.h"

/* ======================== 全局变量定义 ======================== */

/* 历史命令环形数组: 支持 "!n" 按编号直接定位, O(1) 访问 */
char history_list[MAX_HISTORY][MAX_LINE];
int  history_count = 0;

/* 别名结构体数组: MAX_ALIAS=100 线性查找足够快 */
struct alias_entry alias_table[MAX_ALIAS];
int alias_count = 0;

/* 后台进程完成消息队列: 延迟打印, 不打断用户输入(需求11) */
struct bg_job_msg bg_msg_queue[MAX_BG_MSG];
int bg_msg_count = 0;

/* 内置命令名列表: type 命令和 Tab 补全使用, NULL 终止 */
static const char *builtin_names[] = {
    "cd", "echo", "type", "exit", "history", "alias", "unalias",
    NULL
};

/* Tab 按压计数器: 第1次补全, 第2次列出候选 */
static int tab_pressed = 0;

/* 光标位置变量: 用于 read_input 中方向键编辑 */
static int cursor_pos = 0;

/* ============================================================================
 *  sigchld_handler — SIGCHLD 信号处理
 * ============================================================================
 *  子进程终止时内核发送 SIGCHLD, 在此用 waitpid WNOHANG 回收。
 *  while 循环: 同一信号不排队, 防止多个子进程同时终止时遗漏。
 */
static void sigchld_handler(int sig)
{
    (void)sig;
    int saved_errno = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
    errno = saved_errno;
}

/* ============================================================================
 *  reap_children — 回收僵尸进程 + 完成消息入队
 * ============================================================================
 *  信号处理的补充: 在主循环每次迭代开头主动调用, 双重保障。
 *  需求11: 完成信息不直接打印, 存入队列, 由 flush_bg_messages 统一输出。
 */
static void reap_children(void)
{
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (bg_msg_count < MAX_BG_MSG) {
            bg_msg_queue[bg_msg_count].pid = pid;
            if (WIFEXITED(status)) {
                bg_msg_queue[bg_msg_count].status = WEXITSTATUS(status);
                bg_msg_queue[bg_msg_count].type = 0;
            } else if (WIFSIGNALED(status)) {
                bg_msg_queue[bg_msg_count].status = WTERMSIG(status);
                bg_msg_queue[bg_msg_count].type = 1;
            }
            bg_msg_count++;
        }
    }
}

/* ============================================================================
 *  flush_bg_messages — 输出后台进程完成消息队列
 * ============================================================================
 *  需求11: 在 print_prompt 之前统一输出, 不打断用户输入。
 */
void flush_bg_messages(void)
{
    for (int i = 0; i < bg_msg_count; i++) {
        if (bg_msg_queue[i].type == 0)
            printf("\n[后台进程 %d 已完成, 退出码 %d]\n",
                   bg_msg_queue[i].pid, bg_msg_queue[i].status);
        else
            printf("\n[后台进程 %d 被信号 %d 终止]\n",
                   bg_msg_queue[i].pid, bg_msg_queue[i].status);
    }
    bg_msg_count = 0;
}

/* ============================================================================
 *  get_prompt_str — 获取提示符字符串
 * ============================================================================
 *  将提示符格式化为字符串, 供 read_input 中刷新行时复用。
 *  需求12: root 显示 #, 普通用户显示 $
 */
static void get_prompt_str(char *out, int max_len)
{
    char hostname[256], cwd[MAX_LINE];
    struct passwd *pw = getpwuid(getuid());

    if (gethostname(hostname, sizeof(hostname)) != 0)
        strcpy(hostname, "localhost");
    if (getcwd(cwd, sizeof(cwd)) == NULL)
        strcpy(cwd, "?");

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

/* ============================================================================
 *  load_history — 启动时从 ~/.myshell_history 加载历史命令 (需求14)
 */
void load_history(void)
{
    const char *home = getenv("HOME");
    if (!home) return;

    char path[MAX_LINE];
    snprintf(path, sizeof(path), "%s/%s", home, HISTORY_FILE);

    FILE *fp = fopen(path, "r");
    if (!fp) return;

    char line[MAX_LINE];
    while (fgets(line, sizeof(line), fp) && history_count < MAX_HISTORY) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[--len] = '\0';
        if (len > 0) {
            strncpy(history_list[history_count], line, MAX_LINE - 1);
            history_list[history_count][MAX_LINE - 1] = '\0';
            history_count++;
        }
    }
    fclose(fp);
}

/* ============================================================================
 *  save_history — 退出时覆盖写入所有历史记录 (需求14)
 */
void save_history(void)
{
    const char *home = getenv("HOME");
    if (!home) return;

    char path[MAX_LINE];
    snprintf(path, sizeof(path), "%s/%s", home, HISTORY_FILE);

    FILE *fp = fopen(path, "w");
    if (!fp) return;

    int start = (history_count > MAX_HISTORY) ? (history_count - MAX_HISTORY) : 0;
    for (int i = start; i < history_count; i++)
        fprintf(fp, "%s\n", history_list[i % MAX_HISTORY]);

    fclose(fp);
}

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
        if (ch == EOF || ch == 4) {
            if (len == 0) {
                tcsetattr(STDIN_FILENO, TCSANOW, &orig);
                printf("\n");
                return -1;
            }
            continue;
        }
        if (ch == 3) {
            tcsetattr(STDIN_FILENO, TCSANOW, &orig);
            printf("^C\n");
            buf[0] = '\0';
            return 0;
        }
        if (ch == '\r' || ch == '\n') {
            printf("\n");
            buf[len] = '\0';
            tcsetattr(STDIN_FILENO, TCSANOW, &orig);
            return len;
        }
        if (ch == 0x7F || ch == 0x08) {
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
        if (ch == 0x1b) {
            int ch2 = getchar();
            if (ch2 == '[') {
                int ch3 = getchar();
                if (ch3 == 'A') { /* 上箭头: 历史上一条 */
                    if (history_count > 0 && hist_idx > 0) {
                        if (hist_idx == history_count) {
                            strncpy(saved_line, buf, MAX_LINE - 1);
                            saved_line[MAX_LINE - 1] = '\0';
                            saved_len = len;
                        }
                        hist_idx--;
                        strncpy(buf, history_list[hist_idx % MAX_HISTORY], max_len - 1);
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
                            strncpy(buf, history_list[hist_idx % MAX_HISTORY], max_len - 1);
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
        if (ch == '\t') {
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
 *  split_cmd — 命令字符串分割为参数数组 (增强版: 支持引号)
 * ============================================================================
 *  需求3: 引号解析
 *    - 引号内的空格不作为参数分隔符
 *    - 双引号内支持 $VAR 环境变量展开, 单引号内不展开
 *    - 引号本身不作为参数内容传递
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
                        while (*p && *p != '"' && *p != ' ' && *p != '\t' && vi < 255)
                            varname[vi++] = *p++;
                        varname[vi] = '\0';
                        const char *val = getenv(varname);
                        if (val) {
                            while (*val && ai < MAX_LINE - 1)
                                arg_buf[argc][ai++] = *val++;
                        }
                    } else if (*p == '\\' && *(p + 1)) {
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
            } else {
                if (ai < MAX_LINE - 1)
                    arg_buf[argc][ai++] = *p;
                p++;
            }
        }

        arg_buf[argc][ai] = '\0';
        argv[argc] = arg_buf[argc];
        argc++;
    }

    argv[argc] = NULL;
    return argc;
}

static int is_directory(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

/* ============================================================================
 *  tab_complete — Tab 补全逻辑 (增强版: 多级路径, 多列候选)
 */
void tab_complete(const char *prefix, char *buf, int *len, int *cursor)
{
    (void)cursor;
    char candidates[512][256];
    int cc = 0;
    int completing_cmd = (strchr(buf, ' ') == NULL);
    const char *search_prefix = prefix;

    if (completing_cmd) {
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
        DIR *dp = opendir(".");
        if (dp) {
            struct dirent *ent;
            while ((ent = readdir(dp)) && cc < 500) {
                if (ent->d_name[0] == '.') continue;
                if (strncmp(ent->d_name, prefix, strlen(prefix)) == 0) {
                    strncpy(candidates[cc], ent->d_name, 255);
                    candidates[cc][255] = '\0'; cc++;
                }
            }
            closedir(dp);
        }
    } else {
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
                        snprintf(candidates[cc], 256, "%s%s", dt, ent->d_name);
                    } else {
                        strncpy(candidates[cc], ent->d_name, 255);
                        candidates[cc][255] = '\0';
                    }
                    char ck[MAX_LINE];
                    if (last_slash) strncpy(ck, candidates[cc], MAX_LINE - 1);
                    else snprintf(ck, sizeof(ck), "%s/%s", dir_part, ent->d_name);
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
        tab_pressed = 0;
    } else {
        if (tab_pressed >= 2) {
            printf("\n");
            for (int i = 0; i < cc; i++) {
                printf("%-20s", candidates[i]);
                if ((i+1) % 4 == 0) printf("\n");
            }
            if (cc % 4 != 0) printf("\n");
            print_prompt(); printf("%s", buf);
            tab_pressed = 0;
        } else {
            size_t plen = strlen(search_prefix);
            int common = (int)strlen(candidates[0]);
            for (int i = 1; i < cc; i++) {
                int j = (int)plen;
                while (candidates[0][j] && candidates[i][j] &&
                       candidates[0][j] == candidates[i][j]) j++;
                if (j < common) common = j;
            }
            if ((size_t)common > plen) {
                for (int i = (int)plen; i < common && *len < MAX_LINE-1; i++) {
                    buf[*len] = candidates[0][i]; (*len)++;
                    putchar(candidates[0][i]);
                }
                buf[*len] = '\0';
            } else { putchar('\a'); }
        }
        fflush(stdout);
    }
}

/* ============================================================================
 *  history_manage — 历史命令管理
 */
void history_manage(const char *cmd, char *out, int *should_exec)
{
    *should_exec = 0; out[0] = '\0';

    if (strcmp(cmd, "history") == 0) {
        int start = (history_count > MAX_HISTORY) ? (history_count - MAX_HISTORY) : 0;
        for (int i = start; i < history_count; i++)
            printf("  %d  %s\n", i + 1, history_list[i % MAX_HISTORY]);
        return;
    }
    if (strncmp(cmd, "history ", 8) == 0) {
        int n = atoi(cmd + 8);
        if (n <= 0) { printf("myshell: history: 无效参数\n"); return; }
        int start = (history_count > n) ? (history_count - n) : 0;
        int rs = (history_count > MAX_HISTORY) ? (history_count - MAX_HISTORY) : 0;
        if (start < rs) start = rs;
        for (int i = start; i < history_count; i++)
            printf("  %d  %s\n", i + 1, history_list[i % MAX_HISTORY]);
        return;
    }
    if (strcmp(cmd, "!!") == 0) {
        if (history_count == 0) { printf("myshell: !!: 没有历史命令\n"); return; }
        strcpy(out, history_list[(history_count - 1) % MAX_HISTORY]);
        printf("%s\n", out); *should_exec = 1; return;
    }
    if (cmd[0] == '!' && cmd[1] >= '0' && cmd[1] <= '9') {
        int n = atoi(cmd + 1);
        if (n < 1 || n > history_count) { printf("myshell: !%d: 事件未找到\n", n); return; }
        strcpy(out, history_list[(n - 1) % MAX_HISTORY]);
        printf("%s\n", out); *should_exec = 1; return;
    }
    if (cmd[0] == '!' && cmd[1] == '?') {
        const char *kw = cmd + 2;
        if (*kw == '\0') { printf("myshell: !?: 缺少搜索关键词\n"); return; }
        int start = (history_count > MAX_HISTORY) ? (history_count - MAX_HISTORY) : 0;
        for (int i = history_count - 1; i >= start; i--) {
            if (strstr(history_list[i % MAX_HISTORY], kw)) {
                strcpy(out, history_list[i % MAX_HISTORY]);
                printf("%s\n", out); *should_exec = 1; return;
            }
        }
        printf("myshell: !?%s: 未找到匹配的历史命令\n", kw); return;
    }
    if (strlen(cmd) > 0) {
        int idx = history_count % MAX_HISTORY;
        strncpy(history_list[idx], cmd, MAX_LINE - 1);
        history_list[idx][MAX_LINE - 1] = '\0';
        history_count++;
    }
}

/* ============================================================================
 *  alias_handle — 别名管理与递归展开 (需求7)
 */
int alias_handle(const char *line, char *expanded, int depth)
{
    char first[128]; int i = 0;
    while (line[i]==' '||line[i]=='\t') i++;
    int j = 0;
    while (line[i] && line[i]!=' ' && line[i]!='\t' && j<127) first[j++]=line[i++];
    first[j] = '\0';
    const char *rest = line + i;
    while (*rest==' '||*rest=='\t') rest++;

    if (strcmp(first, "alias") == 0) {
        if (*rest == '\0') {
            for (int k=0; k<alias_count; k++)
                printf("alias %s='%s'\n", alias_table[k].name, alias_table[k].value);
        } else {
            char arg[512];
            strncpy(arg, rest, sizeof(arg)-1); arg[sizeof(arg)-1]='\0';
            char *eq = strchr(arg, '=');
            if (!eq) { printf("myshell: alias: 用法: alias 名称='命令'\n"); return 1; }
            *eq = '\0'; char *name = arg; char *val = eq + 1;
            size_t vl = strlen(val);
            if (vl>=2 && val[0]=='\'' && val[vl-1]=='\'') { val[vl-1]='\0'; val++; }
            vl = strlen(val);
            if (vl>=2 && val[0]=='"' && val[vl-1]=='"') { val[vl-1]='\0'; val++; }
            for (int k=0; k<alias_count; k++)
                if (strcmp(alias_table[k].name, name)==0) {
                    strncpy(alias_table[k].value, val, 511);
                    alias_table[k].value[511]='\0'; return 1;
                }
            if (alias_count >= MAX_ALIAS) {
                printf("myshell: alias: 别名数量已达上限 (%d)\n", MAX_ALIAS); return 1;
            }
            strncpy(alias_table[alias_count].name, name, 127);
            alias_table[alias_count].name[127]='\0';
            strncpy(alias_table[alias_count].value, val, 511);
            alias_table[alias_count].value[511]='\0';
            alias_count++;
        }
        return 1;
    }

    if (strcmp(first, "unalias") == 0) {
        if (*rest=='\0') { printf("myshell: unalias: 用法: unalias 别名名称\n"); return 1; }
        char nm[128]; int k=0;
        while (rest[k] && rest[k]!=' ' && rest[k]!='\t' && k<127) { nm[k]=rest[k]; k++; }
        nm[k]='\0';
        for (k=0; k<alias_count; k++)
            if (strcmp(alias_table[k].name, nm)==0) {
                alias_table[k]=alias_table[alias_count-1]; alias_count--; return 1;
            }
        printf("myshell: unalias: %s: 未找到该别名\n", nm); return 1;
    }

    if (depth > MAX_ALIAS_DEPTH) {
        fprintf(stderr, "myshell: 别名展开嵌套过深 (超过%d层)\n", MAX_ALIAS_DEPTH);
        strncpy(expanded, line, MAX_LINE-1); expanded[MAX_LINE-1]='\0'; return 0;
    }

    for (int k=0; k<alias_count; k++)
        if (strcmp(first, alias_table[k].name)==0) {
            char temp[MAX_LINE];
            snprintf(temp, sizeof(temp), "%s%s%s",
                     alias_table[k].value, *rest?" ":"", rest);
            char temp2[MAX_LINE];
            int ret = alias_handle(temp, temp2, depth+1);
            if (ret==1) { strncpy(expanded, temp, MAX_LINE-1); expanded[MAX_LINE-1]='\0'; }
            else { strncpy(expanded, temp2, MAX_LINE-1); expanded[MAX_LINE-1]='\0'; }
            return 0;
        }

    strncpy(expanded, line, MAX_LINE-1); expanded[MAX_LINE-1]='\0';
    return 0;
}


/* ============================================================================
 *  bg_run — 后台进程检测
 */
int bg_run(char **argv, int *argc)
{
    if (*argc > 0 && strcmp(argv[*argc - 1], "&") == 0) {
        (*argc)--; argv[*argc] = NULL; return 1;
    }
    return 0;
}

/* ============================================================================
 *  redirect_parse — I/O 重定向解析 (支持 >> 追加, 需求2)
 */
int redirect_parse(char **argv, int *argc, char **in_file,
                   char **out_file, int *append_mode)
{
    *in_file = NULL; *out_file = NULL; *append_mode = 0;
    int i = 0;
    while (i < *argc) {
        if (strcmp(argv[i], ">>") == 0) {
            if (i+1 >= *argc) { printf("myshell: '>>' 后缺少文件名\n"); return -1; }
            *out_file = argv[i+1]; *append_mode = 1;
            int j; for (j=i; j+2 < *argc; j++) argv[j] = argv[j+2];
            argv[j] = NULL; *argc -= 2; continue;
        } else if (strcmp(argv[i], ">") == 0) {
            if (i+1 >= *argc) { printf("myshell: '>' 后缺少文件名\n"); return -1; }
            *out_file = argv[i+1]; *append_mode = 0;
            int j; for (j=i; j+2 < *argc; j++) argv[j] = argv[j+2];
            argv[j] = NULL; *argc -= 2; continue;
        } else if (strcmp(argv[i], "<") == 0) {
            if (i+1 >= *argc) { printf("myshell: '<' 后缺少文件名\n"); return -1; }
            *in_file = argv[i+1];
            int j; for (j=i; j+2 < *argc; j++) argv[j] = argv[j+2];
            argv[j] = NULL; *argc -= 2; continue;
        }
        i++;
    }
    return 0;
}

/* ============================================================================
 *  execute_external — fork + execvp 执行外部命令 (需求2: 支持O_APPEND)
 */
void execute_external(char **argv, int bg, char *in_file,
                      char *out_file, int append_out)
{
    pid_t pid = fork();
    if (pid < 0) { perror("myshell: fork"); return; }

    if (pid == 0) {
        if (in_file) {
            int fd = open(in_file, O_RDONLY);
            if (fd < 0) { perror("myshell: 输入重定向"); exit(1); }
            dup2(fd, STDIN_FILENO); close(fd);
        }
        if (out_file) {
            int flags = O_WRONLY | O_CREAT;
            if (append_out) flags |= O_APPEND; else flags |= O_TRUNC;
            int fd = open(out_file, flags, 0644);
            if (fd < 0) { perror("myshell: 输出重定向"); exit(1); }
            dup2(fd, STDOUT_FILENO); close(fd);
        }
        execvp(argv[0], argv);
        fprintf(stderr, "myshell: %s: 命令未找到\n", argv[0]);
        exit(127);
    }

    if (bg) {
        printf("[后台进程 %d 已启动]\n", pid);
    } else {
        int status;
        waitpid(pid, &status, 0);
    }
}

/* ============================================================================
 *  pipe_exec — 多级管道处理 (需求4: cmd1|cmd2|cmd3|...)
 * ============================================================================
 *  创建 N-1 个管道, fork N 个子进程, 父进程关闭所有管道 fd
 */
void pipe_exec(char *line)
{
    char *cmd_segs[MAX_ARGS];
    int num_cmds = 0;
    char *p = line;
    cmd_segs[num_cmds++] = p;

    /* 按 '|' 分割(跳过引号内的 '|') */
    int in_sq = 0, in_dq = 0;
    for (char *q = p; *q; q++) {
        if (*q == '\'' && !in_dq) in_sq = !in_sq;
        else if (*q == '"' && !in_sq) in_dq = !in_dq;
        else if (*q == '|' && !in_sq && !in_dq) {
            *q = '\0';
            cmd_segs[num_cmds++] = q + 1;
            if (num_cmds >= MAX_ARGS) {
                printf("myshell: 语法错误: 管道命令过多\n"); return;
            }
        }
    }

    /* 验证每段非空 */
    for (int i = 0; i < num_cmds; i++) {
        char *tmp[MAX_ARGS];
        if (split_cmd(cmd_segs[i], tmp, MAX_ARGS) == 0) {
            printf("myshell: 语法错误: 管道两侧必须有命令\n"); return;
        }
    }

    /* 创建 N-1 个管道 */
    int pipes[MAX_ARGS - 1][2];
    for (int i = 0; i < num_cmds - 1; i++) {
        if (pipe(pipes[i]) < 0) {
            perror("myshell: pipe");
            for (int j = 0; j < i; j++) { close(pipes[j][0]); close(pipes[j][1]); }
            return;
        }
    }

    /* fork N 个子进程 */
    pid_t pids[MAX_ARGS];
    for (int i = 0; i < num_cmds; i++) {
        char *seg_argv[MAX_ARGS];
        int seg_argc = split_cmd(cmd_segs[i], seg_argv, MAX_ARGS);
        if (seg_argc == 0) continue;

        char *seg_in = NULL, *seg_out = NULL;
        int seg_append = 0;
        if (redirect_parse(seg_argv, &seg_argc, &seg_in, &seg_out, &seg_append) < 0) {
            for (int j = 0; j < num_cmds - 1; j++) {
                close(pipes[j][0]); close(pipes[j][1]);
            }
            return;
        }

        pids[i] = fork();
        if (pids[i] < 0) {
            perror("myshell: fork");
            for (int j = 0; j < num_cmds - 1; j++) {
                close(pipes[j][0]); close(pipes[j][1]);
            }
            for (int j = 0; j < i; j++) waitpid(pids[j], NULL, 0);
            return;
        }

        if (pids[i] == 0) {
            if (i > 0) dup2(pipes[i-1][0], STDIN_FILENO);
            if (i < num_cmds - 1) dup2(pipes[i][1], STDOUT_FILENO);

            if (seg_in) {
                int fin = open(seg_in, O_RDONLY);
                if (fin < 0) { perror("myshell: 输入重定向"); exit(1); }
                dup2(fin, STDIN_FILENO); close(fin);
            }
            if (seg_out) {
                int fl = O_WRONLY | O_CREAT;
                fl |= seg_append ? O_APPEND : O_TRUNC;
                int fout = open(seg_out, fl, 0644);
                if (fout < 0) { perror("myshell: 输出重定向"); exit(1); }
                dup2(fout, STDOUT_FILENO); close(fout);
            }

            for (int j = 0; j < num_cmds - 1; j++) {
                close(pipes[j][0]); close(pipes[j][1]);
            }

            execvp(seg_argv[0], seg_argv);
            fprintf(stderr, "myshell: %s: 命令未找到\n", seg_argv[0]);
            exit(127);
        }
    }

    /* 父进程关闭所有管道 fd (关键!) */
    for (int i = 0; i < num_cmds - 1; i++) {
        close(pipes[i][0]); close(pipes[i][1]);
    }
    for (int i = 0; i < num_cmds; i++)
        waitpid(pids[i], NULL, 0);
}

/* ============================================================================
 *  builtin_cmd — 内置命令判断与执行
 * ============================================================================
 *  需求1: ls/cat/grep 为外部命令, 返回0
 *  需求5: type 判断顺序 别名->内置->外部
 *  需求6: cd 支持 ~/xxx
 *  需求9: echo 支持转义字符 \n \t \\
 */
int builtin_cmd(char **argv, int argc, int bg)
{
    (void)bg;
    if (argc == 0) return 0;

    /* cd */
    if (strcmp(argv[0], "cd") == 0) {
        const char *dir;
        if (argc < 2 || strcmp(argv[1], "~") == 0) {
            dir = getenv("HOME");
            if (!dir) { printf("myshell: cd: HOME 环境变量未设置\n"); return 1; }
        } else if (strncmp(argv[1], "~/", 2) == 0) {
            /* 需求6: ~/xxx 展开 */
            static char cd_path[MAX_LINE];
            const char *home = getenv("HOME");
            if (!home) { printf("myshell: cd: HOME 环境变量未设置\n"); return 1; }
            snprintf(cd_path, sizeof(cd_path), "%s%s", home, argv[1] + 1);
            dir = cd_path;
        } else if (strcmp(argv[1], "..") == 0) {
            dir = "..";
        } else {
            dir = argv[1];
        }
        if (chdir(dir) != 0)
            fprintf(stderr, "myshell: cd: %s: %s\n", dir, strerror(errno));
        return 1;
    }

    /* echo (需求9: 转义字符) */
    if (strcmp(argv[0], "echo") == 0) {
        for (int i = 1; i < argc; i++) {
            if (i > 1) printf(" ");
            const char *s = argv[i];
            while (*s) {
                if (*s == '$') {
                    s++;
                    char vn[256]; int vi = 0;
                    while (*s && *s!=' ' && *s!='\t' && *s!='\\' && vi<255)
                        vn[vi++] = *s++;
                    vn[vi] = '\0';
                    if (vi > 0) {
                        const char *val = getenv(vn);
                        printf("%s", val ? val : "");
                    } else { putchar('$'); }
                } else if (*s == '\\') {
                    s++;
                    switch (*s) {
                        case 'n': putchar('\n'); break;
                        case 't': putchar('\t'); break;
                        case '\\': putchar('\\'); break;
                        case '\0': putchar('\\'); return 1;
                        default: putchar('\\'); putchar(*s); break;
                    }
                    if (*s) s++;
                } else { putchar(*s); s++; }
            }
        }
        printf("\n");
        return 1;
    }

    /* type (需求5: 别名->内置->外部) */
    if (strcmp(argv[0], "type") == 0) {
        if (argc < 2) { printf("myshell: type: 用法: type 命令名\n"); return 1; }

        /* 第一优先级: 别名 */
        for (int k = 0; k < alias_count; k++)
            if (strcmp(argv[1], alias_table[k].name) == 0) {
                printf("%s 是别名: %s\n", argv[1], alias_table[k].value);
                return 1;
            }

        /* 第二优先级: 内置命令 */
        for (int k = 0; builtin_names[k]; k++)
            if (strcmp(argv[1], builtin_names[k]) == 0) {
                printf("%s 是 shell 内置命令\n", argv[1]);
                return 1;
            }

        /* 第三优先级: 外部程序 */
        const char *pe = getenv("PATH");
        char found[MAX_LINE] = "";
        if (pe) {
            char pc[MAX_LINE * 4];
            strncpy(pc, pe, sizeof(pc)-1); pc[sizeof(pc)-1] = '\0';
            char *sv, *d = strtok_r(pc, ":", &sv);
            while (d) {
                char full[MAX_LINE];
                snprintf(full, sizeof(full), "%s/%s", d, argv[1]);
                if (access(full, X_OK) == 0) { strcpy(found, full); break; }
                d = strtok_r(NULL, ":", &sv);
            }
        }
        if (found[0]) printf("%s 是 %s\n", argv[1], found);
        else printf("myshell: type: %s: 未找到命令\n", argv[1]);
        return 1;
    }

    /* exit */
    if (strcmp(argv[0], "exit") == 0) {
        int code = (argc >= 2) ? atoi(argv[1]) : 0;
        save_history();
        printf("exit\n");
        exit(code);
    }

    /* history/alias/unalias 已在其他函数处理 */
    if (strcmp(argv[0],"history")==0 || strcmp(argv[0],"alias")==0 ||
        strcmp(argv[0],"unalias")==0)
        return 1;

    /* 其他命令: 走外部程序路径 (需求1: ls/cat/grep 为外部命令) */
    return 0;
}

/* ============================================================================
 *  process_command — 处理单条命令 (主调度函数)
 */
static void process_command(char *line)
{
    char expanded[MAX_LINE], hist_out[MAX_LINE];
    int should_exec = 0;

    /* 步骤1: 历史命令处理 */
    history_manage(line, hist_out, &should_exec);
    if (should_exec) {
        strncpy(line, hist_out, MAX_LINE-1); line[MAX_LINE-1] = '\0';
    } else if (strcmp(line,"history")==0 || strncmp(line,"history ",8)==0) {
        return;
    } else if (line[0]=='!' && !should_exec) {
        return;
    }

    /* 步骤2: 别名展开 */
    if (alias_handle(line, expanded, 0)) return;

    char *cmd = expanded;
    while (*cmd==' '||*cmd=='\t') cmd++;
    if (*cmd=='\0') return;

    /* 步骤3: 管道检测 */
    if (strchr(cmd, '|')) { pipe_exec(cmd); return; }

    /* 步骤4: 参数分割 */
    char *argv[MAX_ARGS];
    int argc = split_cmd(cmd, argv, MAX_ARGS);
    if (argc == 0) return;

    /* 步骤5: 后台检测 */
    int bg = bg_run(argv, &argc);
    if (argc == 0) return;

    /* 步骤6: 重定向解析 */
    char *in_file = NULL, *out_file = NULL;
    int append_mode = 0;
    if (redirect_parse(argv, &argc, &in_file, &out_file, &append_mode) < 0)
        return;
    if (argc == 0) return;

    /* 步骤7: 执行命令 */
    if (builtin_cmd(argv, argc, bg)) return;
    execute_external(argv, bg, in_file, out_file, append_mode);
}

/* ============================================================================
 *  main — Shell 主循环
 */
int main(void)
{
    char input_buf[MAX_LINE];

    signal(SIGCHLD, sigchld_handler);
    signal(SIGINT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);

    load_history();

    printf("====================================\n");
    printf("  欢迎使用 myshell 简易命令解释器\n");
    printf("  输入 'exit' 或按 Ctrl+D 退出\n");
    printf("====================================\n");

    while (1) {
        reap_children();
        flush_bg_messages();
        print_prompt();

        int len = read_input(input_buf, MAX_LINE);
        if (len < 0) { save_history(); printf("exit\n"); break; }
        if (len == 0) continue;

        process_command(input_buf);
    }
    return 0;
}
