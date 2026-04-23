/*
 *  linux/fs/ext2/file.c
 *
 *  ext2 regular file handling primitives
 */

#include <linuxmt/types.h>
#include <linuxmt/fs.h>
#include <linuxmt/ext2_fs.h>
#include <linuxmt/stat.h>

static struct file_operations ext2_file_operations = {
	NULL,
	block_read,
	block_write,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL
};

struct inode_operations ext2_file_inode_operations = {
	&ext2_file_operations,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	ext2_getblk,
	ext2_truncate
};
