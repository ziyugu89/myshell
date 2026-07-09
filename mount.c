/*
 * ============================================================================
 *  mount.c — 挂载管理模块
 * ============================================================================
 *  操作系统课程设计作业
 *
 *  功能: handle_mount(挂载/查看)、handle_umount(卸载)、show_mounts(显示挂载列表)
 *  @author  操作系统课程设计
 * ============================================================================
 */

/* 启用 GNU 扩展, 使 <sys/mount.h> 中的 mount()/umount() 声明可见 */
#define _GNU_SOURCE

#include "shell.h"
#include "mount.h"
#include <sys/mount.h>

/* ============================================================================
 *  check_root — 检查当前用户是否拥有 root 权限
 * ============================================================================
 *  mount/umount 系统调用需要 root 权限, 非 root 时打印错误并返回 0。
 */
static int check_root(void)
{
    if (geteuid() != 0) {
        fprintf(stderr, "myshell: mount/umount: 需要 root 权限\n");
        return 0;
    }
    return 1;
}

/* ============================================================================
 *  show_mounts — 显示当前挂载列表
 * ============================================================================
 *  优先读取 /proc/mounts, 失败时回退到 /etc/mtab, 逐行输出。
 */
void show_mounts(void)
{
    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp) {
        fp = fopen("/etc/mtab", "r");
        if (!fp) {
            fprintf(stderr, "myshell: mount: 无法读取挂载信息: %s\n",
                    strerror(errno));
            return;
        }
    }
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), fp))
        fputs(line, stdout);
    fclose(fp);
}

/* ============================================================================
 *  handle_mount — 处理 mount 命令
 * ============================================================================
 *  argc==1: 显示当前挂载列表
 *  argc==4: 执行挂载操作 mount(源设备, 挂载点, 文件系统类型)
 *  其他:    打印用法提示
 */
int handle_mount(char **argv, int argc)
{
    (void)argv;

    /* 无参数: 显示挂载列表 */
    if (argc == 1) {
        show_mounts();
        return 1;
    }

    /* 三个参数: 执行挂载 */
    if (argc == 4) {
        if (!check_root())
            return 1;
        if (mount(argv[1], argv[2], argv[3], 0, NULL) != 0) {
            fprintf(stderr, "myshell: mount: %s\n", strerror(errno));
            return 1;
        }
        printf("挂载成功\n");
        return 1;
    }

    /* 参数个数不正确 */
    fprintf(stderr, "myshell: mount: 用法: mount 源设备 挂载点 文件系统类型\n");
    return 1;
}

/* ============================================================================
 *  handle_umount — 处理 umount 命令
 * ============================================================================
 *  argc==2: 执行卸载操作 umount(挂载点)
 *  其他:    打印用法提示
 */
int handle_umount(char **argv, int argc)
{
    (void)argv;

    /* 参数个数不正确 */
    if (argc != 2) {
        fprintf(stderr, "myshell: umount: 用法: umount 挂载点\n");
        return 1;
    }

    if (!check_root())
        return 1;

    if (umount(argv[1]) != 0) {
        fprintf(stderr, "myshell: umount: %s\n", strerror(errno));
        return 1;
    }
    printf("卸载成功\n");
    return 1;
}
