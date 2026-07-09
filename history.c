/*
 * ============================================================================
 *  history.c — 历史记录模块
 * ============================================================================
 *  操作系统课程设计作业
 *
 *  功能: history_manage(历史命令管理)、load_history(加载)、
 *         save_history(保存)、!n/!!/!?查找展开
 *  @author  胥思吉 23013152
 * ============================================================================
 */

#include "shell.h"
#include "history.h"

int history_start_idx(void)
{
    return (history_count > MAX_HISTORY) ? (history_count - MAX_HISTORY) : 0;
}

const char *history_get_at(int logical_idx)
{
    if (logical_idx < history_start_idx() || logical_idx >= history_count)
        return NULL;
    return history_list[logical_idx % MAX_HISTORY];
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
 *  history_manage — 历史命令管理
 * ============================================================================
 *  处理 history 命令打印、!!、!n、!?keyword 查找,
 *  以及普通命令的历史记录添加。
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
