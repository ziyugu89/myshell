/*
 * ============================================================================
 *  alias.c — 别名管理模块
 * ============================================================================
 *  操作系统课程设计作业
 *
 *  功能: alias_handle — alias/unalias 命令处理与递归多层展开(最多10层)
 *  @author  胥思吉 23013152
 * ============================================================================
 */

#include "shell.h"
#include "alias.h"

/*
 * alias_handle — 别名管理与递归展开
 * 功能: 处理 alias/unalias 命令,或对普通命令行进行别名递归展开。
 * 实现: 提取第一个词;若是 alias/unalias 则直接操作 alias_table 并返回 1;
 *       否则查找别名表,命中则把别名值+后续参数拼成新行并递归(depth+1),
 *       受 MAX_ALIAS_DEPTH 限制防死循环;未命中则原样复制。
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
