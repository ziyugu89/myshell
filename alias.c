/*
 * ============================================================================
 *  alias.c — 别名管理模块
 * ============================================================================
 *  操作系统课程设计作业
 *
 *  【文件定位】
 *    别名管理与递归展开层，在命令处理流水线的第二步执行。
 *    别名仅存储在内存中(alias_table)，进程退出即丢失。
 *
 *  【与 Linux 内核的关系】
 *    本模块不涉及任何系统调用，纯用户态字符串操作。
 *    别名表是进程内存中的数组，由 Shell 自己管理。
 *    递归展开模拟了 bash 的别名解析机制(最多 10 层防死循环)。
 *
 *  【核心功能】
 *    alias_handle() — 别名管理与递归展开入口
 *      - "alias"      : 设置或显示别名
 *      - "unalias"    : 删除别名
 *      - 普通命令     : 查找别名表，命中则递归展开
 *
 *  @author  胥思吉 23013152
 * ============================================================================
 */

#include "shell.h"
#include "alias.h"

/*
 * alias_handle — 别名管理与递归展开
 * 功能: 处理 alias/unalias 命令，或对普通命令行进行别名递归展开。
 * 【实现逻辑】
 *   1. 提取第一个词(first)作为候选别名
 *   2. "alias"       : 操作 alias_table，设置/显示别名，返回 1
 *   3. "unalias"     : 从 alias_table 中删除别名，返回 1
 *   4. 递归展开保护 : depth > MAX_ALIAS_DEPTH(10) 时停止
 *   5. 别名查找      : 遍历 alias_table 匹配 first，
 *                      命中则将 别名值+剩余参数 拼成新行递归展开
 *   6. 未命中        : 原样复制到 expanded
 * 纯用户态操作，不涉及任何内核系统调用
 */
int alias_handle(const char *line, char *expanded, int depth)
{
    char first[128]; int i = 0;
    while (line[i]==' '||line[i]=='\t') i++;
    /* 提取第一个词 */
    int j = 0;
    while (line[i] && line[i]!=' ' && line[i]!='\t' && j<127) first[j++]=line[i++];
    first[j] = '\0';
    
    /* rest 指向第一个词后面的内容 */
    const char *rest = line + i;
    while (*rest==' '||*rest=='\t') rest++;

    /* alias 命令: 设置或显示别名 */
    if (strcmp(first, "alias") == 0) {
        if (*rest == '\0') {
            /* 无参数: 显示所有别名 */
            for (int k=0; k<alias_count; k++)
                printf("alias %s='%s'\n", alias_table[k].name, alias_table[k].value);
        } else {
            /* 有参数: 设置别名 */
            char arg[512];
            strncpy(arg, rest, sizeof(arg)-1); arg[sizeof(arg)-1]='\0';
            char *eq = strchr(arg, '=');
            if (!eq) { printf("myshell: alias: 用法: alias 名称='命令'\n"); return 1; }
            *eq = '\0'; char *name = arg; char *val = eq + 1;
            /* 去除引号包裹 */
            size_t vl = strlen(val);
            if (vl>=2 && val[0]=='\'' && val[vl-1]=='\'') { val[vl-1]='\0'; val++; }
            vl = strlen(val);
            if (vl>=2 && val[0]=='"' && val[vl-1]=='"') { val[vl-1]='\0'; val++; }
            /* 查找已有别名则更新 */
            for (int k=0; k<alias_count; k++)
                if (strcmp(alias_table[k].name, name)==0) {
                    strncpy(alias_table[k].value, val, 511);
                    alias_table[k].value[511]='\0'; return 1;
                }
            if (alias_count >= MAX_ALIAS) {
                printf("myshell: alias: 别名数量已达上限 (%d)\n", MAX_ALIAS); return 1;
            }
            /* 新增别名 */
            strncpy(alias_table[alias_count].name, name, 127);
            alias_table[alias_count].name[127]='\0';
            strncpy(alias_table[alias_count].value, val, 511);
            alias_table[alias_count].value[511]='\0';
            alias_count++;
        }
        return 1;
    }

    /* unalias 命令: 删除别名 */
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

    /* 递归展开保护: 防止无限嵌套 */
    if (depth > MAX_ALIAS_DEPTH) {
        fprintf(stderr, "myshell: 别名展开嵌套过深 (超过%d层)\n", MAX_ALIAS_DEPTH);
        strncpy(expanded, line, MAX_LINE-1); expanded[MAX_LINE-1]='\0'; return 0;
    }

    /* 别名展开: 查找第一个词是否匹配已注册别名 */
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

    /* 非别名: 原样复制 */
    strncpy(expanded, line, MAX_LINE-1); expanded[MAX_LINE-1]='\0';
    return 0;
}
