#include <stdbool.h>
#include <fs/vfs/vfs.h>
#include <lib/alloc.h>
#include <lib/hashmap.h>
#include <lib/lock.h>
#include <lib/errno.h>
#include <lib/print.h>
#include <lib/resource.h>
#include <lib/debug.h>
#include <sched/proc.h>
#include <bits/posix/stat.h>
#include <abi-bits/fcntl.h>
#include <dirent.h> // XXX this unfortunately brings in some libc function declarations, be careful
#include <limits.h>

spinlock_t vfs_lock = SPINLOCK_INIT;

struct vfs_node *vfs_create_node(struct vfs_filesystem *fs, struct vfs_node *parent,
                                 const char *name, bool dir) {
    struct vfs_node *node = ALLOC(struct vfs_node);

    node->name = alloc(strlen(name) + 1);
    strcpy(node->name, name);

    node->parent = parent;
    node->filesystem = fs;

    if (dir) {
        node->children = (typeof(node->children))HASHMAP_INIT(256);
    }

    return node;
}

void vfs_create_dotentries(struct vfs_node *node, struct vfs_node *parent) {
    struct vfs_node *dot = vfs_create_node(node->filesystem, node, ".", false);
    struct vfs_node *dotdot = vfs_create_node(node->filesystem, node, "..", false);

    dot->redir = node;
    dotdot->redir = parent;

    HASHMAP_SINSERT(&node->children, ".", dot);
    HASHMAP_SINSERT(&node->children, "..", dotdot);
}

static HASHMAP_TYPE(struct vfs_filesystem *) filesystems;

void vfs_add_filesystem(struct vfs_filesystem *fs, const char *identifier) {
    spinlock_acquire(&vfs_lock);

    HASHMAP_SINSERT(&filesystems, identifier, fs);

    spinlock_release(&vfs_lock);
}

struct vfs_node *vfs_root = NULL;

void vfs_init(void) {
    vfs_root = vfs_create_node(NULL, NULL, "", false);

    filesystems = (typeof(filesystems))HASHMAP_INIT(256);
}

struct path2node_res {
    struct vfs_node *target_parent;
    struct vfs_node *target;
    char *basename;
};

static struct vfs_node *reduce_node(struct vfs_node *node, bool follow_symlinks);

static struct path2node_res path2node(struct vfs_node *parent, const char *path) {
    if (path == NULL || strlen(path) == 0) {
        errno = ENOENT;
        return (struct path2node_res){NULL, NULL, NULL};
    }

    size_t path_len = strlen(path);

    bool ask_for_dir = path[path_len - 1] == '/';

    size_t index = 0;
    struct vfs_node *current_node = reduce_node(parent, false);

    if (path[index] == '/') {
        current_node = reduce_node(vfs_root, false);
        while (path[index] == '/') {
            if (index == path_len - 1) {
                return (struct path2node_res){current_node, current_node, strdup("/")};
            }
            index++;
        }
    }

    for (;;) {
        const char *elem = &path[index];
        size_t elem_len = 0;

        while (index < path_len && path[index] != '/') {
            elem_len++, index++;
        }

        while (index < path_len && path[index] == '/') {
            index++;
        }

        bool last = index == path_len;

        char *elem_str = alloc(elem_len + 1);
        memcpy(elem_str, elem, elem_len);

        current_node = reduce_node(current_node, false);

        struct vfs_node *new_node;

        // XXX put a lock around this guy
        // XXX page fault here (seemingly random)
        if (!HASHMAP_SGET(&current_node->children, new_node, elem_str)) {
            errno = ENOENT;
            if (last) {
                return (struct path2node_res){current_node, NULL, elem_str};
            }
            return (struct path2node_res){NULL, NULL, NULL};
        }

        new_node = reduce_node(new_node, false);

        if (last) {
            if (ask_for_dir && !S_ISDIR(new_node->resource->stat.st_mode)) {
                errno = ENOTDIR;
                return (struct path2node_res){current_node, NULL, elem_str};
            }
            return (struct path2node_res){current_node, new_node, elem_str};
        }

        current_node = new_node;

        if (S_ISLNK(current_node->resource->stat.st_mode)) {
            struct path2node_res r = path2node(current_node->parent, current_node->symlink_target);
            if (r.target == NULL) {
                return (struct path2node_res){NULL, NULL, NULL};
            }
            current_node = r.target;
        }

        if (!S_ISDIR(current_node->resource->stat.st_mode)) {
            errno = ENOTDIR;
            return (struct path2node_res){NULL, NULL, NULL};
        }
    }

    errno = ENOENT;
    return (struct path2node_res){NULL, NULL, NULL};
}

static struct vfs_node *get_parent_dir(int dir_fdnum, const char *path) {
    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;

    if (path != NULL && *path == '/') {
        return vfs_root;
    }

    if (dir_fdnum == AT_FDCWD) {
        return proc->cwd;
    }

    struct f_descriptor *fd = fd_from_fdnum(proc, dir_fdnum);
    if (fd == NULL) {
        return NULL;
    }

    struct f_description *description = fd->description;
    if (!S_ISDIR(description->res->stat.st_mode)) {
        errno = ENOTDIR;
        return NULL;
    }

    return description->node;
}

static struct vfs_node *reduce_node(struct vfs_node *node, bool follow_symlinks) {
    if (node->redir != NULL) {
        return reduce_node(node->redir, follow_symlinks);
    }
    if (node->mountpoint != NULL) {
        return reduce_node(node->mountpoint, follow_symlinks);
    }
    if (node->symlink_target != NULL && follow_symlinks == true) {
        struct path2node_res r = path2node(node->parent, node->symlink_target);
        if (r.target == NULL) {
            return NULL;
        }
        return reduce_node(r.target, follow_symlinks);
    }
    return node;
}

struct vfs_node *vfs_get_node(struct vfs_node *parent, const char *path, bool follow_links) {
    spinlock_acquire(&vfs_lock);

    struct vfs_node *ret = NULL;

    struct path2node_res r = path2node(parent, path);
    if (r.target == NULL) {
        goto cleanup;
    }

    if (follow_links) {
        ret = reduce_node(r.target, true);
        goto cleanup;
    }

    ret = r.target;

cleanup:
    if (r.basename != NULL) {
        free(r.basename);
    }
    spinlock_release(&vfs_lock);
    return ret;
}

bool vfs_mount(struct vfs_node *parent, const char *source, const char *target,
               const char *fs_name) {
    spinlock_acquire(&vfs_lock);

    bool ret = false;
    struct path2node_res r = {0};

    struct vfs_filesystem *fs;
    if (!HASHMAP_SGET(&filesystems, fs, fs_name)) {
        errno = ENODEV;
        goto cleanup;
    }

    struct vfs_node *source_node = NULL;
    if (source != NULL && strlen(source) != 0) {
        struct path2node_res rr = path2node(parent, source);
        source_node = rr.target;
        if (rr.basename != NULL) {
            free(rr.basename);
        }
        if (source_node == NULL) {

            goto cleanup;
        }
        if (S_ISDIR(source_node->resource->stat.st_mode)) {
            errno = EISDIR;
            goto cleanup;
        }
    }

    r = path2node(parent, target);

    bool mounting_root = r.target == vfs_root;

    if (r.target == NULL) {
        goto cleanup;
    }

    if (!mounting_root && !S_ISDIR(r.target->resource->stat.st_mode)) {
        errno = EISDIR;
        goto cleanup;
    }

    struct vfs_node *mount_node = fs->mount(r.target_parent, r.basename, source_node);
    r.target->mountpoint = mount_node;

    vfs_create_dotentries(mount_node, r.target_parent);

    if (source != NULL && strlen(source) != 0) {
        kernel_print("vfs: Mounted `%s` on `%s` with filesystem `%s`\n", source, target, fs_name);
    } else {
        kernel_print("vfs: Mounted %s on `%s`\n", fs_name, target);
    }

    ret = true;

cleanup:
    if (r.basename != NULL) {
        free(r.basename);
    }
    spinlock_release(&vfs_lock);
    return ret;
}

struct vfs_node *vfs_symlink(struct vfs_node *parent, const char *dest,
                             const char *target) {
    spinlock_acquire(&vfs_lock);

    struct vfs_node *ret = NULL;

    struct path2node_res r = path2node(parent, target);

    if (r.target_parent == NULL) {
        goto cleanup;
    }

    if (r.target != NULL) {
        errno = EEXIST;
        goto cleanup;
    }

    struct vfs_filesystem *target_fs = r.target_parent->filesystem;
    struct vfs_node *target_node = target_fs->symlink(target_fs, r.target_parent, r.basename, dest);

    HASHMAP_SINSERT(&r.target_parent->children, r.basename, target_node);

    ret = target_node;

cleanup:
    if (r.basename != NULL) {
        free(r.basename);
    }
    spinlock_release(&vfs_lock);
    return ret;
}

bool vfs_unlink(struct vfs_node *parent, const char *path) {
    bool ret = false;

    spinlock_acquire(&vfs_lock);

    struct path2node_res r = path2node(parent, path);

    if (r.target_parent == NULL) {
        goto cleanup;
    }

    if (r.target == NULL) {
        goto cleanup;
    }

    if (r.target->mountpoint != NULL) {
        errno = EBUSY;
        goto cleanup;
    }

    if (!HASHMAP_SREMOVE(&r.target_parent->children, r.basename)) {
        goto cleanup;
    }

    if (!r.target->resource->unref(r.target->resource, &((struct f_description) { .node = r.target, .res = r.target->resource }))) {
        goto cleanup;
    }

    free(r.target->name);
    if (r.target->symlink_target != NULL) {
        free(r.target->symlink_target);
    }

    if (S_ISDIR(r.target->resource->stat.st_mode)) {
        HASHMAP_DELETE(&r.target->children);
    }

    ret = true;

cleanup:
    if (r.basename != NULL) {
        free(r.basename);
    }
    spinlock_release(&vfs_lock);
    return ret;
}

struct vfs_node *vfs_create(struct vfs_node *parent, const char *name, int mode) {
    spinlock_acquire(&vfs_lock);

    struct vfs_node *ret = NULL;

    struct path2node_res r = path2node(parent, name);

    if (r.target_parent == NULL) {
        goto cleanup;
    }

    if (r.target != NULL) {
        errno = EEXIST;
        goto cleanup;
    }

    struct vfs_filesystem *target_fs = r.target_parent->filesystem;
    struct vfs_node *target_node = target_fs->create(target_fs, r.target_parent, r.basename, mode);

    HASHMAP_SINSERT(&r.target_parent->children, r.basename, target_node);

    if (S_ISDIR(target_node->resource->stat.st_mode)) {
        vfs_create_dotentries(target_node, r.target_parent);
    }

    ret = target_node;

cleanup:
    if (r.basename != NULL) {
        free(r.basename);
    }
    spinlock_release(&vfs_lock);
    return ret;
}

size_t vfs_pathname(struct vfs_node *node, char *buffer, size_t len) {
    size_t offset = 0;
    if (node->parent != vfs_root && node->parent != NULL) {
        struct vfs_node *parent = reduce_node(node->parent, false);

        if (parent != vfs_root && parent != NULL) {
            offset += vfs_pathname(parent, buffer, len - offset - 1);
            buffer[offset++] = '/';
        }
    }

    if (strcmp(node->name, "/") != 0) {
        strncpy(buffer + offset, node->name, len - offset);
        return strlen(node->name) + offset;
    } else {
        return offset;
    }
}

bool vfs_fdnum_path_to_node(int dir_fdnum, const char *path, bool empty_path, bool enoent_error,
                            struct vfs_node **parent, struct vfs_node **node, char **basename) {
    if (!empty_path && (path == NULL || strlen(path) == 0)) {
        errno = ENOENT;
        return false;
    }

    struct vfs_node *parent_node = get_parent_dir(dir_fdnum, path);
    if (parent == NULL) {
        return false;
    }

    struct path2node_res res = path2node(parent_node, path);
    if (res.target == NULL && (errno == ENOENT && enoent_error)) {
        return false;
    }

    if (parent != NULL) {
        *parent = res.target_parent;
    }

    if (node != NULL) {
        *node = res.target;
    }

    if (basename != NULL) {
        *basename = res.basename;
    } else {
        if (res.basename != NULL) {
            free(res.basename);
        }
    }

    return true;
}

int syscall_openat(void *_, int dir_fdnum, const char *path, int flags, int mode) {
    (void)_;

    DEBUG_SYSCALL_ENTER("openat(%d, %s, %x, %o)", dir_fdnum, path, flags, mode);

    int ret = -1;

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;

    struct vfs_node *parent = NULL;
    char *basename = NULL;
    if (!vfs_fdnum_path_to_node(dir_fdnum, path, false, false, &parent, NULL, &basename)) {
        goto cleanup;
    }

    if (parent == NULL) {
        errno = ENOENT;
        goto cleanup;
    }

    int create_flags = flags & FILE_CREATION_FLAGS_MASK;
    int follow_links = (flags & O_NOFOLLOW) == 0;

    struct vfs_node *node = vfs_get_node(parent, basename, follow_links);
    if (node == NULL) {
        if ((create_flags & O_CREAT) != 0) {
            node = vfs_create(parent, basename, (mode & ~proc->umask) | S_IFREG);
        } else {
            errno = ENOENT;
            goto cleanup;
        }
    }

    if (node == NULL) {
        goto cleanup;
    }

    if (S_ISLNK(node->resource->stat.st_mode)) {
        errno = ELOOP;
        goto cleanup;
    }

    node = reduce_node(node, true);
    if (node == NULL) {
        goto cleanup;
    }

    if (!S_ISDIR(node->resource->stat.st_mode) && (flags & O_DIRECTORY) != 0) {
        errno = ENOTDIR;
        goto cleanup;
    }

    if (!S_ISREG(node->resource->stat.st_mode) && (flags & O_TRUNC) != 0) {
        errno = EINVAL;
        goto cleanup;
    }

    struct f_descriptor *fd = fd_create_from_resource(node->resource, flags);
    if (fd == NULL) {
        goto cleanup;
    }

    if ((flags & O_TRUNC) != 0) {
        node->resource->truncate(node->resource, fd->description, 0);
    }

    fd->description->node = node;
    ret = fdnum_create_from_fd(proc, fd, 0, false);

cleanup:
    if (basename != NULL) {
        free(basename);
    }

    DEBUG_SYSCALL_LEAVE("%d", ret);
    return ret;
}

// XXX convert to use vfs_fdnum_path_to_node
int syscall_stat(void *_, int dir_fdnum, const char *path, int flags, struct stat *stat_buf) {
    (void)_;

    DEBUG_SYSCALL_ENTER("stat(%d, %s, %x, %lx)", dir_fdnum, path, flags, stat_buf);

    int ret = -1;

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;
    struct stat *stat_src = NULL;

    if (stat_buf == NULL) {
        errno = EINVAL;
        goto cleanup;
    }

    if (strlen(path) == 0) {
        if ((flags & AT_EMPTY_PATH) == 0) {
            errno = ENOENT;
            goto cleanup;
        }

        if (dir_fdnum == AT_FDCWD) {
            stat_src = &proc->cwd->resource->stat;
        } else {
            struct f_descriptor *fd = fd_from_fdnum(proc, dir_fdnum);
            if (fd == NULL) {
                goto cleanup;
            }

            stat_src = &fd->description->res->stat;
        }
    } else {
        struct vfs_node *parent = get_parent_dir(dir_fdnum, path);
        if (parent == NULL) {
            goto cleanup;
        }

        struct vfs_node *node = vfs_get_node(parent, path, (flags & AT_SYMLINK_NOFOLLOW) == 0);
        if (node == NULL) {
            goto cleanup;
        }

        stat_src = &node->resource->stat;
    }

    *stat_buf = *stat_src;
    ret = 0;

cleanup:
    DEBUG_SYSCALL_LEAVE("%d", ret);
    return ret;
}

int syscall_getcwd(void *_, char *buffer, size_t len) {
    (void)_;

    DEBUG_SYSCALL_ENTER("getcwd(%lx, %lu)", buffer, len);

    int ret = -1;

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;

    char path_buffer[PATH_MAX] = {0};
    if (vfs_pathname(proc->cwd, path_buffer, PATH_MAX) >= len) {
        errno = ERANGE;
        goto cleanup;
    }

    strncpy(buffer, path_buffer, len);
    ret = 0;

cleanup:
    DEBUG_SYSCALL_LEAVE("%d", ret);
    return ret;
}

int syscall_chdir(void *_, const char *path) {
    (void)_;

    DEBUG_SYSCALL_ENTER("chdir(%s)", path);

    int ret = -1;

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;

    if (path == NULL) {
        errno = EINVAL;
        goto cleanup;
    }

    if (strlen(path) == 0) {
        errno = ENOENT;
        goto cleanup;
    }

    struct vfs_node *node = vfs_get_node(proc->cwd, path, true);
    if (node == NULL) {
        errno = ENOENT;
        goto cleanup;
    }

    if (!S_ISDIR(node->resource->stat.st_mode)) {
        errno = ENOTDIR;
        goto cleanup;
    }

    proc->cwd = node;
    ret = 0;

cleanup:
    DEBUG_SYSCALL_LEAVE("%d", ret);
    return ret;
}

int syscall_readdir(void *_, int dir_fdnum, void *buffer, size_t *size) {
    (void)_;

    DEBUG_SYSCALL_ENTER("readdir(%d, %lx, %lu)", dir_fdnum, buffer, *size);

    int ret = -1;

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;

    spinlock_acquire(&vfs_lock);

    struct f_descriptor *dir_fd = fd_from_fdnum(proc, dir_fdnum);
    if (dir_fd == NULL) {
        errno = EBADF;
        goto cleanup;
    }

    struct vfs_node *dir_node = dir_fd->description->node;
    if (!S_ISDIR(dir_fd->description->res->stat.st_mode)) {
        errno = ENOTDIR;
        goto cleanup;
    }

    size_t entries_length = 0;
    if (dir_node->children.buckets != NULL) {
        for (size_t i = 0; i < dir_node->children.cap; i++) {
            __auto_type bucket = &dir_node->children.buckets[i];

            for (size_t j = 0; j < bucket->filled; j++) {
                struct vfs_node *child = bucket->items[j].item;
                entries_length += sizeof(struct dirent) - 1024 + strlen(child->name) + 1;
            }
        }
    }

    // We need space for a null entry that marks the end of directory
    entries_length += sizeof(struct dirent) - 1024;

    if (entries_length > *size) {
        *size = entries_length;
        errno = ENOBUFS;
        goto cleanup;
    }

    size_t offset = 0;
    if (dir_node->children.buckets != NULL) {
        for (size_t i = 0; i < dir_node->children.cap; i++) {
            __auto_type bucket = &dir_node->children.buckets[i];

            for (size_t j = 0; j < bucket->filled; j++) {
                struct vfs_node *child = bucket->items[j].item;
                struct vfs_node *reduced = reduce_node(child, false);
                struct dirent *ent = buffer + offset;

                ent->d_ino = reduced->resource->stat.st_ino;
                ent->d_reclen = sizeof(struct dirent) - 1024 + strlen(child->name) + 1;
                ent->d_off = 0;

                switch (reduced->resource->stat.st_mode & S_IFMT) {
                    case S_IFBLK:
                        ent->d_type = DT_BLK;
                        break;
                    case S_IFCHR:
                        ent->d_type = DT_CHR;
                        break;
                    case S_IFIFO:
                        ent->d_type = DT_FIFO;
                        break;
                    case S_IFREG:
                        ent->d_type = DT_REG;
                        break;
                    case S_IFDIR:
                        ent->d_type = DT_DIR;
                        break;
                    case S_IFLNK:
                        ent->d_type = DT_LNK;
                        break;
                    case S_IFSOCK:
                        ent->d_type = DT_SOCK;
                        break;
                }

                memcpy(ent->d_name, child->name, strlen(child->name) + 1);
                offset += ent->d_reclen;
            }
        }
    }

    struct dirent *terminator = buffer + offset;
    terminator->d_reclen = 0;
    ret = 0;

cleanup:
    spinlock_release(&vfs_lock);

    DEBUG_SYSCALL_LEAVE("%d", ret);
    return ret;
}

ssize_t syscall_readlinkat(void *_, int dir_fdnum, const char *path, char *buffer, size_t limit) {
    (void)_;

    DEBUG_SYSCALL_ENTER("readlink(%s, %lx, %lu)", path, buffer, limit);

    ssize_t ret = -1;

    struct vfs_node *parent = NULL;
    char *basename = NULL;
    if (!vfs_fdnum_path_to_node(dir_fdnum, path, false, false, &parent, NULL, &basename)) {
        goto cleanup;
    }

    if (parent == NULL) {
        errno = ENOENT;
        goto cleanup;
    }

    struct vfs_node *node = vfs_get_node(parent, basename, false);
    if (node == NULL) {
        goto cleanup;
    }

    if (!S_ISLNK(node->resource->stat.st_mode)) {
        errno = EINVAL;
        goto cleanup;
    }

    size_t to_copy = strlen(node->symlink_target) + 1;
    if (to_copy > limit) {
        to_copy = limit;
    }

    memcpy(buffer, node->symlink_target, to_copy);

    ret = to_copy;

cleanup:
    if (basename != NULL) {
        free(basename);
    }

    DEBUG_SYSCALL_LEAVE("%lld", ret);
    return ret;
}

// XXX convert to vfs_fdnum_path_to_node
int syscall_linkat(void *_, int olddir_fdnum, const char *old_path, int newdir_fdnum, const char *new_path, int flags) {
    (void)_;

    DEBUG_SYSCALL_ENTER("linkat(%d, %s, %d, %s, %x)", olddir_fdnum, old_path, newdir_fdnum, new_path, flags);

    int ret = -1;

    if (old_path == NULL || strlen(old_path) == 0) {
        errno = ENOENT;
        goto cleanup;
    }

    struct vfs_node *old_parent = get_parent_dir(olddir_fdnum, old_path);
    if (old_parent == NULL) {
        goto cleanup;
    }

    struct vfs_node *new_parent = get_parent_dir(newdir_fdnum, new_path);
    if (new_parent == NULL) {
        goto cleanup;
    }

    struct path2node_res old_res = path2node(old_parent, old_path);
    struct path2node_res new_res = path2node(new_parent, new_path);
    if (old_res.target_parent->filesystem != new_res.target_parent->filesystem) {
        errno = EXDEV;
        goto cleanup;
    }

    struct vfs_node *old_node = vfs_get_node(old_parent, old_path, (flags & AT_SYMLINK_NOFOLLOW) == 0);
    if (old_node == NULL) {
        goto cleanup;
    }

    struct vfs_filesystem *fs = new_res.target_parent->filesystem;
    struct vfs_node *node = fs->link(fs, new_res.target_parent, new_res.basename, old_node);
    if (node == NULL) {
        goto cleanup;
    }

    HASHMAP_SINSERT(&new_res.target_parent->children, new_res.basename, node);
    ret = 0;

cleanup:
    DEBUG_SYSCALL_LEAVE("%d", ret);
    return ret;
}

int syscall_unlinkat(void *_, int dir_fdnum, const char *path, int flags) {
    (void)_;

    DEBUG_SYSCALL_ENTER("unlinkat(%d, %s, %x)", dir_fdnum, path, flags);

    int ret = -1;

    struct vfs_node *parent = NULL, *node = NULL;
    char *basename = NULL;
    if (!vfs_fdnum_path_to_node(dir_fdnum, path, false, true, &parent, &node, &basename)) {
        goto cleanup;
    }

    if (S_ISDIR(node->resource->stat.st_mode) && (flags & AT_REMOVEDIR) == 0) {
        errno = EISDIR;
        goto cleanup;
    }

    vfs_unlink(parent, basename);
    ret = 0;

cleanup:
    if (basename != NULL) {
        free(basename);
    }

    DEBUG_SYSCALL_LEAVE("%d", ret);
    return ret;
}

int syscall_mkdirat(void *_, int dir_fdnum, const char *path, mode_t mode){
    (void)_;

    DEBUG_SYSCALL_ENTER("mkdirat(%d, %s, %04o)", dir_fdnum, path, mode);

    int ret = -1;

    char *basename = NULL;
    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;

    if (path == NULL || strlen(path) == 0) {
        errno = ENOENT;
        goto cleanup;
    }

    struct vfs_node *parent = NULL;
    if (!vfs_fdnum_path_to_node(dir_fdnum, path, false, false, &parent, NULL, &basename)) {
        goto cleanup;
    }

    if (parent == NULL) {
        errno = ENOENT;
        goto cleanup;
    }

    struct vfs_node *node = vfs_create(parent, basename, (mode & ~proc->umask) | S_IFDIR);
    if (node == NULL) {
        goto cleanup;
    }

    ret = 0;

cleanup:
    if (basename != NULL) {
        free(basename);
    }

    DEBUG_SYSCALL_LEAVE("%d", ret);
    return ret;
}
