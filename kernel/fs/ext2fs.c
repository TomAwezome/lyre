#include <bits/posix/stat.h>
#include <fs/ext2fs.h>
#include <fs/vfs/vfs.h>
#include <lib/bitmap.h>
#include <lib/errno.h>
#include <lib/print.h>
#include <lib/resource.h>
#include <lib/vector.h>
#include <mm/mmap.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <time/time.h>

// TODO: Future improvement may be to implement 64-bit sizes in place of seemingly varying sizes

struct ext2fs_superblock {
    uint32_t inodecnt;
    uint32_t blockcnt;
    uint32_t sbrsvd;
    uint32_t unallocb;
    uint32_t unalloci;
    uint32_t sb;
    uint32_t blksize;
    uint32_t fragsize;
    uint32_t blockspergroup;
    uint32_t fragspergroup;
    uint32_t inodespergroup;
    uint32_t lastmnt; // unix epoch for last mount
    uint32_t lastwritten; // unix epoch for last write
    uint16_t mountcnt;
    uint16_t mountallowed; // are we allowed to mount this filesystem?
    uint16_t sig;
    uint16_t fsstate;
    uint16_t errorresp;
    uint16_t vermin;
    uint32_t lastfsck; // last time we cleaned the filesystem
    uint32_t forcedfsck;
    uint32_t osid;
    uint32_t vermaj;
    uint16_t uid;
    uint16_t gid;

    uint32_t first;
    uint16_t inodesize;
    uint16_t sbbgd;
    uint32_t optionalfts;
    uint32_t reqfts;
    uint64_t uuid[2]; // filesystem uuid
    uint64_t name[2];
    uint64_t lastmountedpath[8]; // last path we had when mounted
} __attribute__((packed));

struct ext2fs_blockgroupdesc {
    uint32_t addrblockbmp;
    uint32_t addrinodebmp;
    uint32_t inodetable;
    uint16_t unallocb;
    uint16_t unalloci;
    uint16_t dircnt;
    uint16_t unused[7];
} __attribute__((packed));

struct ext2fs_inode {
    uint16_t perms;
    uint16_t uid;
    uint32_t sizelo;
    uint32_t accesstime;
    uint32_t creationtime;
    uint32_t modifiedtime;
    uint32_t deletedtime;
    uint16_t gid;
    uint16_t hardlinkcnt;
    uint32_t sectors;
    uint32_t flags;
    uint32_t oss;
    uint32_t blocks[15];
    uint32_t gennum;
    uint32_t eab;
    uint32_t sizehi;
    uint32_t fragaddr;
} __attribute__((packed));

struct ext2fs_direntry {
    uint32_t inodeidx;
    uint16_t entsize;
    uint8_t namelen;
    uint8_t dirtype;
} __attribute__((packed));

struct ext2fs {
    struct vfs_filesystem;

    uint64_t devid;

    struct vfs_node *backing; // block device this filesystem exists on
    struct ext2fs_inode root;
    struct ext2fs_superblock sb;

    size_t blksize;
    size_t fragsize;
    size_t bgdcnt;
};

struct ext2fs_resource {
    struct resource;

    struct ext2fs *fs;
};

static ssize_t ext2fs_inoderead(struct ext2fs_inode *inode, struct ext2fs *fs, void *buf, off_t off, size_t count);
static ssize_t ext2fs_inodereadentry(struct ext2fs_inode *inode, struct ext2fs *fs, uint32_t inodeidx);
static ssize_t ext2fs_inodewriteentry(struct ext2fs_inode *inode, struct ext2fs *fs, uint32_t inodeidx);
static uint32_t ext2fs_inodegetblock(struct ext2fs_inode *inode, struct ext2fs *fs, uint32_t iblock);
static ssize_t ext2fs_bgdreadentry(struct ext2fs_blockgroupdesc *bgd, struct ext2fs *fs, uint32_t idx);
static ssize_t ext2fs_bgdwriteentry(struct ext2fs_blockgroupdesc *bgd, struct ext2fs *fs, uint32_t idx);

static void ext2fs_writesuperblock(struct ext2fs *fs) {
    fs->backing->resource->write(fs->backing->resource, NULL, &fs->sb, fs->backing->resource->stat.st_blksize * 2, sizeof(struct ext2fs_superblock));
}

static size_t ext2fs_allocblock(struct ext2fs *fs) {
    for (size_t i = 0; i < fs->sb.blockcnt; i++) {
        struct ext2fs_blockgroupdesc bgd = { 0 };
        ext2fs_bgdreadentry(&bgd, fs, i);

        if (bgd.unallocb <= 0) {
            continue;
        }

        uint8_t *bitmap = alloc(fs->blksize);

        fs->backing->resource->read(fs->backing->resource, NULL, bitmap, bgd.addrblockbmp * fs->blksize, fs->blksize);

        size_t block = 0;

        for (size_t j = 0; j < fs->blksize; j++) {
            if (bitmap[j] == 0xff) {
                continue; // skip for efficiency
            }

            for (size_t bit = 0; bit < 8; bit++) {
                if (bitmap_test(bitmap, bit + (j * 8)) == false) { // free bitmap
                    bitmap_set(bitmap, bit + (j * 8));
                    block = (i * fs->sb.blockspergroup) + j * 8 + bit;
                    break;
                }
            }

            if (block) {
                break;
            }
        }

        if (!block) {
            continue;
        }

        fs->backing->resource->write(fs->backing->resource, NULL, bitmap, bgd.addrblockbmp * fs->blksize, fs->blksize); // write new bitmap

        fs->sb.unallocb--;
        bgd.unallocb--;

        ext2fs_bgdwriteentry(&bgd, fs, i);
        ext2fs_writesuperblock(fs);

        free(bitmap);
        return block;
    }

    return 0;
}

static void ext2fs_freeblock(struct ext2fs *fs, size_t blockidx) {
    size_t bgdidx = blockidx / fs->sb.blockspergroup;
    struct ext2fs_blockgroupdesc bgd = { 0 };
    ext2fs_bgdreadentry(&bgd, fs, bgdidx);

    uint8_t *bitmap = alloc(fs->blksize);

    fs->backing->resource->read(fs->backing->resource, NULL, bitmap, bgd.addrblockbmp * fs->blksize, fs->blksize);

    bitmap_reset(bitmap, blockidx % fs->sb.blockspergroup); // clear bit

    fs->backing->resource->write(fs->backing->resource, NULL, bitmap, bgd.addrblockbmp * fs->blksize, fs->blksize);

    bgd.unallocb++; // new block!
    fs->sb.unallocb++; // ditto

    ext2fs_bgdwriteentry(&bgd, fs, bgdidx);
    ext2fs_writesuperblock(fs);

    free(bitmap);
}

static void ext2fs_freeblocklist(struct ext2fs *fs, size_t blockidx, size_t indirs) {
    if (blockidx == 0) {
        return; // skip any operations if this list is not allocated whatsoever
    }

    uint32_t *buf = alloc(fs->blksize);
    size_t entriesablock = fs->blksize / sizeof(uint32_t);

    fs->backing->resource->read(fs->backing->resource, NULL, buf, blockidx * fs->blksize, fs->blksize);

    for (size_t i = 0; i < entriesablock; i++) {
        if (buf[i] == 0) {
            continue;
        }

        if (indirs > 0) {
            ext2fs_freeblocklist(fs, buf[i], indirs - 1); // recursive decent
        } else {
            ext2fs_freeblock(fs, buf[i]);
        }
    }

    ext2fs_freeblock(fs, blockidx); // release top-most block
    free(buf);
}

static size_t ext2fs_allocinode(struct ext2fs *fs) {
    for (size_t i = 0; i < fs->sb.blockcnt; i++) {
        struct ext2fs_blockgroupdesc bgd = { 0 };
        ext2fs_bgdreadentry(&bgd, fs, i);

        if (bgd.unalloci <= 0) {
            continue;
        }

        uint8_t *bitmap = alloc(fs->blksize);

        fs->backing->resource->read(fs->backing->resource, NULL, bitmap, bgd.addrinodebmp * fs->blksize, fs->blksize);

        size_t inode = 0;

        for (size_t j = 0; j < fs->blksize; j++) {
            if (bitmap[j] == 0xff) {
                continue; // skip for efficiency
            }

            for (size_t bit = 0; bit < 8; bit++) {
                if (bitmap_test(bitmap, bit + (j * 8)) == false) {
                    inode = (i * fs->sb.inodespergroup) + j * 8 + bit + 1; // offset by one as they are 1 initialised
                    if ((inode > fs->sb.first) && inode > 11) {
                        bitmap_set(bitmap, bit + (j * 8));
                        break;
                    }
                }
            }

            if (inode) {
                break;
            }
        }

        if (!inode) {
            continue;
        }

        fs->backing->resource->write(fs->backing->resource, NULL, bitmap, bgd.addrinodebmp * fs->blksize, fs->blksize);

        bgd.unalloci--;
        fs->sb.unalloci--;
        ext2fs_bgdwriteentry(&bgd, fs, i);
        ext2fs_writesuperblock(fs);

        return inode;
    }

    return 0;
}

static void ext2fs_freeinode(struct ext2fs *fs, size_t inodeidx) {
    inodeidx--;

    // everything from here is the same as freeblock
    size_t bgdidx = inodeidx / fs->sb.inodespergroup;
    struct ext2fs_blockgroupdesc bgd = { 0 };
    ext2fs_bgdreadentry(&bgd, fs, bgdidx);

    uint8_t *bitmap = alloc(fs->blksize);

    fs->backing->resource->read(fs->backing->resource, NULL, bitmap, bgd.addrinodebmp * fs->blksize, fs->blksize);

    bitmap_reset(bitmap, inodeidx % fs->sb.inodespergroup); // clear bit

    fs->backing->resource->write(fs->backing->resource, NULL, bitmap, bgd.addrinodebmp * fs->blksize, fs->blksize);

    bgd.unalloci++; // new inode!
    fs->sb.unalloci++; // ditto

    ext2fs_bgdwriteentry(&bgd, fs, bgdidx);
    ext2fs_writesuperblock(fs);

    free(bitmap);
}

static uint32_t ext2fs_inodegetblock(struct ext2fs_inode *inode, struct ext2fs *fs, uint32_t iblock) {
    uint32_t blockidx = 0;
    uint32_t blocklvl = fs->blksize / 4;

    if (iblock < 12) {
        blockidx = inode->blocks[iblock];
        return blockidx;
    }

    iblock -= 12;

    if (iblock >= blocklvl) {
        iblock -= blocklvl;

        uint32_t singleidx = iblock / blocklvl;
        off_t indirectoff = iblock % blocklvl;
        uint32_t indirectblock = 0;

        if (singleidx >= blocklvl) {
            iblock -= blocklvl * blocklvl; // square

            uint32_t doubleindirect = iblock / blocklvl;
            indirectoff = iblock % blocklvl;
            uint32_t singleindirectidx = 0;
            fs->backing->resource->read(fs->backing->resource, NULL, &singleindirectidx, inode->blocks[14] * fs->blksize + doubleindirect * 4, sizeof(uint32_t));
            fs->backing->resource->read(fs->backing->resource, NULL, &indirectblock, doubleindirect * fs->blksize + singleindirectidx * 4, sizeof(uint32_t));
            fs->backing->resource->read(fs->backing->resource, NULL, &blockidx, indirectblock * fs->blksize + indirectoff * 4, sizeof(uint32_t));

            return blockidx;
        }

        fs->backing->resource->read(fs->backing->resource, NULL, &indirectblock, inode->blocks[13] * fs->blksize + singleidx * 4, sizeof(uint32_t));
        fs->backing->resource->read(fs->backing->resource, NULL, &blockidx, indirectblock * fs->blksize + indirectoff * 4, sizeof(uint32_t));

        return blockidx;
    }

    fs->backing->resource->read(fs->backing->resource, NULL, &blockidx, inode->blocks[12] * fs->blksize + iblock * 4, sizeof(uint32_t));

    return blockidx;
}

static ssize_t ext2fs_inodesetblock(struct ext2fs_inode *inode, struct ext2fs *fs, uint32_t inodeidx, uint32_t iblock, uint32_t dblock) {
    uint32_t blocklvl = fs->blksize / 4;

    if (iblock < 12) { // direct
        inode->blocks[iblock] = dblock;
        return dblock;
    }

    iblock -= 12; // next

    if (iblock >= blocklvl) { // collapse down until we write the actual block
        iblock -= blocklvl;

        uint32_t singleidx = iblock / blocklvl;
        off_t indirectoff = iblock % blocklvl;
        uint32_t indirectblock = 0;

        if (singleidx >= blocklvl) {
            iblock -= blocklvl * blocklvl; // squared

            uint32_t doubleindirect = iblock / blocklvl;
            indirectoff = iblock % blocklvl;
            uint32_t singleindirectidx = 0;

            if (inode->blocks[14] == 0) {
                inode->blocks[14] = ext2fs_allocblock(fs);
                ext2fs_inodewriteentry(inode, fs, inodeidx);
            }

            fs->backing->resource->read(fs->backing->resource, NULL, &singleindirectidx, inode->blocks[14] * fs->blksize + doubleindirect * 4, sizeof(uint32_t)); // get our index

            if (singleindirectidx == 0) {
                singleindirectidx = ext2fs_allocblock(fs);

                fs->backing->resource->write(fs->backing->resource, NULL, &singleindirectidx, inode->blocks[14] * fs->blksize + doubleindirect * 4, sizeof(uint32_t)); // write new index
            }

            fs->backing->resource->read(fs->backing->resource, NULL, &indirectblock, doubleindirect * fs->blksize + singleindirectidx * 4, sizeof(uint32_t));

            if (indirectblock == 0) {
                uint32_t new = ext2fs_allocblock(fs);

                fs->backing->resource->write(fs->backing->resource, NULL, &indirectblock, doubleindirect * fs->blksize + singleindirectidx * 4, sizeof(uint32_t));

                indirectblock = new;
            }

            fs->backing->resource->write(fs->backing->resource, NULL, &dblock, indirectblock * fs->blksize * indirectoff * 4, sizeof(uint32_t));

            return dblock;
        }

        if (inode->blocks[13] == 0) {
            inode->blocks[13] = ext2fs_allocblock(fs);

            ext2fs_inodewriteentry(inode, fs, inodeidx);
        }

        fs->backing->resource->read(fs->backing->resource, NULL, &indirectblock, inode->blocks[13] * fs->blksize + singleidx * 4, sizeof(uint32_t));

        if (indirectblock == 0) {
            indirectblock = ext2fs_allocblock(fs);

            fs->backing->resource->write(fs->backing->resource, NULL, &indirectblock, inode->blocks[13] * fs->blksize + singleidx * 4, sizeof(uint32_t));
        }

        fs->backing->resource->write(fs->backing->resource, NULL, &dblock, indirectblock * fs->blksize + indirectoff * 4, sizeof(uint32_t));

        return dblock;
    } else {
        if (inode->blocks[12] == 0) {
            inode->blocks[12] = ext2fs_allocblock(fs);

            ext2fs_inodewriteentry(inode, fs, inodeidx);
        }

        fs->backing->resource->write(fs->backing->resource, NULL, &dblock, inode->blocks[12] * fs->blksize + iblock * 4, sizeof(uint32_t)); // last resort to write
    }

    return dblock;
}

static ssize_t ext2fs_inodeensurelen(struct ext2fs_inode *inode, struct ext2fs *fs, uint32_t inodeidx, size_t start, size_t count) {
    size_t iblockstart = DIV_ROUNDUP(inode->sectors * fs->backing->resource->stat.st_blksize, fs->blksize);
    size_t iblockend = DIV_ROUNDUP(start + count, fs->blksize);

    if (inode->sizelo < (start + count)) {
        inode->sizelo = start + count;
    } else {
        return 0;
    }

    for (size_t i = iblockstart; i < iblockend; i++) {
        size_t dblock = ext2fs_allocblock(fs);

        inode->sectors = fs->blksize / fs->backing->resource->stat.st_blksize;

        ext2fs_inodesetblock(inode, fs, inodeidx, i, dblock);
    }

    ext2fs_inodewriteentry(inode, fs, inodeidx);

    return 0;
}

static ssize_t ext2fs_inoderead(struct ext2fs_inode *inode, struct ext2fs *fs, void *buf, off_t off, size_t count) {
    if (off > inode->sizelo) {
        return 0;
    }

    if ((off + count) > inode->sizelo) {
        count = inode->sizelo - off;
    }

    for (size_t head = 0; head < count;) { // force reads to be block-wise
        size_t iblock = (off + head) / fs->blksize;

        size_t size = count - head;
        off = (off + head) % fs->blksize;

        if (size > (fs->blksize - off)) {
            size = fs->blksize - off;
        }

        uint32_t block = ext2fs_inodegetblock(inode, fs, (uint32_t)iblock);
        if (fs->backing->resource->read(fs->backing->resource, NULL, (void *)((uint64_t)buf + head), block * fs->blksize + off, size) == -1) {
            return -1;
        }

        head += size;
    }

    return count;
}

static ssize_t ext2fs_inodewrite(struct ext2fs_inode *inode, struct ext2fs *fs, const void *buf, uint32_t inodeidx, off_t off, size_t count) {
    ext2fs_inodeensurelen(inode, fs, inodeidx, off, count); // ensure inode is the right size

    for (size_t head = 0; head < count;) { // force writes to be block-wise
        size_t iblock = (off + head) / fs->blksize;

        size_t size = count - head;
        off_t suboff = (off + head) % fs->blksize;

        if (size > (fs->blksize - suboff)) {
            size = fs->blksize - suboff;
        }

        size_t dblock = ext2fs_inodegetblock(inode, fs, iblock);

        fs->backing->resource->write(fs->backing->resource, NULL, (void *)((uint64_t)buf + head), dblock * fs->blksize + suboff, size);

        head += size;
    }

    return count;
}

static ssize_t ext2fs_createdirentry(struct ext2fs *fs, struct ext2fs_inode *parent, uint32_t parentidx, uint32_t newidx, uint8_t dirtype, const char *name) {
    uint8_t *buf = alloc(parent->sizelo);
    ext2fs_inoderead(parent, fs, buf, 0, parent->sizelo);

    bool found = false;

    for (size_t i = 0; i < parent->sizelo;) {
        struct ext2fs_direntry *entry = (struct ext2fs_direntry *)((uint64_t)buf + i);

        if (found) {
            entry->inodeidx = newidx;
            entry->dirtype = dirtype;
            entry->namelen = strlen(name);
            entry->entsize = parent->sizelo - i;

            memcpy((void *)((uint64_t)entry + sizeof(struct ext2fs_direntry)), name, strlen(name));

            ext2fs_inodewrite(parent, fs, buf, parentidx, 0, parent->sizelo);

            free(buf);
            return 0;
        }

        uint32_t expected = ALIGN_UP(sizeof(struct ext2fs_direntry) + entry->namelen, sizeof(uint32_t));
        if (entry->entsize != expected) {
            entry->entsize = expected;
            i += expected;

            found = true;

            continue;
        }

        i += entry->entsize;
    }

    free(buf);

    return -1;
}

static bool ext2fs_removedirentry(struct ext2fs *fs, struct ext2fs_inode *parent, uint32_t parentidx, uint32_t inodeidx, bool deleteinode) {
    uint8_t *buf = alloc(parent->sizelo);
    ext2fs_inoderead(parent, fs, buf, 0, parent->sizelo);

    struct ext2fs_direntry *preventry = NULL;
    for (size_t i = 0; i < parent->sizelo;) {
        struct ext2fs_direntry *entry = (struct ext2fs_direntry *)((uint64_t)buf + i);
        if (preventry == NULL) {
            preventry = entry;
        }

        if (entry->inodeidx == inodeidx) {

            size_t oldentsize = entry->entsize;
            size_t oldentinode = entry->inodeidx;

            if (preventry == entry) {
                struct ext2fs_direntry *nextdir = (struct ext2fs_direntry *)((uint64_t)entry + entry->entsize);
                memmove(entry, nextdir, nextdir->entsize); // move all data from next entry to current entry

                entry->entsize += oldentsize; // grow entry to fill the gap
            } else {
                preventry->entsize += oldentsize; // grow entry
            }

            ext2fs_inodewrite(parent, fs, buf, parentidx, 0, parent->sizelo);

            struct ext2fs_inode inode = { 0 };
            ext2fs_inodereadentry(&inode, fs, oldentinode);
            inode.hardlinkcnt--; // remove reference (usually the last reference, unless it's been hardlinked)
            ext2fs_inodewriteentry(&inode, fs, oldentinode);

            if (inode.hardlinkcnt <= 0 && deleteinode) {
                inode.deletedtime = time_realtime.tv_sec; // officially mark as deleted

                for (size_t j = 0; j < 12; j++) { // direct
                    if (inode.blocks[j] == 0) {
                        continue; // ignore unallocated blocks
                    }

                    ext2fs_freeblock(fs, inode.blocks[j]);
                }

                ext2fs_freeblocklist(fs, inode.blocks[12], 0); // single indirect
                ext2fs_freeblocklist(fs, inode.blocks[13], 1); // double indirect
                ext2fs_freeblocklist(fs, inode.blocks[14], 2); // triple indirect

                ext2fs_inodewriteentry(&inode, fs, inodeidx); // update (for deletion time)

                ext2fs_freeinode(fs, inodeidx);

                if ((inode.perms & 0xF000) == 0x4000) {
                    size_t bgdidx = (inodeidx - 1) / fs->sb.inodespergroup;

                    struct ext2fs_blockgroupdesc bgd = { 0 };
                    ext2fs_bgdreadentry(&bgd, fs, bgdidx);
                    bgd.dircnt--; // removing a directory
                    ext2fs_bgdwriteentry(&bgd, fs, bgdidx);
                }
            }

            free(buf);
            return true;
        }

        preventry = entry;
        i += entry->entsize;
    }

    free(buf);

    return false;
}

static ssize_t ext2fs_bgdreadentry(struct ext2fs_blockgroupdesc *bgd, struct ext2fs *fs, uint32_t idx) {
    off_t off = fs->blksize >= 2048 ? fs->blksize : fs->blksize * 2;

    ASSERT_MSG(fs->backing->resource->read(fs->backing->resource, NULL, bgd, off + sizeof(struct ext2fs_blockgroupdesc) * idx, sizeof(struct ext2fs_blockgroupdesc)), "ext2fs: unable to read bgd entry");
    return 0;
}

static ssize_t ext2fs_bgdwriteentry(struct ext2fs_blockgroupdesc *bgd, struct ext2fs *fs, uint32_t idx) {
    off_t off = fs->blksize >= 2048 ? fs->blksize : fs->blksize * 2;

    ASSERT_MSG(fs->backing->resource->write(fs->backing->resource, NULL, bgd, off + sizeof(struct ext2fs_blockgroupdesc) * idx, sizeof(struct ext2fs_blockgroupdesc)), "ext2fs: unable to write bgd entry");
    return 0;
}

static ssize_t ext2fs_inodereadentry(struct ext2fs_inode *inode, struct ext2fs *fs, uint32_t inodeidx) {
    size_t tableidx = (inodeidx - 1) % fs->sb.inodespergroup;
    size_t bgdidx = (inodeidx - 1) / fs->sb.inodespergroup;

    struct ext2fs_blockgroupdesc bgd = { 0 };
    ext2fs_bgdreadentry(&bgd, fs, bgdidx);

    ASSERT_MSG(fs->backing->resource->read(fs->backing->resource, NULL, inode, bgd.inodetable * fs->blksize + fs->sb.inodesize * tableidx, sizeof(struct ext2fs_inode)), "ext2fs: failed to read inode entry");

    return 0;
}

static ssize_t ext2fs_inodewriteentry(struct ext2fs_inode *inode, struct ext2fs *fs, uint32_t inodeidx) {
    size_t tableidx = (inodeidx - 1) % fs->sb.inodespergroup;
    size_t bgdidx = (inodeidx - 1) / fs->sb.inodespergroup;

    struct ext2fs_blockgroupdesc bgd = { 0 };
    ext2fs_bgdreadentry(&bgd, fs, bgdidx);

    ASSERT_MSG(fs->backing->resource->write(fs->backing->resource, NULL, inode, bgd.inodetable * fs->blksize + fs->sb.inodesize * tableidx, sizeof(struct ext2fs_inode)), "ext2fs: failed to write inode entry");

    return 0;
}

static ssize_t ext2fs_resread(struct resource *_this, struct f_description *description, void *buf, off_t loc, size_t count) {
    (void)description;
    struct ext2fs_resource *this = (struct ext2fs_resource *)_this;
    spinlock_acquire(&this->lock);

    struct ext2fs_inode curinode = { 0 };

    ext2fs_inodereadentry(&curinode, this->fs, this->stat.st_ino);

    if ((off_t)(loc + count) > this->stat.st_size) {
        count = count - ((loc + count) - this->stat.st_size); // reading will only ever read total size!
    }

    ssize_t ret = ext2fs_inoderead(&curinode, this->fs, buf, loc, count);
    spinlock_release(&this->lock);
    return ret;
}

static ssize_t ext2fs_reswrite(struct resource *_this, struct f_description *description, const void *buf, off_t loc, size_t count) {
    (void)description;
    struct ext2fs_resource *this = (struct ext2fs_resource *)_this;

    spinlock_acquire(&this->lock);

    struct ext2fs_inode curinode = { 0 };

    ext2fs_inodereadentry(&curinode, this->fs, this->stat.st_ino);

    if ((off_t)(loc + count) > this->stat.st_size) {
        this->stat.st_size = loc + count; // update vfs stat sizes
        this->stat.st_blocks = DIV_ROUNDUP(this->stat.st_size, this->stat.st_blksize);
    }

    ssize_t ret = ext2fs_inodewrite(&curinode, this->fs, buf, this->stat.st_ino, loc, count); // pass to low level write
    spinlock_release(&this->lock);
    return ret;
}

static bool ext2fs_restruncate(struct resource *_this, struct f_description *description, size_t length) {
    (void)description;

    struct ext2fs_resource *this = (struct ext2fs_resource *)_this;

    struct ext2fs_inode curinode = { 0 };
    ext2fs_inodereadentry(&curinode, this->fs, this->stat.st_ino);

    ext2fs_inodeensurelen(&curinode, this->fs, this->stat.st_ino, 0, length); // shrink/grow as we see fit

    this->stat.st_size = (off_t)length;
    this->stat.st_blocks = DIV_ROUNDUP(this->stat.st_size, this->stat.st_blksize);

    return true;
}

static void *ext2fs_resmmap(struct resource *_this, size_t file_page, int flags) {
    (void)flags;
    struct ext2fs_resource *this = (struct ext2fs_resource *)_this;

    spinlock_acquire(&this->lock);

    void *ret = NULL;

    ret = pmm_alloc_nozero(1);
    if (ret == NULL) {
        goto cleanup;
    }

    this->read(_this, NULL, (void *)((uint64_t)ret + VMM_HIGHER_HALF), file_page * PAGE_SIZE, PAGE_SIZE);

cleanup:
    spinlock_release(&this->lock);
    return ret;
}

static bool ext2fs_resunref(struct resource *_this, struct f_description *description) {
    struct ext2fs_resource *this = (struct ext2fs_resource *)_this;

    bool ret = false;

    // XXX: Files not properly deferenced when using `echo data > file` (fault unrelated to ext2fs driver)
    // XXX: Large deletion operations are screwed over by spinlocks (lock.h:15)
    // XXX: Unref is broken due to underlying vfs issues
    this->refcount--;
    if (this->refcount == 0) {
        spinlock_acquire(&this->lock);

        struct ext2fs_inode inode = { 0 };
        ext2fs_inodereadentry(&inode, this->fs, this->stat.st_ino);

        struct ext2fs_inode parent = { 0 };
        ext2fs_inodereadentry(&parent, this->fs, description->node->parent->resource->stat.st_ino);

        if (!S_ISDIR(this->stat.st_mode)) {
            ret = ext2fs_removedirentry(this->fs, &parent, description->node->parent->resource->stat.st_ino, this->stat.st_ino, true);
        } else {
            parent.hardlinkcnt--; // remove reference to parent via ..
            ext2fs_inodewriteentry(&parent, this->fs, description->node->parent->resource->stat.st_ino);
            inode.hardlinkcnt--; // remove reference to ourselves with . (no need to worry about flushing this entry as it'll be taken care of by ext2fs_removedirentry)
            ext2fs_inodewriteentry(&inode, this->fs, this->stat.st_ino); // inode is written as this is never passed in its form to removedirentry!
            ret = ext2fs_removedirentry(this->fs, &parent, description->node->parent->resource->stat.st_ino, this->stat.st_ino, true);
        }

        spinlock_release(&this->lock);
    }
    return ret;
}

static struct vfs_node *ext2fs_mount(struct vfs_node *parent, const char *name, struct vfs_node *source);

static uint8_t ext2fs_inode2dirtype(uint16_t inodetype) {
    switch(inodetype) {
        case 0x8000: return 1;
        case 0x4000: return 2;
        case 0x2000: return 3;
        case 0x6000: return 4;
        case 0x1000: return 5;
        case 0xc000: return 6;
        case 0xa000:
        default:
            return 7;
    }
}

static struct vfs_node *ext2fs_create(struct vfs_filesystem *_this, struct vfs_node *parent, const char *name, int mode) {
    struct vfs_node *node = NULL;
    struct ext2fs_resource *resource = NULL;
    struct ext2fs *this = (struct ext2fs *)_this;

    node = vfs_create_node(_this, parent, name, S_ISDIR(mode));
    if (node == NULL) {
        goto fail;
    }

    resource = resource_create(sizeof(struct ext2fs_resource));
    if (resource == NULL) {
        goto fail;
    }

    if (S_ISREG(mode)) {
        resource->can_mmap = true;
    }

    resource->read = ext2fs_resread;
    resource->write = ext2fs_reswrite;
    resource->truncate = ext2fs_restruncate;
    resource->mmap = ext2fs_resmmap;
    resource->unref = ext2fs_resunref;

    resource->stat.st_size = 0;
    resource->stat.st_blocks = 0;
    resource->stat.st_blksize = this->blksize;
    resource->stat.st_dev = this->devid;
    resource->stat.st_mode = mode;
    resource->stat.st_nlink = 1;

    resource->stat.st_atim = time_realtime;
    resource->stat.st_ctim = time_realtime;
    resource->stat.st_mtim = time_realtime;

    resource->stat.st_ino = ext2fs_allocinode(this);

    uint16_t inodetype = S_ISREG(mode) ? 0x8000 :
        S_ISDIR(mode) ? 0x4000 :
        S_ISCHR(mode) ? 0x2000 :
        S_ISBLK(mode) ? 0x6000 :
        S_ISFIFO(mode) ? 0x1000 :
        S_ISSOCK(mode) ? 0xc000 :
        0xa000;

    struct ext2fs_inode inode = {
        .perms = (mode & 0xfff) | inodetype,
        .uid = 0,
        .sizelo = 0,
        .accesstime = time_realtime.tv_sec,
        .creationtime = time_realtime.tv_sec,
        .modifiedtime = time_realtime.tv_sec,
        .deletedtime = 0,
        .gid = 0,
        .hardlinkcnt = 1,
        .sectors = this->blksize / this->backing->resource->stat.st_blksize,
        .flags = 0,
        .oss = 0,
        .blocks = {},
        .gennum = 0,
        .eab = 0,
        .sizehi = 0,
        .fragaddr = 0,
    };
    inode.blocks[0] = ext2fs_allocblock(this);

    ext2fs_inodewriteentry(&inode, this, resource->stat.st_ino);

    struct ext2fs_inode parentinode = { 0 };
    ext2fs_inodereadentry(&parentinode, this, parent->resource->stat.st_ino);

    uint8_t dirtype = ext2fs_inode2dirtype(inodetype);

    if (S_ISDIR(mode)) {
        uint8_t *buf = alloc(this->blksize);

        struct ext2fs_direntry *dotent = (struct ext2fs_direntry *)buf;
        dotent->inodeidx = resource->stat.st_ino;
        dotent->entsize = 12;
        dotent->namelen = 1;
        dotent->dirtype = 2;
        ((char *)((uint64_t)dotent + sizeof(struct ext2fs_direntry)))[0] = '.';

        struct ext2fs_direntry *dotdotent = (struct ext2fs_direntry *)((uint64_t)buf + dotent->entsize);
        dotdotent->inodeidx = parent->resource->stat.st_ino;
        dotdotent->entsize = this->blksize - dotent->entsize;
        dotdotent->namelen = 2;
        dotdotent->dirtype = 2;
        strncpy(((char *)((uint64_t)dotdotent + sizeof(struct ext2fs_direntry))), "..", 2); // only copy the two dots

        ext2fs_inodewrite(&inode, this, buf, resource->stat.st_ino, 0, this->blksize); // update directory entries at base

        size_t bgdidx = (resource->stat.st_ino - 1) / this->sb.inodespergroup;
        struct ext2fs_blockgroupdesc bgd = { 0 };
        ext2fs_bgdreadentry(&bgd, this, bgdidx);
        bgd.dircnt++; // new directory in this block group :+1:
        ext2fs_bgdwriteentry(&bgd, this, bgdidx);

        parentinode.hardlinkcnt++; // parent is referenced ONCE
        inode.hardlinkcnt++; // WE are referenced ONCE
        ext2fs_inodewriteentry(&parentinode, this, parent->resource->stat.st_ino);
        ext2fs_inodewriteentry(&inode, this, resource->stat.st_ino);
    }

    ext2fs_createdirentry(this, &parentinode, parent->resource->stat.st_ino, resource->stat.st_ino, dirtype, name);

    resource->fs = this;

    node->resource = (struct resource *)resource;
    parent->resource->stat.st_nlink = parentinode.hardlinkcnt;
    node->resource->stat.st_nlink = inode.hardlinkcnt;
    node->resource->refcount = S_ISDIR(mode) ? inode.hardlinkcnt - 1 : inode.hardlinkcnt; // physical references (ignoring initial, these are "virtual" entries)
    return node;
fail:
    if (node != NULL) {
        free(node); // TODO: Use vfs_destroy_node
    }
    if (resource != NULL) {
        free(resource);
    }

    return NULL;
}

static void ext2fs_readlink(struct ext2fs_inode *inode, struct ext2fs *fs, char *buffer) {
    if (inode->sizelo <= 60) { // fast
        strncpy(buffer, (const char *)inode->blocks, inode->sizelo);
        return;
    }

    ext2fs_inoderead(inode, fs, buffer, 0, inode->sizelo); // slow
}

static void ext2fs_populate(struct vfs_filesystem *_this, struct vfs_node *node) {
    struct ext2fs *fs = (struct ext2fs *)_this;
    struct ext2fs_inode parent = { 0 };
    ext2fs_inodereadentry(&parent, fs, node->resource->stat.st_ino);

    void *buf = alloc(parent.sizelo);
    ext2fs_inoderead(&parent, fs, buf, 0, parent.sizelo);

    for (size_t i = 0; i < parent.sizelo;) {
        struct ext2fs_direntry *direntry = (struct ext2fs_direntry *)((uint64_t)buf + i);

        char *namebuf = alloc(direntry->namelen + 1);
        memcpy(namebuf, (void *)((uint64_t)direntry + sizeof(struct ext2fs_direntry)), direntry->namelen);

        if (direntry->inodeidx == 0) {
            free(namebuf);
            goto cleanup;
        }

        if (!strcmp(namebuf, ".") || !strcmp(namebuf, "..")) {
            i += direntry->entsize; // vfs already handles creating these
            continue;
        }

        struct ext2fs_inode inode = { 0 };
        ext2fs_inodereadentry(&inode, fs, direntry->inodeidx);

        uint16_t mode = (inode.perms & 0xFFF) |
            (direntry->dirtype == 1 ? S_IFREG :
             direntry->dirtype == 2 ? S_IFDIR :
             direntry->dirtype == 3 ? S_IFCHR :
             direntry->dirtype == 4 ? S_IFBLK :
             direntry->dirtype == 5 ? S_IFIFO :
             direntry->dirtype == 6 ? S_IFSOCK :
             S_IFLNK);

        struct vfs_node *fnode = vfs_create_node((struct vfs_filesystem *)fs, node, namebuf, S_ISDIR(mode));
        struct ext2fs_resource *fres = resource_create(sizeof(struct ext2fs_resource));

        if (S_ISREG(mode)) {
            fres->can_mmap = true;
        }

        fres->read = ext2fs_resread;
        fres->write = ext2fs_reswrite;
        fres->truncate = ext2fs_restruncate;
        fres->mmap = ext2fs_resmmap;
        fres->unref = ext2fs_resunref;

        fres->stat.st_uid = inode.uid;
        fres->stat.st_gid = inode.gid;
        fres->stat.st_mode = mode;
        fres->stat.st_ino = direntry->inodeidx;
        fres->stat.st_size = inode.sizelo | ((uint64_t)inode.sizehi >> 32);
        fres->stat.st_nlink = inode.hardlinkcnt;
        fres->refcount = S_ISDIR(mode) ? inode.hardlinkcnt - 1 : inode.hardlinkcnt; // exclude dot entries in directory inode links (our vfs relies on its own implementation)
        fres->stat.st_blksize = fs->blksize;
        fres->stat.st_blocks = fres->stat.st_size / fs->blksize;

        fres->stat.st_atim = (struct timespec) { .tv_sec = inode.accesstime, .tv_nsec = 0 };
        fres->stat.st_ctim = (struct timespec) { .tv_sec = inode.creationtime, .tv_nsec = 0 };
        fres->stat.st_mtim = (struct timespec) { .tv_sec = inode.modifiedtime, .tv_nsec = 0 };

        fres->fs = fs;

        fnode->resource = (struct resource *)fres;
        fnode->populated = false;

        HASHMAP_SINSERT(&fnode->parent->children, namebuf, fnode);

        if (S_ISDIR(mode)) {
            vfs_create_dotentries(fnode, node); // set up for correct directory structure
            ext2fs_populate((struct vfs_filesystem *)fs, fnode); // recurse filesystem
        }

        if (S_ISLNK(mode)) {
            char *linkbuffer = alloc(inode.sizelo);
            ext2fs_readlink(&inode, fs, linkbuffer); // implicit acceptance of link buffer length being the same as the inode
            fnode->symlink_target = strdup(linkbuffer);
            free(linkbuffer); // free current context
        }

        i += direntry->entsize;
    }

    node->populated = true; // we already populated this node with all existing files
cleanup:
    free(buf);
    return;
}

static struct vfs_node *ext2fs_link(struct vfs_filesystem *_this, struct vfs_node *parent, const char *name, struct vfs_node *node) {
    if (S_ISDIR(node->resource->stat.st_mode)) {
        errno = EISDIR;
        return NULL;
    }

    // persist link disk-wise (however, the usage of resources will be a bit inefficient during population due to it having no clue it's a hardlink (and it's not like it's possible to tell it))
    struct ext2fs_inode parentinode = { 0 };
    ext2fs_inodereadentry(&parentinode, (struct ext2fs *)_this, parent->resource->stat.st_ino);
    struct ext2fs_inode linkinode = { 0 };
    ext2fs_inodereadentry(&linkinode, (struct ext2fs *)_this, node->resource->stat.st_ino);
    linkinode.hardlinkcnt++;
    node->resource->stat.st_nlink++; // increase stat as well
    ext2fs_createdirentry((struct ext2fs *)_this, &parentinode, parent->resource->stat.st_ino, node->resource->stat.st_ino, ext2fs_inode2dirtype(linkinode.perms & (0xF000)), name);
    ext2fs_inodewriteentry(&linkinode, (struct ext2fs *)_this, node->resource->stat.st_ino);

    // satisfy vfs's convention of hardlinks
    struct vfs_node *new_node = vfs_create_node(_this, parent, name, false);
    if (new_node == NULL) {
        return NULL;
    }

    node->resource++;
    new_node->resource = node->resource;
    return new_node;
}

static struct vfs_node *ext2fs_symlink(struct vfs_filesystem *_this, struct vfs_node *parent, const char *name, const char *target) {
    struct vfs_node *new_node = NULL;

    new_node = ext2fs_create(_this, parent, name, 0777 | S_IFLNK); // create us a resource for this
    new_node->symlink_target = strdup(target);
    if (strlen(target) > 60) { // slow symlink
        new_node->resource->write(new_node->resource, NULL, target, 0, strlen(target)); // persist disk-wise
    } else { // fast symlink
        struct ext2fs_inode inode = { 0 };
        ext2fs_inodereadentry(&inode, (struct ext2fs *)_this, new_node->resource->stat.st_ino);
        strncpy((char *)inode.blocks, target, strlen(target));
        ext2fs_inodewriteentry(&inode, (struct ext2fs *)_this, new_node->resource->stat.st_ino);
    }
    return new_node;
}

static inline struct vfs_filesystem *ext2fs_instantiate(void) {
    struct ext2fs *new_fs = alloc(sizeof(struct ext2fs));
    if (new_fs == NULL) {
        return NULL;
    }

    new_fs->mount = ext2fs_mount;
    new_fs->create = ext2fs_create;
    new_fs->populate = ext2fs_populate;
    new_fs->symlink = ext2fs_symlink;
    new_fs->link = ext2fs_link;

    return (struct vfs_filesystem *)new_fs;
}

static struct vfs_node *ext2fs_mount(struct vfs_node *parent, const char *name, struct vfs_node *source) {

    struct ext2fs *new_fs = (struct ext2fs *)ext2fs_instantiate();

    source->resource->read(source->resource, NULL, &new_fs->sb, source->resource->stat.st_blksize * 2, sizeof(struct ext2fs_superblock));

    if (new_fs->sb.sig != 0xef53) {
        panic(NULL, false, "Told to mount an ext2 filesystem whilst source is not ext2!");
    }

    new_fs->backing = source;
    new_fs->blksize = 1024 << new_fs->sb.blksize;
    new_fs->fragsize = 1024 << new_fs->sb.fragsize;
    new_fs->bgdcnt = new_fs->sb.blockcnt / new_fs->sb.blockspergroup;

    ASSERT_MSG(!ext2fs_inodereadentry(&new_fs->root, new_fs, 2), "ext2fs: unable to read root inode");

    struct vfs_node *node = vfs_create_node((struct vfs_filesystem *)new_fs, parent, name, true);
    if (node == NULL) {
        return NULL;
    }
    struct ext2fs_resource *resource = resource_create(sizeof(struct ext2fs_resource));
    if (resource == NULL) {
        return NULL;
    }

    resource->stat.st_size = new_fs->root.sizelo | ((uint64_t)new_fs->root.sizehi >> 32);
    resource->stat.st_blksize = new_fs->blksize;
    resource->stat.st_blocks = resource->stat.st_size / resource->stat.st_blksize;
    resource->stat.st_dev = source->resource->stat.st_rdev; // assign to device id of source device
    resource->stat.st_mode = 0644 | S_IFDIR;
    resource->stat.st_nlink = new_fs->root.hardlinkcnt;
    resource->stat.st_ino = 2; // root inode

    resource->stat.st_atim = time_realtime;
    resource->stat.st_ctim = time_realtime;
    resource->stat.st_mtim = time_realtime;

    resource->fs = new_fs;

    node->resource = (struct resource *)resource;

    ext2fs_populate((struct vfs_filesystem *)new_fs, node); // recursively fill vfs with filesystem

    return node; // root node (will become child of parent)
}

void ext2fs_init(void) {
    struct vfs_filesystem *new_fs = ext2fs_instantiate();
    if (new_fs == NULL) {
        panic(NULL, false, "Failed to instantiate ext2fs");
    }

    vfs_add_filesystem(new_fs, "ext2fs");
}
