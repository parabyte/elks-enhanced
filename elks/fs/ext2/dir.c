/*
 *  linux/fs/ext2/dir.c
 *
 *  ext2 directory handling functions
 */

#include <linuxmt/types.h>
#include <linuxmt/config.h>
#include <linuxmt/string.h>
#include <linuxmt/errno.h>
#include <linuxmt/kernel.h>
#include <linuxmt/fs.h>
#include <linuxmt/ext2_fs.h>
#include <linuxmt/stat.h>
#include <linuxmt/limits.h>

#ifdef CONFIG_EXT2_DEBUG
#define ext2_dbg printk
#else
#define ext2_dbg(...)
#endif

static size_t ext2_dir_read(struct inode *inode, struct file *filp,
			    char *buf, size_t count)
{
	(void)inode;
	(void)filp;
	(void)buf;
	(void)count;
	return -EISDIR;
}

static int ext2_readdir(struct inode *inode, register struct file *filp,
			char *dirent, filldir_t filldir)
{
	register struct buffer_head *bh;
	struct ext2_dir_entry *de;
	block_t block;
	unsigned short offset;
	unsigned short reclen;
	unsigned short namelen;

	if (!inode || !S_ISDIR(inode->i_mode))
		return -EBADF;

	block = (block_t)(filp->f_pos >> BLOCK_SIZE_BITS);
	while (filp->f_pos < (loff_t)inode->i_size) {
		offset = (unsigned short)filp->f_pos & (BLOCK_SIZE - 1);
		bh = ext2_bread(inode, block++, 0);
		if (!bh) {
			filp->f_pos += (BLOCK_SIZE - offset);
			continue;
		}
		map_buffer(bh);

		while (offset < BLOCK_SIZE && filp->f_pos < (loff_t)inode->i_size) {
			de = (struct ext2_dir_entry *)(bh->b_data + offset);
			if (!ext2_dirent_ok(de, offset)) {
				ext2_dbg("EXT2: readdir bad dentry ino %lu pos %lu\n",
					 (unsigned long)inode->i_ino,
					 (unsigned long)filp->f_pos);
				unmap_brelse(bh);
				return -EIO;
			}
			reclen = de->rec_len;
			namelen = (unsigned char)de->name_len;

			if (de->inode) {
				if (namelen > MAXNAMLEN)
					namelen = MAXNAMLEN;
				filldir(dirent, de->name, namelen,
					filp->f_pos, de->inode);
				filp->f_pos += reclen;
				unmap_brelse(bh);
				return 0;
			}

			offset += reclen;
			filp->f_pos += reclen;
		}
		unmap_brelse(bh);
	}
	return 0;
}

static struct file_operations ext2_dir_operations = {
	NULL,
	ext2_dir_read,
	NULL,
	ext2_readdir,
	NULL,
	NULL,
	NULL,
	NULL
};

struct inode_operations ext2_dir_inode_operations = {
	&ext2_dir_operations,
	ext2_create,
	ext2_lookup,
	ext2_link,
	ext2_unlink,
	ext2_symlink,
	ext2_mkdir,
	ext2_rmdir,
	ext2_mknod,
	NULL,
	NULL,
	NULL,
	ext2_truncate
};
