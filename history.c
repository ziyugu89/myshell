/*
 * ============================================================================
 *  history.c — 历史记录模块
 * ============================================================================
 *  操作系统课程设计作业
 *
 *  【文件定位】
 *    状态持久层，负责历史命令的存储与展开。
 *    使用环形数组实现 O(1) 访问，持久化到 ~/.myshell_history。
 *
 *  【与 Linux 内核的关系】
 *    - fopen/fclose : 内核 sys_open/sys_close，通过 VFS 打开文件
 *    - fgets/fprintf: 内核 read/write，通过 VFS 读写文件内容
 *    - getenv("HOME"): 读取环境变量，HOME 由登录时内核/pam 设置
 *    本模块不涉及进程控制类系统调用，主要做字符串操作和文件 I/O
 *
 *  【核心功能】
 *    history_manage()    — 历史管理入口: history/!!/!n/!?kw/添加历史
 *    load_history()      — 启动时从文件加载历史
 *    save_history()      — 退出时保存历史到文件
 *    history_start_idx() — 环形缓冲区最旧有效下标
 *    history_get_at()    — 按逻辑下标 O(1) 访问
 *
 *  【环形缓冲区原理】
 *    history_list[count % MAX_HISTORY] 取模索引
 *    满时自动覆盖最旧条目，只保留最近 MAX_HISTORY 条
 *
 *  @author  胥思吉 23013152
 * ============================================================================
 */

#include "shell.h"
#include "history.h"

/*
 * history_start_idx — 计算当前有效历史的最旧逻辑下标
 * 功能: 环形缓冲区满时,保证 history/!n 只访问最近 MAX_HISTORY 条。
 * 实现: history_count > MAX_HISTORY 时返回 history_count - MAX_HISTORY,否则 0。
 */
int history_start_idx(void)
{
    return (history_count > MAX_HISTORY) ? (history_count - MAX_HISTORY) : 0;
}

/*
 * history_get_at — 按逻辑下标获取历史条目
 * 功能: 支持 O(1) 访问任意有效历史命令。
 * 实现: 先校验范围,再取 history_list[logical_idx % MAX_HISTORY]。
 */
const char *history_get_at(int logical_idx)
{
    if (logical_idx < history_start_idx() || logical_idx >= history_count)
        return NULL;
    return history_list[logical_idx % MAX_HISTORY];
}

/*
 * load_history — 启动时加载历史命令
 * 功能: 从 ~/.myshell_history 读取上次会话保存的历史。
 * 【系统调用】
 *   - fopen() → 内核 sys_open，打开文件获取 fd
 *   - fgets() → 内核 read，逐行读取文件内容
 *   - fclose() → 内核 sys_close，关闭文件
 *   - getenv("HOME") → 读取环境变量构造路径
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

/*
 * save_history — 退出时保存历史命令
 * 功能: 将本次会话历史覆盖写入 ~/.myshell_history。
 * 【系统调用】
 *   - fopen("w") → 内核 sys_open(O_WRONLY|O_CREAT|O_TRUNC)
 *   - fprintf() → 内核 write，写入文件
 *   - fclose() → 内核 sys_close
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

/*
 * history_manage — 历史命令管理与展开
 * 功能: 处理 history 命令、!!、!n、!?keyword，并将普通命令加入历史。
 * 【实现逻辑】
 *   - "history"    : 打印全部历史(环形缓冲区有效部分)
 *   - "history n"  : 打印最近 n 条
 *   - "!!"         : 执行上一条命令，复制到 out 并置 should_exec=1
 *   - "!n"         : 按编号执行，O(1) 定位 (n-1) % MAX_HISTORY
 *   - "!?keyword"  : 搜索含关键词的历史(从新到旧)
 *   - 普通命令     : 追加到环形缓冲区
 * 纯用户态逻辑，不涉及内核系统调用
 */
void history_manage(const char *cmd, char *out, int *should_exec)
{
    *should_exec = 0; out[0] = '\0';

    /* history: 打印全部历史 */
    if (strcmp(cmd, "history") == 0) {
        int start = (history_count > MAX_HISTORY) ? (history_count - MAX_HISTORY) : 0;
        for (int i = start; i < history_count; i++)
            printf("  %d  %s\n", i + 1, history_list[i % MAX_HISTORY]);
        return;
    }
    /* history n: 打印最近n条 */
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
    /* !!: 执行上一条历史命令 */
    if (strcmp(cmd, "!!") == 0) {
        const char *entry = history_get_at(history_count - 1);
        if (!entry) { printf("myshell: !!: 没有历史命令\n"); return; }
        strcpy(out, entry);
        printf("%s\n", out); *should_exec = 1; return;
    }
    /* !n: 按编号执行历史命令 */
    if (cmd[0] == '!' && cmd[1] >= '0' && cmd[1] <= '9') {
        int n = atoi(cmd + 1);
        int start_num = (history_count > MAX_HISTORY)
                        ? (history_count - MAX_HISTORY + 1) : 1;
        if (n < start_num || n > history_count) {
            printf("myshell: !%d: 事件未找到\n", n); return;
        }
        const char *entry = history_list[(n - 1) % MAX_HISTORY];
        strcpy(out, entry);
        printf("%s\n", out); *should_exec = 1; return;
    }
    /* !?keyword: 搜索包含关键词的历史命令 */
    if (cmd[0] == '!' && cmd[1] == '?') {
        const char *kw = cmd + 2;
        if (*kw == '\0') { printf("myshell: !?: 缺少搜索关键词\n"); return; }
        int start = history_start_idx();
        for (int i = history_count - 1; i >= start; i--) {
            const char *entry = history_get_at(i);
            if (entry && strstr(entry, kw)) {
                strcpy(out, entry);
                printf("%s\n", out); *should_exec = 1; return;
            }
        }
        printf("myshell: !?%s: 未找到匹配的历史命令\n", kw); return;
    }
    /* 普通命令: 添加到历史记录 */
    if (strlen(cmd) > 0) {
        int idx = history_count % MAX_HISTORY;
        strncpy(history_list[idx], cmd, MAX_LINE - 1);
        history_list[idx][MAX_LINE - 1] = '\0';
        history_count++;
    }
}
