/*
 * ============================================================================
 *  parser.h — 命令解析模块头文件
 * ============================================================================
 */

#ifndef PARSER_H
#define PARSER_H

int split_cmd(const char *line, char **argv, int max_args);

/* 对 argv 中含 * ? 的参数做 glob 展开 */
int glob_expand_argv(char **argv, int *argc);

#endif /* PARSER_H */
