/*
 * ============================================================================
 *  builtin.c — 内置命令模块
 * ============================================================================
 */

#include "shell.h"
#include "builtin.h"
#include "history.h"
#include "mount.h"
#include "background.h"

extern char **environ;

const char *const shell_builtin_names[] = {
    "cd", "pwd", "echo", "type", "exit", "history", "alias", "unalias",
    "export", "unset", "jobs", "fg", "bg",
    "mount", "umount",
    NULL
};

/*
 * builtin_cmd — 内置命令分发器
 * 功能: 根据 argv[0] 识别并执行对应内置命令; 若非内置命令返回 -1。
 * 实现: 用 strcmp 链式判断 argv[0], 分别调用 chdir/getcwd/printf/setenv/
 *       exit 等系统调用/库函数完成具体功能; history/alias/unalias 已在
 *       流程早期处理, 这里仅返回 0 占位避免被误判为外部命令。
 */
int builtin_cmd(char **argv, int argc, int bg)
{
    (void)bg;
    if (argc == 0) return -1;

    /* cd: 切换当前工作目录(必须在父进程执行,不能fork) */
    if (strcmp(argv[0], "cd") == 0) {
        const char *dir;
        static char cd_path[MAX_LINE];

        if (argc < 2 || strcmp(argv[1], "~") == 0) {
            dir = getenv("HOME");
            if (!dir) {
                printf("myshell: cd: HOME 环境变量未设置\n");
                return 1;
            }
        } else if (strcmp(argv[1], "-") == 0) {
            dir = getenv("OLDPWD");
            if (!dir) {
                printf("myshell: cd: OLDPWD 未设置\n");
                return 1;
            }
        } else if (strncmp(argv[1], "~/", 2) == 0) {
            const char *home = getenv("HOME");
            if (!home) {
                printf("myshell: cd: HOME 环境变量未设置\n");
                return 1;
            }
            snprintf(cd_path, sizeof(cd_path), "%s%s",
                     home, argv[1] + 1);
            dir = cd_path;
        } else {
            dir = argv[1];
        }

        char oldcwd[MAX_LINE];
        if (getcwd(oldcwd, sizeof(oldcwd)))
            setenv("OLDPWD", oldcwd, 1);

        if (chdir(dir) != 0) {
            fprintf(stderr, "myshell: cd: %s: %s\n", dir, strerror(errno));
            return 1;
        }
        return 0;
    }

    /* pwd: 调用 getcwd 输出当前工作目录 */
    if (strcmp(argv[0], "pwd") == 0) {
        char cwd[MAX_LINE];
        if (getcwd(cwd, sizeof(cwd))) {
            printf("%s\n", cwd);
            return 0;
        }
        fprintf(stderr, "myshell: pwd: %s\n", strerror(errno));
        return 1;
    }

    /* echo: 输出参数,支持 -n 和 \\n/\\t/\\\\ 转义 */
    if (strcmp(argv[0], "echo") == 0) {
        int start = 1;
        int no_nl = 0;
        if (argc > 1 && strcmp(argv[1], "-n") == 0) {
            start = 2;
            no_nl = 1;
        } else if (argc > 1 && strcmp(argv[1], "-e") == 0) {
            start = 2;
        }
        for (int i = start; i < argc; i++) {
            if (i > start) printf(" ");
            const char *s = argv[i];
            while (*s) {
                if (*s == '\\' && *(s + 1)) {
                    s++;
                    switch (*s) {
                        case 'n':  putchar('\n'); break;
                        case 't':  putchar('\t'); break;
                        case '\\': putchar('\\'); break;
                        default:   putchar('\\'); putchar(*s); break;
                    }
                    s++;
                } else {
                    putchar(*s);
                    s++;
                }
            }
        }
        if (!no_nl)
            printf("\n");
        return 0;
    }

    /* type: 判断命令类型(别名→内置→外部),外部命令遍历PATH检查X_OK */
    if (strcmp(argv[0], "type") == 0) {
        if (argc < 2) {
            printf("myshell: type: 用法: type 命令名\n");
            return 1;
        }
        for (int k = 0; k < alias_count; k++)
            if (strcmp(argv[1], alias_table[k].name) == 0) {
                printf("%s 是别名: %s\n", argv[1], alias_table[k].value);
                return 0;
            }
        for (int k = 0; shell_builtin_names[k]; k++)
            if (strcmp(argv[1], shell_builtin_names[k]) == 0) {
                printf("%s 是 shell 内置命令\n", argv[1]);
                return 0;
            }
        const char *pe = getenv("PATH");
        char found[MAX_LINE] = "";
        if (pe) {
            char pc[MAX_LINE * 4];
            strncpy(pc, pe, sizeof(pc) - 1);
            pc[sizeof(pc) - 1] = '\0';
            char *sv, *d = strtok_r(pc, ":", &sv);
            while (d) {
                char full[MAX_LINE];
                snprintf(full, sizeof(full), "%s/%s", d, argv[1]);
                if (access(full, X_OK) == 0) {
                    strncpy(found, full, MAX_LINE - 1);
                    found[MAX_LINE - 1] = '\0';
                    break;
                }
                d = strtok_r(NULL, ":", &sv);
            }
        }
        if (found[0]) printf("%s 是 %s\n", argv[1], found);
        else printf("myshell: type: %s: 未找到命令\n", argv[1]);
        return found[0] ? 0 : 1;
    }

    /* exit: 保存历史后按指定退出码终止Shell进程 */
    if (strcmp(argv[0], "exit") == 0) {
        int code = (argc >= 2) ? atoi(argv[1]) : 0;
        save_history();
        printf("exit\n");
        exit(code);
    }

    /* export: 设置/查看环境变量,通过 setenv 影响当前进程及子进程 */
    if (strcmp(argv[0], "export") == 0) {
        if (argc < 2) {
            for (char **e = environ; e && *e; e++)
                printf("export %s\n", *e);
            return 0;
        }
        for (int i = 1; i < argc; i++) {
            char *eq = strchr(argv[i], '=');
            if (!eq) {
                const char *val = getenv(argv[i]);
                if (val) printf("export %s='%s'\n", argv[i], val);
                else printf("myshell: export: %s: 未设置\n", argv[i]);
            } else {
                *eq = '\0';
                setenv(argv[i], eq + 1, 1);
                *eq = '=';
            }
        }
        return 0;
    }

    /* unset: 删除环境变量,调用 unsetenv */
    if (strcmp(argv[0], "unset") == 0) {
        if (argc < 2) {
            printf("myshell: unset: 用法: unset 变量名\n");
            return 1;
        }
        for (int i = 1; i < argc; i++)
            unsetenv(argv[i]);
        return 0;
    }

    /* jobs/fg/bg: 委托给 background.c 的作业控制函数 */
    if (strcmp(argv[0], "jobs") == 0)
        return builtin_jobs(argv, argc);
    if (strcmp(argv[0], "fg") == 0)
        return builtin_fg(argv, argc);
    if (strcmp(argv[0], "bg") == 0)
        return builtin_bg(argv, argc);

    /* mount/umount: 委托给 mount.c 的挂载管理函数 */
    if (strcmp(argv[0], "mount") == 0) {
        handle_mount(argv, argc);
        return 0;
    }
    if (strcmp(argv[0], "umount") == 0) {
        handle_umount(argv, argc);
        return 0;
    }

    /* history/alias/unalias 已在 process_command 流程早期处理,此处占位返回 */
    if (strcmp(argv[0], "history") == 0 ||
        strcmp(argv[0], "alias") == 0 ||
        strcmp(argv[0], "unalias") == 0)
        return 0;

    return -1;
}
