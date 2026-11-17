#include "vfs.h"
#include "heap.h"
#include "timer.h"
#include "string.h"

static struct vfs_node nodes[VFS_MAX_NODES];
static struct vfs_node *root_node = NULL;
static struct vfs_node *cwd_node = NULL;
static uint32_t next_inode_id = 1;

static struct vfs_node *alloc_node(const char *name, enum vfs_type type, struct vfs_node *parent) {
    for (size_t i = 0; i < VFS_MAX_NODES; i++) {
        if (nodes[i].type == VFS_UNUSED) {
            memset(&nodes[i], 0, sizeof(nodes[i]));
            nodes[i].id = next_inode_id++;
            strncpy(nodes[i].name, name, VFS_NAME_MAX - 1);
            nodes[i].type = type;
            nodes[i].mode = (type == VFS_DIR) ? 0755U : 0644U;
            nodes[i].created_tick = timer_get_ticks();
            nodes[i].modified_tick = nodes[i].created_tick;
            nodes[i].parent = parent;
            if (parent && parent != &nodes[i]) {
                nodes[i].next_sibling = parent->first_child;
                parent->first_child = &nodes[i];
            }
            return &nodes[i];
        }
    }
    return NULL;
}

static struct vfs_node *find_child(struct vfs_node *dir, const char *name) {
    if (!dir || dir->type != VFS_DIR) return NULL;
    if (strcmp(name, ".") == 0) return dir;
    if (strcmp(name, "..") == 0) return dir->parent ? dir->parent : dir;
    struct vfs_node *c = dir->first_child;
    while (c) {
        if (strcmp(c->name, name) == 0) return c;
        c = c->next_sibling;
    }
    return NULL;
}

static void detach_node(struct vfs_node *node) {
    struct vfs_node *p = node->parent;
    if (!p || p == node) return;
    struct vfs_node **link = &p->first_child;
    while (*link) {
        if (*link == node) {
            *link = node->next_sibling;
            node->next_sibling = NULL;
            return;
        }
        link = &(*link)->next_sibling;
    }
}

static void free_node_recursive(struct vfs_node *node) {
    if (!node || node == root_node) return;
    while (node->first_child) {
        struct vfs_node *child = node->first_child;
        detach_node(child);
        free_node_recursive(child);
    }
    if (node->data) {
        kfree(node->data);
        node->data = NULL;
    }
    memset(node, 0, sizeof(*node));
    node->type = VFS_UNUSED;
}

void vfs_init(void) {
    memset(nodes, 0, sizeof(nodes));
    next_inode_id = 1;
    root_node = alloc_node("/", VFS_DIR, NULL);
    root_node->parent = root_node;
    cwd_node = root_node;
}

struct vfs_node *vfs_get_root(void) {
    return root_node;
}

struct vfs_node *vfs_get_cwd(void) {
    return cwd_node;
}

void vfs_get_path(const struct vfs_node *node, char *buf, size_t buf_size) {
    if (!buf || buf_size == 0) return;
    if (!node || node == root_node) {
        strncpy(buf, "/", buf_size - 1);
        buf[buf_size - 1] = '\0';
        return;
    }

    const struct vfs_node *stack[32];
    int depth = 0;
    const struct vfs_node *cur = node;
    while (cur && cur != root_node && depth < 32) {
        stack[depth++] = cur;
        cur = cur->parent;
    }

    buf[0] = '\0';
    for (int i = depth - 1; i >= 0; i--) {
        if (strlen(buf) + 1 + strlen(stack[i]->name) >= buf_size) break;
        strcat(buf, "/");
        strcat(buf, stack[i]->name);
    }
    if (buf[0] == '\0') strcpy(buf, "/");
}

static bool split_parent(const char *path, struct vfs_node **out_parent, char *out_name) {
    if (!path || !*path) return false;

    char copy[VFS_PATH_MAX];
    strncpy(copy, path, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';

    size_t len = strlen(copy);
    while (len > 1 && copy[len - 1] == '/') {
        copy[--len] = '\0';
    }
    if (strcmp(copy, "/") == 0) return false;

    char *slash = strrchr(copy, '/');
    struct vfs_node *parent = NULL;
    const char *leaf = NULL;

    if (!slash) {
        parent = cwd_node;
        leaf = copy;
    } else if (slash == copy) {
        parent = root_node;
        leaf = copy + 1;
    } else {
        *slash = '\0';
        parent = vfs_resolve(copy);
        leaf = slash + 1;
    }

    if (!parent || parent->type != VFS_DIR) return false;
    if (!*leaf || strcmp(leaf, ".") == 0 || strcmp(leaf, "..") == 0) return false;

    strncpy(out_name, leaf, VFS_NAME_MAX - 1);
    out_name[VFS_NAME_MAX - 1] = '\0';
    *out_parent = parent;
    return true;
}

struct vfs_node *vfs_resolve(const char *path) {
    if (!path || !*path) return cwd_node;
    char copy[VFS_PATH_MAX];
    strncpy(copy, path, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';

    struct vfs_node *cur = (copy[0] == '/') ? root_node : cwd_node;
    char *p = copy;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char *start = p;
        while (*p && *p != '/') p++;
        if (*p == '/') {
            *p = '\0';
            p++;
        }
        cur = find_child(cur, start);
        if (!cur) return NULL;
    }
    return cur;
}

int vfs_chdir(const char *path) {
    struct vfs_node *n = vfs_resolve(path);
    if (!n) return -ENOENT;
    if (n->type != VFS_DIR) return -ENOTDIR;
    cwd_node = n;
    return 0;
}

struct vfs_node *vfs_mkdir(const char *path) {
    struct vfs_node *existing = vfs_resolve(path);
    if (existing) return (existing->type == VFS_DIR) ? existing : NULL;

    struct vfs_node *parent = NULL;
    char name[VFS_NAME_MAX];
    if (!split_parent(path, &parent, name)) return NULL;
    return alloc_node(name, VFS_DIR, parent);
}

struct vfs_node *vfs_touch(const char *path) {
    struct vfs_node *existing = vfs_resolve(path);
    if (existing) {
        existing->modified_tick = timer_get_ticks();
        return existing;
    }
    struct vfs_node *parent = NULL;
    char name[VFS_NAME_MAX];
    if (!split_parent(path, &parent, name)) return NULL;
    return alloc_node(name, VFS_FILE, parent);
}

int vfs_write(const char *path, const void *data, size_t size, bool append) {
    struct vfs_node *n = vfs_resolve(path);
    if (!n) {
        n = vfs_touch(path);
        if (!n) return -ENOSPC;
    }
    if (n->type != VFS_FILE) return -EISDIR;

    size_t offset = append ? n->size : 0;
    size_t needed = offset + size;

    if (needed > n->capacity) {
        size_t new_cap = ALIGN_UP(needed + 64, 64);
        uint8_t *new_buf = (uint8_t *)kmalloc(new_cap);
        if (!new_buf) return -ENOMEM;
        if (n->data && offset > 0) {
            memcpy(new_buf, n->data, offset);
        }
        if (n->data) kfree(n->data);
        n->data = new_buf;
        n->capacity = new_cap;
    }

    if (size > 0 && data) {
        memcpy(n->data + offset, data, size);
    }
    n->size = needed;
    if (n->data && n->size < n->capacity) {
        n->data[n->size] = '\0';
    }
    n->modified_tick = timer_get_ticks();
    return 0;
}

ssize_t vfs_read(const char *path, void *buf, size_t max_size) {
    struct vfs_node *n = vfs_resolve(path);
    if (!n) return -ENOENT;
    if (n->type != VFS_FILE) return -EISDIR;

    size_t to_copy = (n->size < max_size) ? n->size : max_size;
    if (to_copy > 0 && n->data && buf) {
        memcpy(buf, n->data, to_copy);
    }
    return (ssize_t)to_copy;
}

int vfs_remove(const char *path, bool recursive) {
    struct vfs_node *n = vfs_resolve(path);
    if (!n) return -ENOENT;
    if (n == root_node) return -EPERM;
    if (n == cwd_node) return -EBUSY;
    if (n->type == VFS_DIR && n->first_child != NULL && !recursive) {
        return -ENOTEMPTY;
    }
    detach_node(n);
    free_node_recursive(n);
    return 0;
}

int vfs_copy(const char *src_path, const char *dst_path) {
    struct vfs_node *src = vfs_resolve(src_path);
    if (!src) return -ENOENT;
    if (src->type != VFS_FILE) return -EISDIR;

    struct vfs_node *dst = vfs_resolve(dst_path);
    if (dst && dst->type == VFS_DIR) {
        char full[VFS_PATH_MAX];
        vfs_get_path(dst, full, sizeof(full));
        if (strcmp(full, "/") != 0) strcat(full, "/");
        strcat(full, src->name);
        return vfs_write(full, src->data, src->size, false);
    }
    return vfs_write(dst_path, src->data, src->size, false);
}

int vfs_move(const char *src_path, const char *dst_path) {
    struct vfs_node *src = vfs_resolve(src_path);
    if (!src) return -ENOENT;
    if (src == root_node || src == cwd_node) return -EBUSY;

    struct vfs_node *dst = vfs_resolve(dst_path);
    if (dst && dst->type == VFS_DIR) {
        detach_node(src);
        src->parent = dst;
        src->next_sibling = dst->first_child;
        dst->first_child = src;
        return 0;
    }

    struct vfs_node *new_parent = NULL;
    char new_name[VFS_NAME_MAX];
    if (!split_parent(dst_path, &new_parent, new_name)) return -EINVAL;

    if (dst && dst->type == VFS_FILE) {
        vfs_remove(dst_path, false);
    }
    detach_node(src);
    strncpy(src->name, new_name, VFS_NAME_MAX - 1);
    src->name[VFS_NAME_MAX - 1] = '\0';
    src->parent = new_parent;
    src->next_sibling = new_parent->first_child;
    new_parent->first_child = src;
    return 0;
}

void vfs_get_stats(struct vfs_stats *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->max_inodes = VFS_MAX_NODES;
    for (size_t i = 0; i < VFS_MAX_NODES; i++) {
        if (nodes[i].type != VFS_UNUSED) {
            out->used_inodes++;
            if (nodes[i].type == VFS_FILE) {
                out->file_count++;
                out->used_bytes += nodes[i].size;
            } else {
                out->dir_count++;
            }
        }
    }
}
