/*
 * ============================================================================
 *  redirect.h — 重定向处理模块头文件
 * ============================================================================
 */

#ifndef REDIRECT_H
#define REDIRECT_H

typedef struct {
    char *in_file;
    char *out_file;
    char *err_file;
    int   append_out;
    int   append_err;
    int   err_to_out;
} redirect_t;

void redirect_init(redirect_t *r);

/* 从 argv 中提取 < > >> 2> 2>> 2>&1 */
int redirect_parse(char **argv, int *argc, redirect_t *r);

/* 在当前进程应用重定向, 成功返回 0 */
int redirect_apply(const redirect_t *r);

/* 保存 fd 后应用, 返回 saved fd (失败返回 -1) */
int redirect_apply_with_save(const redirect_t *r,
                             int *saved_in, int *saved_out, int *saved_err);

void redirect_restore(int saved_in, int saved_out, int saved_err);

#endif /* REDIRECT_H */
