/*
 * ============================================================================
 *  mount.c — 挂载管理模块
 * ============================================================================
 *  操作系统课程设计作业
 *
 *  【文件定位】
 *    直接调用内核 mount/umount 系统调用的模块，是本项目与内核 VFS 层
 *    交互最直接的例子。需要 root 权限才能执行挂载/卸载操作。
 *
 *  【与 Linux 内核的关系 — 核心系统调用详解】
 *    - mount(source, target, fstype, flags, data) : 内核 sys_mount
 *      这是直接的内核系统调用，不是用户态封装！
 *      内核将源设备(如 /dev/sda1)挂载到目标目录(如 /mnt)
 *      内核 VFS 层将文件系统操作路由到对应的文件系统驱动
 *    - umount(target) : 内核 sys_umount
 *      卸载指定挂载点，内核从 VFS 挂载表中移除条目
 *    - geteuid() : 内核 sys_geteuid
 *      获取有效用户 ID，判断是否有权限执行特权操作
 *    - fopen("/proc/mounts") : 内核 procfs 提供的虚拟文件
 *      /proc/mounts 内容由内核动态生成，反映当前真实的挂载状态
 *      这是 Linux 特有的 proc 文件系统机制
 *
 *  【为什么需要 root 权限】
 *    mount/umount 是特权操作，因为挂载可以覆盖任意目录，
 *    普通用户可能通过挂载恶意思文件系统来替换系统关键目录。
 *    内核在 sys_mount 中检查 CAP_SYS_ADMIN 能力。
 *
 *  @author  操作系统课程设计
 * ============================================================================
 */

/* 启用 GNU 扩展, 使 <sys/mount.h> 中的 mount()/umount() 声明可见 */
#define _GNU_SOURCE

#include "shell.h"
#include "mount.h"
#include <sys/mount.h>

/*
 * check_root — 检查当前进程是否拥有 root 权限
 * 功能: mount/umount 系统调用需要特权。
 * 【系统调用】geteuid() → 内核 sys_geteuid
 *   获取进程的有效用户 ID，root 的 UID 为 0
 *   内核在 sys_mount 中也会检查 CAP_SYS_ADMIN 能力
 */
static int check_root(void)
{
    if (geteuid() != 0) {
        fprintf(stderr, "myshell: mount/umount: 需要 root 权限\n");
        return 0;
    }
    return 1;
}

/*
 * show_mounts — 显示当前系统挂载列表
 * 功能: 实现无参数 mount 命令。
 * 【与内核的关系】
 *   /proc/mounts 是内核 procfs 提供的虚拟文件
 *   内容由内核动态生成，反映 VFS 挂载表的真实状态
 *   每行格式: 设备 挂载点 文件系统类型 选项 dump pass
 *   这是 Linux 特有的内核机制，属于 proc 文件系统子系统
 */
void show_mounts(void)
{
    /* 打开 /proc/mounts → 内核 procfs 提供的虚拟文件
     * 内容由内核动态生成，反映当前 VFS 挂载表 */
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

/*
 * handle_mount — 处理 mount 内置命令
 * 功能: 无参数时显示挂载列表，三参数时执行挂载。
 * 【系统调用】mount(source, target, fstype, 0, NULL) → 内核 sys_mount
 *   内核将源设备挂载到目标目录，VFS 层将文件操作路由到对应文件系统
 *   例如: mount /dev/sda1 /mnt ext4 表示将 ext4 格式的分区挂载到 /mnt
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
        /* 【系统调用】mount() → 内核 sys_mount
         * 这是直接的内核系统调用，将文件系统挂载到目录树
         * 内核检查 CAP_SYS_ADMIN 能力，无权限则返回 EPERM */
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

/*
 * handle_umount — 处理 umount 内置命令
 * 功能: 卸载指定挂载点。
 * 【系统调用】umount(target) → 内核 sys_umount
 *   内核从 VFS 挂载表中移除指定挂载点
 *   如果仍有进程在使用该挂载，内核返回 EBUSY
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

    /* 【系统调用】umount() → 内核 sys_umount，卸载文件系统 */
    if (umount(argv[1]) != 0) {
        fprintf(stderr, "myshell: umount: %s\n", strerror(errno));
        return 1;
    }
    printf("卸载成功\n");
    return 1;
}
