#ifndef __LINUXMT_EXT2_FS_H
#define __LINUXMT_EXT2_FS_H

/*
 * Minimal ext2 support for ELKS.
 * Current implementation targets ext2 rev0, 1KiB blocks, with
 * experimental read-write support.
 */

#include <linuxmt/types.h>

#define EXT2_ROOT_INO                   2
#define EXT2_SUPER_MAGIC                0xEF53
#define EXT2_GOOD_OLD_REV               0
#define EXT2_GOOD_OLD_INODE_SIZE        128
#define EXT2_NAME_LEN                   255
#define EXT2_LINK_MAX                   128
#define EXT2_VALID_FS                   0x0001
#define EXT2_ERROR_FS                   0x0002

#define EXT2_NDIR_BLOCKS                12
#define EXT2_IND_BLOCK                  EXT2_NDIR_BLOCKS
#define EXT2_DIND_BLOCK                 (EXT2_IND_BLOCK + 1)
#define EXT2_TIND_BLOCK                 (EXT2_DIND_BLOCK + 1)
#define EXT2_N_BLOCKS                   (EXT2_TIND_BLOCK + 1)
#define EXT2_ADDR_PER_BLOCK             (BLOCK_SIZE / sizeof(__u32))
#define EXT2_INODES_PER_BLOCK           (BLOCK_SIZE / EXT2_GOOD_OLD_INODE_SIZE)
#define EXT2_DIR_REC_LEN(name_len)      (((name_len) + 11) & ~3)

#define EXT2_S_IFSOCK                   0xC000
#define EXT2_S_IFLNK                    0xA000
#define EXT2_S_IFREG                    0x8000
#define EXT2_S_IFBLK                    0x6000
#define EXT2_S_IFDIR                    0x4000
#define EXT2_S_IFCHR                    0x2000
#define EXT2_S_IFIFO                    0x1000

struct super_block;
struct inode;
struct statfs;
struct buffer_head;

struct ext2_super_block {
	__u32	s_inodes_count;
	__u32	s_blocks_count;
	__u32	s_r_blocks_count;
	__u32	s_free_blocks_count;
	__u32	s_free_inodes_count;
	__u32	s_first_data_block;
	__u32	s_log_block_size;
	__s32	s_log_frag_size;
	__u32	s_blocks_per_group;
	__u32	s_frags_per_group;
	__u32	s_inodes_per_group;
	__u32	s_mtime;
	__u32	s_wtime;
	__u16	s_mnt_count;
	__s16	s_max_mnt_count;
	__u16	s_magic;
	__u16	s_state;
	__u16	s_errors;
	__u16	s_minor_rev_level;
	__u32	s_lastcheck;
	__u32	s_checkinterval;
	__u32	s_creator_os;
	__u32	s_rev_level;
	__u16	s_def_resuid;
	__u16	s_def_resgid;
} __attribute__((packed));

struct ext2_group_desc {
	__u32	bg_block_bitmap;
	__u32	bg_inode_bitmap;
	__u32	bg_inode_table;
	__u16	bg_free_blocks_count;
	__u16	bg_free_inodes_count;
	__u16	bg_used_dirs_count;
	__u16	bg_pad;
	__u32	bg_reserved[3];
} __attribute__((packed));

struct ext2_inode {
	__u16	i_mode;
	__u16	i_uid;
	__u32	i_size;
	__u32	i_atime;
	__u32	i_ctime;
	__u32	i_mtime;
	__u32	i_dtime;
	__u16	i_gid;
	__u16	i_links_count;
	__u32	i_blocks;
	__u32	i_flags;
	__u32	i_osd1;
	__u32	i_block[EXT2_N_BLOCKS];
	__u32	i_generation;
	__u32	i_file_acl;
	__u32	i_dir_acl;
	__u32	i_faddr;
	__u8	i_osd2[12];
} __attribute__((packed));

struct ext2_dir_entry {
	__u32	inode;
	__u16	rec_len;
	__u16	name_len;
	char	name[];
} __attribute__((packed));

#ifdef __KERNEL__

/*
 * Directory entry sanity (1KiB blocks only).  Defined in fs/ext2/namei.c.
 */
int ext2_dirent_ok(struct ext2_dir_entry *de, unsigned short off);

#define EXT2_SB(sb)                     (&((sb)->u.ext2_sb))

extern struct super_block *ext2_read_super(register struct super_block *,
					   char *, int);
extern void ext2_read_inode(register struct inode *);
extern void ext2_put_super(register struct super_block *);
extern void ext2_statfs(struct super_block *, struct statfs *, int);
extern int ext2_remount(register struct super_block *, int *, char *);

extern struct buffer_head *ext2_getblk(struct inode *, block_t, int);
extern struct buffer_head *ext2_bread(struct inode *, block_t, int);
extern struct buffer_head *ext2_get_inode(struct inode *,
					  struct ext2_inode **);
extern block32_t ext2_bmap(struct inode *, block_t);
extern struct inode *ext2_new_inode(struct inode *, mode_t);
extern void ext2_truncate(register struct inode *);

extern int ext2_lookup(struct inode *, const char *, size_t,
		       struct inode **);
extern int ext2_create(struct inode *, const char *, size_t, mode_t,
		       struct inode **);
extern int ext2_link(struct inode *, char *, size_t, struct inode *);
extern int ext2_symlink(struct inode *, char *, size_t, char *);
extern int ext2_mkdir(struct inode *, const char *, size_t, mode_t);
extern int ext2_mknod(struct inode *, const char *, size_t, mode_t, int);
extern int ext2_unlink(struct inode *, char *, size_t);
extern int ext2_rmdir(struct inode *, char *, size_t);

extern struct inode_operations ext2_file_inode_operations;
extern struct inode_operations ext2_dir_inode_operations;
extern struct inode_operations ext2_symlink_inode_operations;

#endif

#endif
