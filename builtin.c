/*
 * ============================================================================
 *  builtin.c — 内置命令模块
 * ============================================================================
 *  操作系统课程设计作业
 *
 *  【文件定位】
 *    内置命令分发器，在 Shell 父进程内直接执行，不需要 fork+exec。
 *    这是与内核系统调用交互最密集的"业务逻辑"模块。
 *
 *  【与 Linux 内核的关系 — 各内置命令对应的系统调用】
 *    cd     : chdir() → 内核 sys_chdir，修改进程的当前工作目录
 *    pwd    : getcwd() → 内核 sys_getcwd，获取当前工作目录路径
 *    echo   : printf/putchar → 内核 write(1,...)，写标准输出
 *    type   : access() → 内核 sys_access，检查文件可执行权限(X_OK)
 *    exit   : exit() → 内核 sys_exit_group，终止整个进程组
 *    export : setenv() → 内核 sys_setenv(通过 /proc/self/environ)，设置环境变量
 *    unset  : unsetenv() → 内核 sys_unsetenv，删除环境变量
 *    jobs   : 纯用户态遍历作业表
 *    fg/bg  : kill(SIGCONT) → 内核 sys_kill，waitpid → 内核 sys_wait4
 *             tcsetpgrp → 内核 ioctl(TIOCSPGRP)
 *    kill   : kill() → 内核 sys_kill，向 PID 或作业进程组发信号
 *    wait   : waitpid() → 内核 sys_wait4，阻塞等待后台作业结束
 *    help   : 纯用户态，列出内置命令说明
 *    mount  : mount() → 内核 sys_mount，直接调用内核挂载系统调用(需root)
 *    umount : umount() → 内核 sys_umount，直接调用内核卸载系统调用(需root)
 *
 *  【重要设计原则】
 *    cd 必须在父进程执行！不能 fork，因为 chdir 只影响当前进程，
 *    子进程 cd 后父进程目录不变。
 *
 *  @author  操作系统课程设计
 * ============================================================================
 */

#include "shell.h"
#include "builtin.h"
#include "history.h"
#include "alias.h"
#include "mount.h"
#include "background.h"

extern char **environ;

const char *const shell_builtin_names[] = {
    "cd", "pwd", "echo", "type", "exit", "history", "alias", "unalias",
    "export", "unset", "jobs", "fg", "bg", "kill", "wait", "help",
    "mount", "umount",
    NULL
};

/* help 命令说明表（与 shell_builtin_names 对应） */
static const struct {
    const char *name;
    const char *desc;
} help_table[] = {
    {"cd",      "切换当前工作目录"},
    {"pwd",     "显示当前工作目录"},
    {"echo",    "输出文本（支持 -n 与转义）"},
    {"type",    "判断命令类型（别名/内置/外部）"},
    {"exit",    "退出 Shell"},
    {"history", "显示历史命令"},
    {"alias",   "定义或列出别名"},
    {"unalias", "删除别名"},
    {"export",  "设置或查看环境变量"},
    {"unset",   "删除环境变量"},
    {"jobs",    "列出后台/停止作业"},
    {"fg",      "将作业调到前台"},
    {"bg",      "让已停止作业在后台继续运行"},
    {"kill",    "向 PID 或作业发送信号"},
    {"wait",    "等待后台作业结束"},
    {"help",    "显示内置命令帮助"},
    {"mount",   "挂载文件系统或查看挂载列表"},
    {"umount",  "卸载文件系统"},
    {NULL, NULL}
};

/*
 * builtin_cmd — 内置命令分发器
 * 功能: 根据 argv[0] 识别并执行对应内置命令; 若非内置命令返回 -1。
 * 返回值: -1=非内置(降级为外部命令); >=0=内置命令退出码
 *
 * 【各命令对应的内核系统调用】
 *   cd     → chdir()  : 内核 sys_chdir
 *   pwd    → getcwd() : 内核 sys_getcwd
 *   echo   → printf   : 内核 write(1,...)
 *   type   → access() : 内核 sys_access(X_OK)
 *   exit   → exit()   : 内核 sys_exit_group
 *   export → setenv() : 修改环境变量(影响 fork 的子进程)
 *   unset  → unsetenv(): 删除环境变量
 *   jobs/fg/bg/kill/wait → 委托给 background.c (kill/waitpid/tcsetpgrp)
 *   help → 用户态帮助
 *   mount/umount → 委托给 mount.c (内核 mount/umount 系统调用)
 */
int builtin_cmd(char **argv, int argc, int bg)
{
    (void)bg;
    if (argc == 0) return -1;

    /* cd: 切换当前工作目录
     * 【必须】在父进程执行！chdir() 只影响当前进程，fork后子进程cd无效
     * 【系统调用】chdir() → 内核 sys_chdir，修改进程的 CWD(当前工作目录)
     * 【系统调用】getcwd() → 内核 sys_getcwd，保存旧目录到 OLDPWD
     * 【系统调用】setenv() → 设置 OLDPWD 环境变量 */
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

        /* 【系统调用】getcwd() → 内核 sys_getcwd，保存切换前的目录 */
        char oldcwd[MAX_LINE];
        if (getcwd(oldcwd, sizeof(oldcwd)))
            setenv("OLDPWD", oldcwd, 1);  /* 保存旧目录供 cd - 使用 */

        /* 【系统调用】chdir() → 内核 sys_chdir
         * 修改进程的当前工作目录，内核更新进程的 fs_struct->pwd */
        if (chdir(dir) != 0) {
            fprintf(stderr, "myshell: cd: %s: %s\n", dir, strerror(errno));
            return 1;
        }
        return 0;
    }

    /* pwd: 输出当前工作目录
     * 【系统调用】getcwd() → 内核 sys_getcwd
     *   内核从进程的 fs_struct->pwd 读取当前目录路径 */
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

    /* type: 判断命令类型(别名→内置→外部)
     * 【系统调用】access(full, X_OK) → 内核 sys_access
     *   检查文件是否可执行，内核检查 inode 的权限位和进程的有效 UID/GID
     *   遍历 PATH 环境变量中每个目录，检查 dir/cmd 是否可执行
     */
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
                /* 【系统调用】access() → 内核 sys_access，检查文件可执行性 */
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

    /* exit: 保存历史后按指定退出码终止 Shell 进程
     * 【系统调用】exit() → 内核 sys_exit_group
     *   内核释放进程资源(fd表、内存映射、信号队列)，终止整个进程组
     *   Shell 调用 exit 后永远不会返回 */
    if (strcmp(argv[0], "exit") == 0) {
        int code = (argc >= 2) ? atoi(argv[1]) : 0;
        save_history();
        save_alias();
        printf("exit\n");
        exit(code);
    }

    /* export: 设置/查看环境变量
     * 【库函数】setenv()/getenv() → 操作进程的环境变量空间
     *   环境变量在内核 fork 时通过复制环境块传递给子进程
     *   export KEY=VALUE 设置后，后续 fork 的子进程都能看到这个变量
     */
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

    /* unset: 删除环境变量
     * 【库函数】unsetenv() → 从进程环境块中删除指定变量
     *   影响当前进程及后续 fork 的子进程 */
    if (strcmp(argv[0], "unset") == 0) {
        if (argc < 2) {
            printf("myshell: unset: 用法: unset 变量名\n");
            return 1;
        }
        for (int i = 1; i < argc; i++)
            unsetenv(argv[i]);
        return 0;
    }

    /* jobs/fg/bg/kill/wait: 委托给 background.c 的作业控制函数
     * 这些命令涉及内核系统调用:
     *   kill(-pgid, SIG) → 内核 sys_kill，向进程组发信号
     *   waitpid() → 内核 sys_wait4，等待子进程
     *   tcsetpgrp() → 内核 ioctl(TIOCSPGRP)，切换前台进程组 */
    if (strcmp(argv[0], "jobs") == 0)
        return builtin_jobs(argv, argc);
    if (strcmp(argv[0], "fg") == 0)
        return builtin_fg(argv, argc);
    if (strcmp(argv[0], "bg") == 0)
        return builtin_bg(argv, argc);
    if (strcmp(argv[0], "kill") == 0)
        return builtin_kill(argv, argc);
    if (strcmp(argv[0], "wait") == 0)
        return builtin_wait(argv, argc);

    /* help: 列出内置命令说明（纯用户态） */
    if (strcmp(argv[0], "help") == 0) {
        if (argc < 2) {
            printf("myshell 内置命令:\n");
            for (int i = 0; help_table[i].name; i++)
                printf("  %-10s %s\n", help_table[i].name, help_table[i].desc);
            printf("输入 help <命令> 查看单条说明。\n");
            return 0;
        }
        for (int i = 0; help_table[i].name; i++) {
            if (strcmp(argv[1], help_table[i].name) == 0) {
                printf("%s — %s\n", help_table[i].name, help_table[i].desc);
                return 0;
            }
        }
        printf("myshell: help: %s: 未知内置命令\n", argv[1]);
        return 1;
    }

    /* mount/umount: 委托给 mount.c 的挂载管理函数
     * 这些是直接调用内核的系统调用(不是用户态库封装):
     *   mount()  → 内核 sys_mount，挂载文件系统
     *   umount() → 内核 sys_umount，卸载文件系统
     * 需要 root 权限(geteuid()==0) */
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
