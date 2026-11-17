#ifndef FULMEN_VFS_H
#define FULMEN_VFS_H

#include "types.h"

#define VFS_MAX_NODES   128U
#define VFS_NAME_MAX    48U
#define VFS_PATH_MAX    256U

enum vfs_type {
    VFS_UNUSED = 0,
    VFS_FILE   = 1,
    VFS_DIR    = 2
};

struct vfs_node {
    uint32_t id;
    char name[VFS_NAME_MAX];
    enum vfs_type type;
    uint32_t mode;
    uint64_t created_tick;
    uint64_t modified_tick;
    size_t size;
    size_t capacity;
    uint8_t *data;

    struct vfs_node *parent;
    struct vfs_node *first_child;
    struct vfs_node *next_sibling;
};

struct vfs_stats {
    size_t max_inodes;
    size_t used_inodes;
    size_t file_count;
    size_t dir_count;
    size_t used_bytes;
};

void vfs_init(void);
void initrd_populate(void);

struct vfs_node *vfs_get_root(void);
struct vfs_node *vfs_get_cwd(void);
void vfs_get_path(const struct vfs_node *node, char *buf, size_t buf_size);
int vfs_chdir(const char *path);

struct vfs_node *vfs_resolve(const char *path);
struct vfs_node *vfs_mkdir(const char *path);
struct vfs_node *vfs_touch(const char *path);
int vfs_write(const char *path, const void *data, size_t size, bool append);
ssize_t vfs_read(const char *path, void *buf, size_t max_size);
int vfs_remove(const char *path, bool recursive);
int vfs_copy(const char *src_path, const char *dst_path);
int vfs_move(const char *src_path, const char *dst_path);

void vfs_get_stats(struct vfs_stats *out);

#endif /* FULMEN_VFS_H */
