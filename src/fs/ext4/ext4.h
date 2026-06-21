#pragma once

#include <misc/stddef.h>
#include <hal/blk.h>

/*
 * EXT4 on-disk structures (little-endian)
 * Minimum read-only implementation for the OS competition
 */

/* Superblock — 1024 bytes, at byte offset 1024 from partition start */
struct ext4_superblock {
	u32 s_inodes_count;          /* 0x00 */
	u32 s_blocks_count_lo;       /* 0x04 */
	u32 s_r_blocks_count_lo;     /* 0x08 */
	u32 s_free_blocks_count_lo;  /* 0x0C */
	u32 s_free_inodes_count_lo;  /* 0x10 */
	u32 s_first_data_block;      /* 0x14 */
	u32 s_log_block_size;        /* 0x18: block_size = 1024 << this */
	u32 s_log_cluster_size;      /* 0x1C */
	u32 s_blocks_per_group;      /* 0x20 */
	u32 s_clusters_per_group;    /* 0x24 */
	u32 s_inodes_per_group;      /* 0x28 */
	u32 s_mtime;                 /* 0x2C */
	u32 s_wtime;                 /* 0x30 */
	u16 s_mnt_count;             /* 0x34 */
	u16 s_max_mnt_count;         /* 0x36 */
	u16 s_magic;                 /* 0x38: must be 0xEF53 */
	u16 s_state;                 /* 0x3A */
	u16 s_errors;                /* 0x3C */
	u16 s_minor_rev_level;       /* 0x3E */
	u32 s_lastcheck;             /* 0x40 */
	u32 s_checkinterval;         /* 0x44 */
	u32 s_creator_os;            /* 0x48 */
	u32 s_rev_level;             /* 0x4C */
	u16 s_def_resuid;            /* 0x50 */
	u16 s_def_resgid;            /* 0x52 */
	u32 s_first_ino;             /* 0x54 */
	u16 s_inode_size;            /* 0x58 */
	u16 s_block_group_nr;        /* 0x5A */
	u32 s_feature_compat;        /* 0x5C */
	u32 s_feature_incompat;      /* 0x60 */
	u32 s_feature_ro_compat;     /* 0x64 */
	u8  s_uuid[16];              /* 0x68 */
	char s_volume_name[16];      /* 0x78 */
	char s_last_mounted[64];     /* 0x88 */
	u32 s_algorithm_usage_bitmap;/* 0xC8 */
	u8  s_def_hash_version;      /* 0xCC */
	u8  s_reserved_char;         /* 0xCD */
	u16 s_default_mount_opts;    /* 0xCE */
	u32 s_first_meta_bg;         /* 0xD0 */
	u32 s_mkfs_time;             /* 0xD4 */
	u32 s_jnl_blocks[17];        /* 0xD8 */
	/* 0x124-0x3FF: more fields (not needed for basic read) */
} __attribute__((packed));

#define EXT4_SUPERBLOCK_OFFSET 1024
#define EXT4_SUPERBLOCK_SIZE  1024
#define EXT4_MAGIC            0xEF53

/* Feature flags */
#define EXT4_FEATURE_INCOMPAT_EXTENTS   0x0040
#define EXT4_FEATURE_INCOMPAT_64BIT     0x0080
#define EXT4_FEATURE_INCOMPAT_FLEX_BG   0x0200

/* Block Group Descriptor — 32 bytes each */
struct ext4_group_desc {
	u32 bg_block_bitmap_lo;       /* 0x00 */
	u32 bg_inode_bitmap_lo;       /* 0x04 */
	u32 bg_inode_table_lo;        /* 0x08 */
	u16 bg_free_blocks_count_lo;  /* 0x0C */
	u16 bg_free_inodes_count_lo;  /* 0x0E */
	u16 bg_used_dirs_count_lo;    /* 0x10 */
	u16 bg_flags;                 /* 0x12 */
	u32 bg_exclude_bitmap_lo;     /* 0x14 */
	u16 bg_block_bitmap_csum_lo;  /* 0x18 */
	u16 bg_inode_bitmap_csum_lo;  /* 0x1A */
	u16 bg_itable_unused_lo;      /* 0x1C */
	u16 bg_checksum;              /* 0x1E */
} __attribute__((packed));

/* Inode — 128 or 256 bytes */
struct ext4_inode {
	u16 i_mode;                  /* 0x00 */
	u16 i_uid;                   /* 0x02 */
	u32 i_size_lo;               /* 0x04 */
	u32 i_atime;                 /* 0x08 */
	u32 i_ctime;                 /* 0x0C */
	u32 i_mtime;                 /* 0x10 */
	u32 i_dtime;                 /* 0x14 */
	u16 i_gid;                   /* 0x16 */
	u16 i_links_count;           /* 0x18 */
	u32 i_blocks_lo;             /* 0x1C: 512-byte sectors */
	u32 i_flags;                 /* 0x20 */
	u32 i_osd1;                  /* 0x24 */
	u32 i_block[15];             /* 0x28: 60 bytes of block/extent data */
	u32 i_generation;            /* 0x64 */
	u32 i_file_acl_lo;           /* 0x68 */
	u32 i_size_high;             /* 0x6C */
	u32 i_obso_faddr;            /* 0x70 */
	u8  i_osd2[12];              /* 0x74 */
} __attribute__((packed));

#define EXT4_INODE_SIZE_DEFAULT 128
#define EXT4_EXTENTS_FL         0x00080000
#define EXT4_N_BLOCKS           15
#define EXT4_DIRECT_BLOCKS      12

/* Inline data threshold — small files may use i_block directly */
#define EXT4_MIN_INLINE_DATA    60

/* File type in directory entry */
#define EXT4_FT_UNKNOWN  0
#define EXT4_FT_REG_FILE 1
#define EXT4_FT_DIR      2
#define EXT4_FT_SYMLINK  7

/* Inode mode bits */
#define EXT4_S_IFMT   0xF000
#define EXT4_S_IFREG  0x8000
#define EXT4_S_IFDIR  0x4000
#define EXT4_S_IFLNK  0xA000

/* Extent tree structures */
struct ext4_extent_header {
	u16 eh_magic;                /* 0xF30A */
	u16 eh_entries;              /* number of valid entries */
	u16 eh_max;                  /* capacity */
	u16 eh_depth;                /* 0=leaf, >0=index node */
	u32 eh_generation;
} __attribute__((packed));

#define EXT4_EXTENT_MAGIC 0xF30A

struct ext4_extent {
	u32 ee_block;                /* first logical block */
	u16 ee_len;                  /* number of blocks (clobbered by ee_start_hi) */
	u16 ee_start_hi;             /* high 16 bits of physical block */
	u32 ee_start_lo;             /* low 32 bits of physical block */
} __attribute__((packed));

struct ext4_extent_idx {
	u32 ei_block;                /* logical block covered */
	u32 ei_leaf_lo;              /* physical block of child node (low) */
	u16 ei_leaf_hi;              /* physical block of child node (high) */
	u16 ei_unused;
} __attribute__((packed));

/* Directory entry (ext4_dir_entry_2) */
struct ext4_dirent {
	u32 inode;                   /* inode number */
	u16 rec_len;                 /* record length (to next entry) */
	u8  name_len;                /* name length */
	u8  file_type;               /* file type */
	char name[0];                /* variable-length name */
} __attribute__((packed));

/* EXT4 private per-superblock data */
struct ext4_sb_info {
	struct ext4_superblock sb;   /* raw on-disk superblock */
	u32 block_size;              /* filesystem block size */
	u32 blocks_per_group;
	u32 inodes_per_group;
	u32 inode_size;
	u32 inodes_per_block;
	u32 groups_count;
	u32 desc_per_block;
	u32 desc_size;               /* on-disk descriptor size: 32 or 64 */
	dev_t dev;
};

/* Functions called from ext4_init */
int ext4_fill_super(struct super_block *vsb, dev_t dev, void *data);

/* VFS interface functions */
struct super_block *ext4_mount(struct file_system_type *fs_type,
			       dev_t dev, void *data);
void ext4_kill_sb(struct super_block *sb);
int ext4_init_fs(void);