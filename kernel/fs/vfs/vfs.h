#ifndef _FS__VFS__VFS_H
#define _FS__VFS__VFS_H

#include <stdbool.h>
#include <lib/resource.h>
#include <lib/hashmap.h>
#include <lib/lock.h>

extern spinlock_t vfs_lock;

struct vfs_filesystem;

struct vfs_node {
    struct vfs_node *mountpoint;
    struct vfs_node *redir;
    struct resource *resource;
    struct vfs_filesystem *filesystem;
    char *name;
    struct vfs_node *parent;
    HASHMAP_TYPE(struct vfs_node *) children;
    char *symlink_target;
    bool populated;
};

struct vfs_filesystem {
    void             (*populate)(struct vfs_filesystem *, struct vfs_node *);
    struct vfs_node *(*mount)(struct vfs_node *, const char *, struct vfs_node *);
    struct vfs_node *(*create)(struct vfs_filesystem *, struct vfs_node *, const char *, int);
    struct vfs_node *(*symlink)(struct vfs_filesystem *, struct vfs_node *, const char *, const char *);
    struct vfs_node *(*link)(struct vfs_filesystem *, struct vfs_node *, const char *, struct vfs_node *);
};

extern struct vfs_node *vfs_root;

void vfs_init(void);
struct vfs_node *vfs_create_node(struct vfs_filesystem *fs, struct vfs_node *parent,
                                 const char *name, bool dir);
void vfs_create_dotentries(struct vfs_node *node, struct vfs_node *parent);
void vfs_add_filesystem(struct vfs_filesystem *fs, const char *identifier);
struct vfs_node *vfs_get_node(struct vfs_node *parent, const char *path, bool follow_links);
bool vfs_mount(struct vfs_node *parent, const char *source, const char *target,
               const char *fs_name);
struct vfs_node *vfs_symlink(struct vfs_node *parent, const char *dest,
                             const char *target);
struct vfs_node *vfs_create(struct vfs_node *parent, const char *name, int mode);
bool vfs_unlink(struct vfs_node *parent, const char *path);
size_t vfs_pathname(struct vfs_node *node, char *buffer, size_t len);
bool vfs_fdnum_path_to_node(int dir_fdnum, const char *path, bool empty_path, bool enoent_error,
                            struct vfs_node **parent, struct vfs_node **node, char **basename);

#endif
