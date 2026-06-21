#include <misc/log.h>
#include <misc/stddef.h>
#include <misc/string.h>
#include <misc/errno.h>
#include <misc/complier.h>
#include <misc/align.h>
#include <mm/kalloc.h>
#include <mm/bcache.h>
#include <sync/spinlock.h>
#include <fs/vfs.h>
#include <fs/super.h>
#include <fs/inode.h>
#include <fs/dentry.h>
#include <fs/file.h>
#include <fs/namei.h>
#include <fs/fs_types.h>
#include <hal/blk.h>
#include "ext4.h"

/* ─────────────────────────────────────────────
 * Helpers: read raw bytes from block device
 * ───────────────────────────────────────────── */

/* Read `size` bytes from device `dev` at byte offset `offset` into `buf`.
   Uses bcache (BLOCK_SIZE=512 sectors). */
static int ext4_read_at(dev_t dev, u64 offset, void *buf, size_t size)
{
	u8 *p = (u8 *)buf;
	while (size > 0) {
		u64 blockno = offset / BLOCK_SIZE;
		u64 off     = offset % BLOCK_SIZE;
		size_t n    = BLOCK_SIZE - off;
		if (n > size) n = size;

		struct buf *b = bread(dev, blockno);
		if (!b) {
			warnf("ext4: bread failed at block %llu", blockno);
			return -EIO;
		}
		memcpy(p, b->data + off, n);
		brelse(b);

		p      += n;
		offset += n;
		size   -= n;
	}
	return 0;
}

/* Write `size` bytes to device `dev` at byte offset `offset` from `buf`.
   Uses bcache (BLOCK_SIZE=512 sectors). */
static int ext4_write_at(dev_t dev, u64 offset, const void *buf, size_t size)
{
	const u8 *p = (const u8 *)buf;
	while (size > 0) {
		u64 blockno = offset / BLOCK_SIZE;
		u64 off     = offset % BLOCK_SIZE;
		size_t n    = BLOCK_SIZE - off;
		if (n > size) n = size;

		struct buf *b = bread(dev, blockno);
		if (!b) {
			warnf("ext4: bread failed at block %llu (write)", blockno);
			return -EIO;
		}
		memcpy(b->data + off, p, n);
		bwrite(b);
		brelse(b);

		p      += n;
		offset += n;
		size   -= n;
	}
	return 0;
}

/* ─────────────────────────────────────────────
 * Superblock operations
 * ───────────────────────────────────────────── */

static struct ext4_sb_info *ext4_get_sbi(struct super_block *sb)
{
	return (struct ext4_sb_info *)sb->s_fs_info;
}

/* Read the on-disk superblock, validate, and fill VFS super_block + sbi */
int ext4_fill_super(struct super_block *vsb, dev_t dev, void *data)
{
	int ret;
	struct ext4_sb_info *sbi = kzalloc(sizeof(struct ext4_sb_info));
	if (!sbi)
		return -ENOMEM;

	ret = ext4_read_at(dev, EXT4_SUPERBLOCK_OFFSET,
			   &sbi->sb, sizeof(sbi->sb));
	if (ret < 0) {
		kfree(sbi);
		return ret;
	}

	/* Validate magic */
	if (sbi->sb.s_magic != EXT4_MAGIC) {
		warnf("ext4: bad magic 0x%04x (expected 0x%04x)",
		      sbi->sb.s_magic, EXT4_MAGIC);
		kfree(sbi);
		return -EINVAL;
	}

	/* Block size = 1024 << s_log_block_size */
		if (sbi->sb.s_log_block_size > 5) {
			warnf("ext4: invalid s_log_block_size %u", sbi->sb.s_log_block_size);
			kfree(sbi);
			return -EINVAL;
		}
		sbi->block_size      = 1024U << sbi->sb.s_log_block_size;
	sbi->blocks_per_group = sbi->sb.s_blocks_per_group;
	sbi->inodes_per_group = sbi->sb.s_inodes_per_group;
	sbi->inode_size       = sbi->sb.s_inode_size ? sbi->sb.s_inode_size : 128;
	sbi->inodes_per_block = sbi->block_size / sbi->inode_size;
	sbi->dev              = dev;

	/* Number of block groups */
	sbi->groups_count = (sbi->sb.s_blocks_count_lo +
			     sbi->blocks_per_group - 1) /
			    sbi->blocks_per_group;

	/* Descriptor size: 64 for 64bit feature, 32 otherwise */
	if (sbi->sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
		sbi->desc_size = 64;
	else
		sbi->desc_size = sizeof(struct ext4_group_desc);

	sbi->desc_per_block = sbi->block_size / sbi->desc_size;

	vsb->s_fs_info     = sbi;
	vsb->s_blocksize   = sbi->block_size;
	vsb->s_blocksize_bits = sbi->sb.s_log_block_size + 10; /* log2(1024)=10 */
	vsb->s_dev         = dev;

	return 0;
}

/* ─────────────────────────────────────────────
 * Block group descriptor reading
 * ───────────────────────────────────────────── */

/* Read the block group descriptor for group `bg_idx`.
   The block group descriptor table starts at the block AFTER the superblock.
   For 1KiB blocks: block 2 (= block 0 for superblock, block 1 for GDT start).
   For larger blocks, it's always the first block after the superblock. */
static int ext4_read_group_desc(struct ext4_sb_info *sbi, u32 bg_idx,
				struct ext4_group_desc *bgd)
{
	u32 block_size = sbi->block_size;
	/* GDT starts at the first block after the superblock area.
	   Superblock is 1024 bytes, so GDT starts at block 0 or 1 depending on block size. */
	u32 gdt_block;
	if (block_size > 1024)
		gdt_block = 1;   /* superblock within block 0, GDT at block 1 */
	else
		gdt_block = 2;   /* superblock at block 1, GDT at block 2 */

	/* Byte offset of descriptor bg_idx */
	u64 offset = (u64)gdt_block * block_size +
		     (u64)bg_idx * sbi->desc_size;

	return ext4_read_at(sbi->dev, offset, bgd, sizeof(*bgd));
}

/* ─────────────────────────────────────────────
 * Inode reading
 * ───────────────────────────────────────────── */

/* Write superblock back to disk (1024 bytes at offset 1024) */
static int ext4_write_superblock(struct ext4_sb_info *sbi)
{
	return ext4_write_at(sbi->dev, EXT4_SUPERBLOCK_OFFSET,
			     &sbi->sb, sizeof(sbi->sb));
}

/* Write block group descriptor back to disk */
static int ext4_write_group_desc(struct ext4_sb_info *sbi, u32 bg_idx,
				  const struct ext4_group_desc *bgd)
{
	u32 block_size = sbi->block_size;
	u32 gdt_block;
	if (block_size > 1024)
		gdt_block = 1;
	else
		gdt_block = 2;

	u64 offset = (u64)gdt_block * block_size +
		     (u64)bg_idx * sbi->desc_size;
	return ext4_write_at(sbi->dev, offset, bgd, sizeof(*bgd));
}

/* Allocate a free inode number from the inode bitmap.
   Returns inode number on success, 0 on failure. */
static u32 ext4_alloc_inode_from_disk(struct super_block *sb)
{
	struct ext4_sb_info *sbi = ext4_get_sbi(sb);
	u32 block_size = sbi->block_size;
	u8 *bitmap = kzalloc(block_size);
	if (!bitmap)
		return 0;

	for (u32 bg = 0; bg < sbi->groups_count; bg++) {
		struct ext4_group_desc bgd;
		if (ext4_read_group_desc(sbi, bg, &bgd) < 0)
			continue;

		if (bgd.bg_free_inodes_count_lo == 0)
			continue;

		/* Read inode bitmap block */
		u64 bm_off = (u64)bgd.bg_inode_bitmap_lo * block_size;
		if (ext4_read_at(sbi->dev, bm_off, bitmap, block_size) < 0)
			continue;

		/* Skip reserved inodes (0, 1) — in bitmap bits 0..1 */
		u32 start = (bg == 0) ? 10 : 0;
		for (u32 i = start; i < sbi->inodes_per_group; i++) {
			if (!(bitmap[i / 8] & (1 << (i % 8)))) {
				/* Found free inode */
				bitmap[i / 8] |= (1 << (i % 8));

				/* Write back bitmap */
				ext4_write_at(sbi->dev, bm_off, bitmap, block_size);

				/* Update group descriptor */
				bgd.bg_free_inodes_count_lo--;
				ext4_write_group_desc(sbi, bg, &bgd);

				/* Update superblock */
				sbi->sb.s_free_inodes_count_lo--;
				ext4_write_superblock(sbi);

				kfree(bitmap);
				return bg * sbi->inodes_per_group + i + 1;
			}
		}
	}

	kfree(bitmap);
	return 0;
}

/* Allocate a free data block from the block bitmap.
   Returns physical block number on success, 0 on failure. */
static u32 ext4_alloc_block(struct super_block *sb)
{
	struct ext4_sb_info *sbi = ext4_get_sbi(sb);
	u32 block_size = sbi->block_size;
	u8 *bitmap = kzalloc(block_size);
	if (!bitmap)
		return 0;

	for (u32 bg = 0; bg < sbi->groups_count; bg++) {
		struct ext4_group_desc bgd;
		if (ext4_read_group_desc(sbi, bg, &bgd) < 0)
			continue;

		if (bgd.bg_free_blocks_count_lo == 0)
			continue;

		u64 bm_off = (u64)bgd.bg_block_bitmap_lo * block_size;
		if (ext4_read_at(sbi->dev, bm_off, bitmap, block_size) < 0)
			continue;

		/* Search bitmap for free bit */
		u32 bits_per_block = block_size * 8;
		u32 search_count = sbi->blocks_per_group < bits_per_block ?
				   sbi->blocks_per_group : bits_per_block;

		for (u32 i = 0; i < search_count; i++) {
			if (!(bitmap[i / 8] & (1 << (i % 8)))) {
				bitmap[i / 8] |= (1 << (i % 8));

				ext4_write_at(sbi->dev, bm_off, bitmap, block_size);

				bgd.bg_free_blocks_count_lo--;
				ext4_write_group_desc(sbi, bg, &bgd);

				sbi->sb.s_free_blocks_count_lo--;
				ext4_write_superblock(sbi);

				/* Zero out the allocated block */
				u8 *zero_block = kzalloc(block_size);
				if (zero_block) {
					u64 blk_off = (u64)(bg * sbi->blocks_per_group + i + sbi->sb.s_first_data_block) * block_size;
					ext4_write_at(sbi->dev, blk_off, zero_block, block_size);
					kfree(zero_block);
				}

				kfree(bitmap);
				return bg * sbi->blocks_per_group + i +
				       sbi->sb.s_first_data_block;
			}
		}
	}

	kfree(bitmap);
	return 0;
}

/* Read a raw inode from disk and fill into VFS inode structure */
static int ext4_read_inode(struct super_block *sb, struct inode *inode)
{
	struct ext4_sb_info *sbi = ext4_get_sbi(sb);
	u32 ino = inode->i_ino;

	if (ino == 0 || ino > sbi->sb.s_inodes_count)
		return -ENOENT;

	/* Find block group and index within group */
	u32 bg_idx     = (ino - 1) / sbi->inodes_per_group;
	u32 bg_ino_idx = (ino - 1) % sbi->inodes_per_group;

	if (bg_idx >= sbi->groups_count)
		return -ENOENT;

	/* Read block group descriptor to find inode table location */
	struct ext4_group_desc bgd;
	int ret = ext4_read_group_desc(sbi, bg_idx, &bgd);
	if (ret < 0)
		return ret;

	/* Inode table byte offset */
	u64 inode_table_off = (u64)bgd.bg_inode_table_lo * sbi->block_size;
	u64 inode_off       = inode_table_off +
			      (u64)bg_ino_idx * sbi->inode_size;

	/* Read raw inode (we only need first 128 bytes) */
	struct ext4_inode raw_inode;
	ret = ext4_read_at(sbi->dev, inode_off, &raw_inode,
			   sizeof(raw_inode));
	if (ret < 0)
		return ret;

	/* Fill VFS inode from raw inode */
	u16 mode = raw_inode.i_mode;
	if (mode & EXT4_S_IFREG)
		inode->i_mode = S_IFREG;
	else if (mode & EXT4_S_IFDIR)
		inode->i_mode = S_IFDIR;
	else if (mode & EXT4_S_IFLNK)
		inode->i_mode = S_IFLNK;
	else
		inode->i_mode = S_IFREG;

	inode->i_uid     = raw_inode.i_uid;
	inode->i_gid     = raw_inode.i_gid;
	inode->i_size    = (u64)raw_inode.i_size_lo |
			   ((u64)raw_inode.i_size_high << 32);
	inode->i_blocks  = raw_inode.i_blocks_lo; /* 512-byte sectors */
	inode->i_nlink   = raw_inode.i_links_count;
	inode->i_atime   = raw_inode.i_atime;
	inode->i_mtime   = raw_inode.i_mtime;
	inode->i_ctime   = raw_inode.i_ctime;
	inode->i_sb      = sb;

	/* Save raw inode block pointers / extent data as private data */
	struct ext4_inode *priv = kmalloc(sizeof(struct ext4_inode));
	if (priv) {
		memcpy(priv, &raw_inode, sizeof(struct ext4_inode));
		inode->i_private = priv;
	}

	/* Set operations based on file type */
	if (S_ISDIR(inode->i_mode)) {
		inode->i_op  = NULL; /* we'll set these via the filesystem type */
		inode->i_fop = NULL;
	} else {
		inode->i_op  = NULL;
		inode->i_fop = NULL;
	}

	return 0;
}

/* ─────────────────────────────────────────────
 * Extent tree traversal — find physical block
 * ───────────────────────────────────────────── */

/* For a given logical block number and raw inode with extents,
   return the physical block number (in filesystem-block-size units).
   Returns ~0 on error. */
static u32 ext4_find_extent_block(struct ext4_sb_info *sbi,
				  struct ext4_inode *raw_inode,
				  u32 logical_block)
{
	struct ext4_extent_header *eh;
	u32 block_size = sbi->block_size;

	/* The extent header is at the start of i_block[] */
	eh = (struct ext4_extent_header *)raw_inode->i_block;

	if (eh->eh_magic != EXT4_EXTENT_MAGIC) {
		warnf("ext4: bad extent magic 0x%04x", eh->eh_magic);
		return ~0;
	}

	u16 depth    = eh->eh_depth;
	u16 entries  = eh->eh_entries;
	(void)entries;

	if (depth == 0) {
		/* Leaf node — search the extent entries directly after header */
		struct ext4_extent *ext = (struct ext4_extent *)(eh + 1);
		for (u16 i = 0; i < eh->eh_entries; i++) {
			if (logical_block >= ext[i].ee_block &&
			    logical_block < ext[i].ee_block + ext[i].ee_len) {
				u32 phys_block = ext[i].ee_start_lo |
					((u32)ext[i].ee_start_hi << 16);
				phys_block += (logical_block - ext[i].ee_block);
				return phys_block;
			}
		}
	} else {
		/* Index node — descend (handle depth 1 for simplicity) */
		struct ext4_extent_idx *idx = (struct ext4_extent_idx *)(eh + 1);

		/* Find the index entry that covers this block */
		u16 i;
		for (i = 0; i < eh->eh_entries; i++) {
			if (logical_block < idx[i].ei_block)
				break;
		}
		if (i == 0 || i > eh->eh_entries)
			return ~0;

		/* Descend to child node at idx[i-1] */
		u32 child_block = idx[i - 1].ei_leaf_lo |
				  ((u32)idx[i - 1].ei_leaf_hi << 16);
		u64 child_offset = (u64)child_block * block_size;

		/* Read the child node block */
		u8 *child_buf = kzalloc(block_size);
		if (!child_buf)
			return ~0;

		int ret = ext4_read_at(sbi->dev, child_offset,
				       child_buf, block_size);
		if (ret < 0) {
			kfree(child_buf);
			return ~0;
		}

		/* Child is a leaf (depth-1 == 0 expected) */
		struct ext4_extent_header *child_eh =
			(struct ext4_extent_header *)child_buf;
		struct ext4_extent *child_ext =
			(struct ext4_extent *)(child_eh + 1);

		u32 phys_block = ~0;
		for (u16 j = 0; j < child_eh->eh_entries; j++) {
			if (logical_block >= child_ext[j].ee_block &&
			    logical_block < child_ext[j].ee_block +
					    child_ext[j].ee_len) {
				phys_block = child_ext[j].ee_start_lo |
					((u32)child_ext[j].ee_start_hi << 16);
				phys_block += (logical_block -
					       child_ext[j].ee_block);
				break;
			}
		}

		kfree(child_buf);
		return phys_block;
	}

	return ~0;
}

/* ─────────────────────────────────────────────
 * File data reading
 * ───────────────────────────────────────────── */

/* Read file data using the inode's extent tree or direct blocks.
   Returns bytes read, or negative on error. */
static ssize_t ext4_read_data(struct super_block *sb,
			      struct ext4_inode *raw_inode,
			      u64 pos, u8 *buf, size_t len)
{
	struct ext4_sb_info *sbi = ext4_get_sbi(sb);
	u64 file_size = (u64)raw_inode->i_size_lo |
			((u64)raw_inode->i_size_high << 32);

	if (pos >= file_size)
		return 0;
	if (pos + len > file_size)
		len = file_size - pos;

	u32 block_size    = sbi->block_size;
	u32 block_size_bits = sbi->sb.s_log_block_size + 10;
	u32 block_cnt     = (file_size + block_size - 1) >> block_size_bits;

	ssize_t total = 0;

	while (len > 0) {
		u32 logical_block = pos >> block_size_bits;
		u32 block_off     = pos & (block_size - 1);
		u32 to_copy       = block_size - block_off;
		if (to_copy > len) to_copy = len;

		/* Find physical block */
		u32 phys_block;
		if (raw_inode->i_flags & EXT4_EXTENTS_FL) {
			phys_block = ext4_find_extent_block(
				sbi, raw_inode, logical_block);
		} else {
			/* Direct blocks only (for non-extent files) */
			if (logical_block < EXT4_DIRECT_BLOCKS) {
				phys_block = raw_inode->i_block[logical_block];
			} else {
				warnf("ext4: indirect blocks not supported");
				return total > 0 ? total : -ENOSYS;
			}
		}

		if (phys_block == ~0U) {
			/* Sparse hole: zero-fill and continue */
			memset(buf + total, 0, to_copy);
			total += to_copy;
			pos   += to_copy;
			len   -= to_copy;
			continue;
		}

		/* Read the block */
		u64 byte_offset = (u64)phys_block * block_size + block_off;
		u8  tmp[512]; /* bcache block size */

		/* Copy bcache blocks piece-by-piece */
		while (to_copy > 0) {
			u64 dev_block = byte_offset / BLOCK_SIZE;
			u64 dev_off   = byte_offset % BLOCK_SIZE;
			size_t n      = BLOCK_SIZE - dev_off;
			if (n > to_copy) n = to_copy;

			struct buf *b = bread(sbi->dev, dev_block);
			if (!b)
				return total > 0 ? total : -EIO;

			memcpy(buf + total, b->data + dev_off, n);
			brelse(b);

			byte_offset += n;
			total       += n;
			pos         += n;
			len         -= n;
			to_copy     -= n;
		}
	}

	return total;
}

/* ─────────────────────────────────────────────
 * Directory operations
 * ───────────────────────────────────────────── */

/* Look up a single component name in a directory inode.
   Returns the inode number, or 0 if not found. */
static u32 ext4_dir_lookup(struct super_block *sb,
			   struct ext4_inode *dir_inode,
			   const char *name, int namelen)
{
	struct ext4_sb_info *sbi = ext4_get_sbi(sb);

	/* Directory content is read as file data — it's a sequence of
	   ext4_dirent entries */
	u32 block_size = sbi->block_size;
	u32 dir_blocks = ((u64)dir_inode->i_size_lo +
			  (u64)dir_inode->i_size_high * (u64)-1 >
			  0) ? 0 : 0; /* force full read */
	u64 dir_size = (u64)dir_inode->i_size_lo |
		       ((u64)dir_inode->i_size_high << 32);


	u8 *buf = kzalloc(dir_size < 4096 ? 4096 : dir_size);
	if (!buf)
		return 0;

	ssize_t ret = ext4_read_data(sb, dir_inode, 0, buf, dir_size);
	if (ret < 0 || (size_t)ret < sizeof(struct ext4_dirent)) {
		kfree(buf);
		return 0;
	}

	u32 found_ino = 0;
	u64 offset = 0;
	while (offset < dir_size) {
		struct ext4_dirent *de = (struct ext4_dirent *)(buf + offset);
		if (de->inode == 0 || de->rec_len == 0)
			break;


		if (de->name_len == (u8)namelen &&
		    memcmp(de->name, name, namelen) == 0) {
			found_ino = de->inode;
			break;
		}
		offset += de->rec_len;
	}

	kfree(buf);
	return found_ino;
}

/* ─────────────────────────────────────────────
 * VFS callback implementations
 * ───────────────────────────────────────────── */

/* super_operations */
static struct inode *ext4_alloc_inode(struct super_block *sb)
{
	return kzalloc(sizeof(struct inode));
}

static void ext4_destroy_inode(struct inode *inode)
{
	if (inode->i_private)
		kfree(inode->i_private);
	kfree(inode);
}

static int ext4_write_inode(struct inode *inode)
{
	if (!inode || !inode->i_private)
		return -EINVAL;

	struct ext4_inode *raw = (struct ext4_inode *)inode->i_private;
	struct ext4_sb_info *sbi = ext4_get_sbi(inode->i_sb);
	u32 block_size = sbi->block_size;

	u32 bg_idx = (inode->i_ino - 1) / sbi->inodes_per_group;
	u32 bg_ino_idx = (inode->i_ino - 1) % sbi->inodes_per_group;
	struct ext4_group_desc bgd;
	if (ext4_read_group_desc(sbi, bg_idx, &bgd) < 0)
		return -EIO;

	u64 inode_table_off = (u64)bgd.bg_inode_table_lo * block_size;
	u64 inode_off = inode_table_off + (u64)bg_ino_idx * sbi->inode_size;

	ext4_write_at(sbi->dev, inode_off, raw, sizeof(*raw));
	return 0;
}

static void ext4_put_super(struct super_block *sb)
{
	struct ext4_sb_info *sbi = ext4_get_sbi(sb);
	if (sbi) {
		kfree(sbi);
		sb->s_fs_info = NULL;
	}
}

static int ext4_statfs(struct super_block *sb)
{
	struct ext4_sb_info *sbi = ext4_get_sbi(sb);
	if (!sbi)
		return -EIO;
	/* Just log info for now */
	return 0;
}

/* ─────────────────────────────────────────────
 * Operation structures (defined before use)
 * ───────────────────────────────────────────── */

/* Forward declarations for static functions */
static struct dentry *ext4_lookup(struct inode *dir, struct dentry *dentry);
static ssize_t ext4_read_file(struct file *file, void *buf,
			      size_t count, loff_t *pos);
static ssize_t ext4_write_file(struct file *file, const void *buf,
			       size_t count, loff_t *pos);
static int ext4_readdir(struct file *file, struct dirent *dirent_buf,
			   size_t count);
static loff_t ext4_llseek(struct file *file, loff_t offset, int whence);
static int ext4_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode);
static int ext4_create(struct inode *dir, struct dentry *dentry, umode_t mode);
static int ext4_unlink(struct inode *dir, struct dentry *dentry);

struct super_operations ext4_super_ops = {
	.alloc_inode   = ext4_alloc_inode,
	.destroy_inode = ext4_destroy_inode,
	.write_inode   = ext4_write_inode,
	.put_super     = ext4_put_super,
	.statfs        = ext4_statfs,
};

struct inode_operations ext4_dir_inode_ops = {
	.lookup = ext4_lookup,
	.mkdir  = ext4_mkdir,
	.create = ext4_create,
	.unlink = ext4_unlink,
};

struct file_operations ext4_file_operations = {
	.read   = ext4_read_file,
	.write  = ext4_write_file,
	.llseek = ext4_llseek,
};

struct file_operations ext4_dir_operations = {
	.readdir = ext4_readdir,
	.llseek  = ext4_llseek,
};

/* inode_operations: lookup */
static struct dentry *ext4_lookup(struct inode *dir, struct dentry *dentry)
{
	struct super_block *sb = dir->i_sb;
	struct ext4_sb_info *sbi = ext4_get_sbi(sb);
	struct ext4_inode *dir_raw = (struct ext4_inode *)dir->i_private;

	if (!dir_raw) {
		warnf("ext4_lookup: no private inode data");
		return PTR(-ENOENT);
	}

	const char *name = dentry->d_name.name;
	int namelen = dentry->d_name.len;

	u32 ino = ext4_dir_lookup(sb, dir_raw, name, namelen);
	if (ino == 0)
		return PTR(-ENOENT);

	/* Get/create VFS inode */
	struct inode *child_inode = inode_get(sb, ino);
	if (!child_inode)
		return PTR(-EIO);

	/* Read the inode data if it's new (i_private == NULL) */
	if (!child_inode->i_private) {
		int ret = ext4_read_inode(sb, child_inode);
		if (ret < 0) {
			inode_put(child_inode);
			return PTR(-EIO);
		}
	}

	/* Set operations based on type */
	if (S_ISDIR(child_inode->i_mode)) {
		child_inode->i_op  = &ext4_dir_inode_ops;
		child_inode->i_fop = &ext4_dir_operations;
	} else if (S_ISREG(child_inode->i_mode)) {
		child_inode->i_op  = NULL;
		child_inode->i_fop = &ext4_file_operations;
	} else if (S_ISLNK(child_inode->i_mode)) {
		child_inode->i_op  = NULL;
		child_inode->i_fop = &ext4_file_operations;
	}

	dentry->d_inode = child_inode;
	return dentry;
}

/* file_operations: read */
static ssize_t ext4_read_file(struct file *file, void *buf,
			      size_t count, loff_t *pos)
{
	struct inode *inode = file->f_inode;
	if (!inode || !inode->i_private)
		return -EIO;

	struct ext4_inode *raw = (struct ext4_inode *)inode->i_private;
	struct super_block *sb = inode->i_sb;

	loff_t p = *pos;
	ssize_t ret = ext4_read_data(sb, raw, p, (u8 *)buf, count);
	if (ret > 0)
		*pos += ret;

	return ret;
}

/* file_operations: write */
static ssize_t ext4_write_file(struct file *file, const void *buf,
			       size_t count, loff_t *pos)
{
	struct inode *inode = file->f_inode;
	if (!inode || !inode->i_private)
		return -EIO;

	struct ext4_inode *raw = (struct ext4_inode *)inode->i_private;
	struct super_block *sb = inode->i_sb;
	struct ext4_sb_info *sbi = ext4_get_sbi(sb);
	u32 block_size = sbi->block_size;
	u32 block_bits = sbi->sb.s_log_block_size + 10;

	loff_t p = *pos;
	const u8 *src = (const u8 *)buf;
	ssize_t total = 0;
	u8 *tmp = kzalloc(block_size);
	if (!tmp)
		return -ENOMEM;

	while (count > 0) {
		u32 lb = p >> block_bits;
		u32 off = p & (block_size - 1);
		u32 n = block_size - off;
		if (n > count)
			n = count;

		/* Only direct blocks supported */
		if (lb >= EXT4_DIRECT_BLOCKS) {
			errorf("ext4_write_file: indirect blocks not supported");
			total = total > 0 ? total : -ENOSPC;
			break;
		}

		u32 pb = raw->i_block[lb];
		if (pb == 0) {
			pb = ext4_alloc_block(sb);
			if (pb == 0) {
				errorf("ext4_write_file: no free blocks");
				total = total > 0 ? total : -ENOSPC;
				break;
			}
			raw->i_block[lb] = pb;
		}

		/* Write the block (whole or partial) */
		u64 byte_off = (u64)pb * block_size;
		if (off == 0 && n == block_size) {
			ext4_write_at(sbi->dev, byte_off, src, block_size);
		} else {
			ext4_read_at(sbi->dev, byte_off, tmp, block_size);
			memcpy(tmp + off, src, n);
			ext4_write_at(sbi->dev, byte_off, tmp, block_size);
		}

		total += n;
		p += n;
		src += n;
		count -= n;
	}

	kfree(tmp);

	if (total > 0) {
		*pos += total;

		/* Update inode size */
		if (p > inode->i_size) {
			inode->i_size = p;
			raw->i_size_lo = (u32)(p & 0xFFFFFFFF);
			raw->i_size_high = (u32)(p >> 32);
		}

		/* Update block count (in 512-byte sectors) */
		u32 alloc_blocks = 0;
		for (int i = 0; i < EXT4_DIRECT_BLOCKS; i++) {
			if (raw->i_block[i] != 0)
				alloc_blocks++;
		}
		raw->i_blocks_lo = alloc_blocks * (block_size / 512);

		inode_dirty(inode);
	}

	return total;
}

/* file_operations: llseek */
static loff_t ext4_llseek(struct file *file, loff_t offset, int whence)
{
	struct inode *inode = file->f_inode;
	loff_t size;

	if (!inode)
		return -EIO;

	size = inode->i_size;

	switch (whence) {
	case SEEK_SET:
		if (offset < 0) return -EINVAL;
		file->f_pos = offset;
		break;
	case SEEK_CUR:
		if (file->f_pos + offset < 0) return -EINVAL;
		file->f_pos += offset;
		break;
	case SEEK_END:
		if (size + offset < 0) return -EINVAL;
		file->f_pos = size + offset;
		break;
	default:
		return -EINVAL;
	}
	return file->f_pos;
}

/* Add a directory entry to the parent directory's data blocks.
 * Standard ext4 approach: find the last entry, shorten its rec_len to its
 * actual size, and place the new entry right after it. */
static int ext4_add_dir_entry(struct inode *dir, u32 ino, const char *name,
                              int namelen, u8 file_type)
{
        struct ext4_sb_info *sbi = ext4_get_sbi(dir->i_sb);
        struct ext4_inode *raw = (struct ext4_inode *)dir->i_private;
        u32 block_size = sbi->block_size;
        u64 dir_size  = dir->i_size;

        /* Calculate new entry's required rec_len (padded to 4 bytes) */
        u16 new_rec_len = (u16)(sizeof(struct ext4_dirent) + namelen);
        new_rec_len = (new_rec_len + 3) & ~3;
        if (new_rec_len < 12)
                new_rec_len = 12;

        /* Read current directory content */
        u8 *buf = kzalloc(block_size);
        if (!buf)
                return -ENOMEM;

        if (dir_size > 0) {
                ssize_t ret = ext4_read_data(dir->i_sb, raw, 0, buf, dir_size);
                if (ret < 0) {
                        kfree(buf);
                        return ret;
                }
        }

        /* Walk entries to find the last one */
        u64 offset   = 0;
        u64 last_off = 0;
        struct ext4_dirent *last_de = NULL;

        while (offset < dir_size) {
                struct ext4_dirent *de = (struct ext4_dirent *)(buf + offset);
                if (de->inode == 0 || de->rec_len == 0)
                        break;
                last_de  = de;
                last_off = offset;
                offset += de->rec_len;
        }

        /* Calculate actual size of last entry */
        u16 last_actual;
        if (last_de) {
                last_actual = (u16)(sizeof(struct ext4_dirent) + last_de->name_len);
                last_actual = (last_actual + 3) & ~3;
                if (last_actual < 12)
                        last_actual = 12;
        } else {
                last_off    = 0;
                last_actual = 0;
        }

        /* Check if new entry fits after the last entry in current block */
        u64 new_off = last_off + last_actual;
        if (new_off + new_rec_len > block_size) {
                kfree(buf);
                return -ENOSPC;
        }

        /* Shorten last entry's rec_len to its actual size */
        if (last_de) {
                last_de->rec_len = last_actual;
        }

        /* Place new entry */
        struct ext4_dirent *new_de = (struct ext4_dirent *)(buf + new_off);
        new_de->inode     = ino;
        new_de->name_len  = (u8)namelen;
        new_de->file_type = file_type;
        new_de->rec_len   = (u16)(block_size - new_off);
        memcpy(new_de->name, name, namelen);

        u64 new_size = new_off + new_de->rec_len;

        /* Write back — locate the physical block containing new_off */
        u32 logical_block = (u32)(new_off / block_size);
        u32 phys_block;
        if (raw->i_flags & EXT4_EXTENTS_FL) {
                phys_block = ext4_find_extent_block(sbi, raw, logical_block);
        } else {
                if (logical_block < EXT4_DIRECT_BLOCKS)
                        phys_block = raw->i_block[logical_block];
                else
                        phys_block = ~0U;
        }

        if (phys_block != ~0U) {
                u64 byte_off = (u64)phys_block * block_size;
                ext4_write_at(sbi->dev, byte_off, buf, block_size);
        }

        /* Update directory inode size */
        dir->i_size = new_size;
        raw->i_size_lo = (u32)(new_size & 0xFFFFFFFF);
        raw->i_size_high = (u32)(new_size >> 32);
        inode_dirty(dir);

        kfree(buf);
        return 0;
}

/* inode_operations: unlink (remove a directory entry) */
static int ext4_unlink(struct inode *dir, struct dentry *dentry)
{
        struct super_block *sb = dir->i_sb;
        struct ext4_sb_info *sbi = ext4_get_sbi(sb);
        struct ext4_inode *raw = (struct ext4_inode *)dir->i_private;
        u32 block_size = sbi->block_size;
        u64 dir_size = dir->i_size;

        const char *name = dentry->d_name.name;
        int namelen = dentry->d_name.len;

        /* Read the full directory content */
        u8 *buf = kzalloc(dir_size < block_size ? block_size : (u32)dir_size);
        if (!buf)
                return -ENOMEM;

        ssize_t ret = ext4_read_data(sb, raw, 0, buf, dir_size);
        if (ret < 0) {
                kfree(buf);
                return ret;
        }

        /* Walk entries to find the target */
        u64 offset = 0;
        int found = 0;
        u64 entry_off = 0;

        while (offset < dir_size) {
                struct ext4_dirent *de = (struct ext4_dirent *)(buf + offset);
                if (de->inode == 0 || de->rec_len == 0)
                        break;

                if (de->name_len == (u8)namelen &&
                    memcmp(de->name, name, namelen) == 0) {
                        found = 1;
                        entry_off = offset;
                        break;
                }
                offset += de->rec_len;
        }

        if (!found) {
                kfree(buf);
                return -ENOENT;
        }

        /* Mark the entry as deleted */
        struct ext4_dirent *de = (struct ext4_dirent *)(buf + entry_off);
        u32 deleted_ino = de->inode;
        de->inode = 0;

        /* Write back the directory block */
        u32 logical_block = (u32)(entry_off / block_size);
        u32 phys_block;
        if (raw->i_flags & EXT4_EXTENTS_FL) {
                phys_block = ext4_find_extent_block(sbi, raw, logical_block);
        } else {
                if (logical_block < EXT4_DIRECT_BLOCKS)
                        phys_block = raw->i_block[logical_block];
                else
                        phys_block = ~0U;
        }

        if (phys_block == ~0U) {
                kfree(buf);
                return -EIO;
        }

        u64 byte_off = (u64)phys_block * block_size;
        ext4_write_at(sbi->dev, byte_off, buf + logical_block * block_size,
                      block_size);

        kfree(buf);

        /* Decrement target inode's i_links_count */
        u32 ino = deleted_ino;
        u32 bg_idx = (ino - 1) / sbi->inodes_per_group;
        u32 bg_ino_idx = (ino - 1) % sbi->inodes_per_group;
        struct ext4_group_desc bgd;

        if (ext4_read_group_desc(sbi, bg_idx, &bgd) == 0) {
                u64 inode_table_off = (u64)bgd.bg_inode_table_lo * block_size;
                u64 inode_disk_off = inode_table_off +
                                     (u64)bg_ino_idx * sbi->inode_size;

                struct ext4_inode target_raw;
                int r = ext4_read_at(sbi->dev, inode_disk_off, &target_raw,
                                     sizeof(target_raw));
                if (r == 0) {
                        if (target_raw.i_links_count > 0)
                                target_raw.i_links_count--;
                        ext4_write_at(sbi->dev, inode_disk_off, &target_raw,
                                      sizeof(target_raw));
                }
        }

        /* Update parent inode's mtime and mark dirty */
        dir->i_mtime = 0;
        inode_dirty(dir);

        return 0;
}

/* inode_operations: create (regular file) */
static int ext4_create(struct inode *dir, struct dentry *dentry, umode_t mode)
{
        struct super_block *sb = dir->i_sb;
        struct ext4_sb_info *sbi = ext4_get_sbi(sb);
        u32 block_size = sbi->block_size;

        /* Allocate inode */
        u32 ino = ext4_alloc_inode_from_disk(sb);
        if (ino == 0)
                return -ENOSPC;

        /* Build the raw inode on disk (regular file, size=0, no data blocks) */
        struct ext4_inode raw_inode;
        memset(&raw_inode, 0, sizeof(raw_inode));
        raw_inode.i_mode       = EXT4_S_IFREG | (mode & 0777);
        raw_inode.i_uid        = 0;
        raw_inode.i_gid        = 0;
        raw_inode.i_size_lo    = 0;
        raw_inode.i_size_high  = 0;
        raw_inode.i_links_count = 1;
        raw_inode.i_blocks_lo  = 0;
        raw_inode.i_flags      = 0;

        /* Write raw inode to disk */
        u32 bg_idx     = (ino - 1) / sbi->inodes_per_group;
        u32 bg_ino_idx = (ino - 1) % sbi->inodes_per_group;
        struct ext4_group_desc bgd;
        if (ext4_read_group_desc(sbi, bg_idx, &bgd) == 0) {
                u64 inode_table_off = (u64)bgd.bg_inode_table_lo * block_size;
                u64 inode_off = inode_table_off + (u64)bg_ino_idx * sbi->inode_size;
                ext4_write_at(sbi->dev, inode_off, &raw_inode, sizeof(raw_inode));
        }

        /* Add entry to parent directory */
        ext4_add_dir_entry(dir, ino, dentry->d_name.name, dentry->d_name.len,
                           EXT4_FT_REG_FILE);

        /* Create VFS inode and associate with dentry */
        struct inode *child_inode = inode_get(sb, ino);
        if (!child_inode)
                return -ENOMEM;

        if (!child_inode->i_private) {
                int ret = ext4_read_inode(sb, child_inode);
                if (ret < 0) {
                        inode_put(child_inode);
                        return ret;
                }
        }

        child_inode->i_op   = NULL;
        child_inode->i_fop  = &ext4_file_operations;
        child_inode->i_sb   = sb;
        child_inode->i_mode = S_IFREG | (mode & 0777);
        child_inode->i_nlink = 1;

        dentry->d_inode = child_inode;
        dentry->d_sb    = sb;

        return 0;
}

/* inode_operations: mkdir */
static int ext4_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode)
{
	struct super_block *sb = dir->i_sb;
	struct ext4_sb_info *sbi = ext4_get_sbi(sb);
	u32 block_size = sbi->block_size;

	/* Allocate inode */
	u32 ino = ext4_alloc_inode_from_disk(sb);
	if (ino == 0)
		return -ENOSPC;

	/* Allocate a data block for directory contents (. and ..) */
	u32 phys_block = ext4_alloc_block(sb);
	if (phys_block == 0) {
		/* TODO: free inode back */
		return -ENOSPC;
	}

	/* Build the raw inode on disk */
	struct ext4_inode raw_inode;
	memset(&raw_inode, 0, sizeof(raw_inode));
	raw_inode.i_mode  = EXT4_S_IFDIR | (mode & 0777);
	raw_inode.i_uid   = 0;
	raw_inode.i_gid   = 0;
	raw_inode.i_size_lo = block_size;
	raw_inode.i_size_high = 0;
	raw_inode.i_links_count = 1; /* no .. yet, will be 2 after parent update */
	raw_inode.i_blocks_lo = (u32)(sbi->block_size / 512);
	raw_inode.i_flags     = 0;
	raw_inode.i_block[0]  = phys_block;

	/* Write raw inode to disk */
	u32 bg_idx     = (ino - 1) / sbi->inodes_per_group;
	u32 bg_ino_idx = (ino - 1) % sbi->inodes_per_group;
	struct ext4_group_desc bgd;
	if (ext4_read_group_desc(sbi, bg_idx, &bgd) == 0) {
		u64 inode_table_off = (u64)bgd.bg_inode_table_lo * block_size;
		u64 inode_off = inode_table_off + (u64)bg_ino_idx * sbi->inode_size;
		ext4_write_at(sbi->dev, inode_off, &raw_inode, sizeof(raw_inode));
	}

	/* Build directory content: "." and ".." entries */
	u8 *dir_data = kzalloc(block_size);
	if (!dir_data)
		return -ENOMEM;

	u8 *p = dir_data;

	/* "." entry - points to self */
	struct ext4_dirent *dot = (struct ext4_dirent *)p;
	dot->inode     = ino;
	dot->name_len  = 1;
	dot->file_type = EXT4_FT_DIR;
	dot->rec_len   = sizeof(struct ext4_dirent) + 4; /* 12 bytes, padded to 12 */
	dot->name[0]   = '.';

	/* ".." entry - points to parent */
	struct ext4_dirent *dotdot = (struct ext4_dirent *)(p + dot->rec_len);
	dotdot->inode     = dir->i_ino;
	dotdot->name_len  = 2;
	dotdot->file_type = EXT4_FT_DIR;
	dotdot->rec_len   = block_size - dot->rec_len; /* Takes remaining space */
	dotdot->name[0]   = '.';
	dotdot->name[1]   = '.';

	/* Write directory content to the allocated block */
	u64 byte_off = (u64)phys_block * block_size;
	ext4_write_at(sbi->dev, byte_off, dir_data, block_size);
	kfree(dir_data);

	/* Add entry to parent directory */
	ext4_add_dir_entry(dir, ino, dentry->d_name.name, dentry->d_name.len,
			   EXT4_FT_DIR);

	/* Update parent inode */
	struct ext4_inode *parent_raw = (struct ext4_inode *)dir->i_private;
	if (parent_raw)
		parent_raw->i_links_count++;
	inode_dirty(dir);

	/* Create VFS inode and associate with dentry */
	struct inode *child_inode = inode_get(sb, ino);
	if (!child_inode)
		return -ENOMEM;

	if (!child_inode->i_private) {
		int ret = ext4_read_inode(sb, child_inode);
		if (ret < 0) {
			inode_put(child_inode);
			return ret;
		}
	}

	child_inode->i_op  = &ext4_dir_inode_ops;
	child_inode->i_fop = &ext4_dir_operations;
	child_inode->i_sb  = sb;
	child_inode->i_mode = S_IFDIR | (mode & 0777);
	child_inode->i_nlink = 2;

	dentry->d_inode = child_inode;
	dentry->d_sb    = sb;

	return 0;
}

/* file_operations: readdir */
static int ext4_readdir(struct file *file, struct dirent *dirent_buf,
			size_t count)
{
	struct inode *inode = file->f_inode;
	if (!inode || !inode->i_private)
		return -EINVAL;

	struct ext4_inode *raw = (struct ext4_inode *)inode->i_private;
	struct super_block *sb = inode->i_sb;
	struct ext4_sb_info *sbi = ext4_get_sbi(sb);

	u64 dir_size = inode->i_size;
	loff_t pos   = file->f_pos;

	u64 limit = dir_size;
	if (pos >= (loff_t)limit)
		return 0;

	/* 读目录内容 */
	u8 *buf = kzalloc(limit < 4096 ? 4096 : limit);
	if (!buf)
		return -ENOMEM;
	ssize_t ret = ext4_read_data(sb, raw, 0, buf, dir_size);
	if (ret < 0) {
		kfree(buf);
		return ret;
	}


	size_t written = 0;
	u64 off = (u64)pos;

	while (off < limit && written + sizeof(struct dirent) <= count) {
		struct ext4_dirent *de = (struct ext4_dirent *)(buf + off);
		if (de->rec_len == 0)
			break;

		if (de->inode == 0) {
			off += de->rec_len;
			continue;
		}

		if (de->name_len > 0) {
			struct dirent *d =
				(struct dirent *)((u8 *)dirent_buf + written);
			d->d_ino  = de->inode;
			d->d_off  = off;
			d->d_reclen = sizeof(struct dirent);
			d->d_type = de->file_type;
			u8 len = de->name_len;
			if (len > 255) len = 255;
			memcpy(d->d_name, de->name, len);
			d->d_name[len] = '\0';
			written += sizeof(struct dirent);
		}
		off += de->rec_len;
	}

	file->f_pos = off;
	kfree(buf);
	return written;
}

/* ─────────────────────────────────────────────
 * Mount / filesystem type
 * ───────────────────────────────────────────── */

struct super_block *ext4_mount(struct file_system_type *fs_type,
			       dev_t dev, void *data)
{
	struct super_block *sb = super_alloc(fs_type, dev);
	if (IS_ERR(sb))
		return sb;

	sb->s_op = &ext4_super_ops;

	int ret = ext4_fill_super(sb, dev, data);
	if (ret < 0) {
		super_free(sb);
		return PTR(ret);
	}

	/* Read root inode (inode 2 is always the root directory in ext4) */
	struct inode *root_inode = inode_alloc(sb);
	if (!root_inode) {
		ext4_put_super(sb);
		super_free(sb);
		return PTR(-ENOMEM);
	}
	root_inode->i_ino = 2;  /* EXT4_ROOT_INO */

	ret = ext4_read_inode(sb, root_inode);
	if (ret < 0) {
		inode_free(root_inode);
		ext4_put_super(sb);
		super_free(sb);
		return PTR(ret);
	}

	root_inode->i_sb  = sb;
	root_inode->i_op  = &ext4_dir_inode_ops;
	root_inode->i_fop = &ext4_dir_operations;

	/* Create root dentry */
	struct dentry *root_dentry = dentry_root(sb);
	if (IS_ERR(root_dentry)) {
		int err = PTR_ERR(root_dentry);
		inode_free(root_inode);
		ext4_put_super(sb);
		super_free(sb);
		return PTR(err);
	}
	root_dentry->d_inode = root_inode;
	sb->s_root = root_dentry;

	return sb;
}

void ext4_kill_sb(struct super_block *sb)
{
	ext4_put_super(sb);
	if (sb->s_root) {
		dentry_free(sb->s_root);
		sb->s_root = NULL;
	}
}

/* Filesystem type registration */
static struct file_system_type ext4_fs_type = {
	.name    = "ext4",
	.mount   = ext4_mount,
	.kill_sb = ext4_kill_sb,
};

int ext4_init_fs(void)
{
	int ret = register_filesystem(&ext4_fs_type);
	if (ret < 0) {
		panic("ext4: failed to register filesystem");
		return ret;
	}
	return 0;
}