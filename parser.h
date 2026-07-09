/*
 * ============================================================================
 *  parser.h — 命令解析模块头文件
 * ============================================================================
 */

#ifndef PARSER_H
#define PARSER_H

typedef enum {
    CHAIN_NONE = 0,
    CHAIN_SEMI,
    CHAIN_AND,
    CHAIN_OR
} chain_op_t;

int split_cmd(const char *line, char **argv, int max_args);

/* 对 argv 中含 * ? 的参数做 glob 展开 */
int glob_expand_argv(char **argv, int *argc);

/* 行级检测与处理 */
int line_has_chain(const char *line);
int line_has_heredoc(const char *line);

/* 将 << DELIM 替换为 < 临时文件, 交互读取 heredoc 内容 */
int heredoc_process(char *line, int max_len);

/* 复合命令: ; && || */
int chain_exec(char *line);

#endif /* PARSER_H */
