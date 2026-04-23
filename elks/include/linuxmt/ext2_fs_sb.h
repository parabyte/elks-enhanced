#ifndef __LINUXMT_EXT2_FS_SB_H
#define __LINUXMT_EXT2_FS_SB_H

#include <linuxmt/ext2_fs.h>

struct ext2_sb_info {
	__u32	s_inodes_count;
	__u32	s_blocks_count;
	__u32	s_r_blocks_count;
	__u32	s_free_blocks_count;
	__u32	s_free_inodes_count;
	__u32	s_first_data_block;
	__u32	s_blocks_per_group;
	__u32	s_inodes_per_group;
	__u16	s_groups_count;
	__u16	s_desc_per_block;
	__u16	s_mount_state;
	__u16	s_state;
	__u16	s_mnt_count;
	/* Allocation locality: start scan at last successful group */
	__u16	s_last_block_group;
	__u16	s_last_inode_group;
	/* Hot-path cache: inode table location for the last inode group read. */
	__u32	s_inode_table_block;
	__u16	s_inode_table_group;
	__u8	s_inode_table_valid;
};

#endif
