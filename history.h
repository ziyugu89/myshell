/*
 * ============================================================================
 *  history.h — 历史记录模块头文件
 * ============================================================================
 *  操作系统课程设计作业
 *
 *  功能: 历史命令管理、持久化读写、历史查找展开
 *  @author  胥思吉 23013152
 * ============================================================================
 */

#ifndef HISTORY_H
#define HISTORY_H

/* 历史命令管理: 添加、打印、!n/!!/!?查找 */
void history_manage(const char *cmd, char *out, int *should_exec);

/* 启动时从 ~/.myshell_history 加载历史命令 */
void load_history(void);

/* 退出时覆盖写入所有历史记录 */
void save_history(void);

/* 环形缓冲中最早仍有效的逻辑下标 */
int history_start_idx(void);

/* 按逻辑下标取历史条目, 无效时返回 NULL */
const char *history_get_at(int logical_idx);

#endif /* HISTORY_H */
