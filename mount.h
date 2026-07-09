/*
 * ============================================================================
 *  mount.h — 挂载管理模块头文件
 * ============================================================================
 *  操作系统课程设计作业
 *
 *  功能: mount/umount 内置命令处理
 *  @author  操作系统课程设计
 * ============================================================================
 */

#ifndef MOUNT_H
#define MOUNT_H

/* 处理 mount 命令 */
int handle_mount(char **argv, int argc);

/* 处理 umount 命令 */
int handle_umount(char **argv, int argc);

/* 显示当前挂载列表 */
void show_mounts(void);

#endif /* MOUNT_H */
