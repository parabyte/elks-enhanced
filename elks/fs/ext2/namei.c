/*
 *  linux/fs/ext2/namei.c
 *
 *  ext2 name lookup and basic directory mutation.
 */

#include <linuxmt/types.h>
#include <linuxmt/config.h>
#include <linuxmt/sched.h>
#include <linuxmt/ext2_fs.h>
#include <linuxmt/kernel.h>
#include <linuxmt/string.h>
#include <linuxmt/stat.h>
#include <linuxmt/errno.h>
#include <linuxmt/mm.h>

#include <arch/segment.h>

#ifdef CONFIG_EXT2_DEBUG
#define ext2_dbg printk
#else
#define ext2_dbg(...)
#endif

int ext2_dirent_ok(struct ext2_dir_entry *de, unsigned short off)
{
	unsigned short rl = de->rec_len;
	unsigned int nl = de->name_len;

	if (rl < 8 || rl > (unsigned short)(1024 - off) || (rl & 3))
		return 0;
	if (nl > EXT2_NAME_LEN)
		return 0;
	if (de->inode && EXT2_DIR_REC_LEN(nl) > rl)
		return 0;
	return 1;
}

static int ext2_namecompare(size_t len, size_t max,
			    const char *name, register char *buf)
{
	return (len == max) && !fs_memcmp(name, buf, len);
}

static int ext2_match(size_t len, const char *name,
		      register struct ext2_dir_entry *de)
{
	size_t namelen;

	if (!de->inode)
		return 0;
	namelen = (unsigned char)de->name_len;
	if (!len && de->name[0] == '.' && namelen == 1)
		return 1;
	return ext2_namecompare(len, namelen, name, de->name);
}

static struct buffer_head *ext2_find_entry(register struct inode *dir,
					   const char *name, size_t len,
					   struct ext2_dir_entry **res_de)
{
	register struct buffer_head *bh;
	struct ext2_dir_entry *de;
	block_t block;
	loff_t pos;
	unsigned short offset;
	unsigned short reclen;

	*res_de = NULL;
	if (!dir || !S_ISDIR(dir->i_mode))
		return NULL;

	block = 0;
	pos = 0;
	while (pos < (loff_t)dir->i_size) {
		bh = ext2_bread(dir, block++, 0);
		if (!bh) {
			pos += BLOCK_SIZE;
			continue;
		}
		map_buffer(bh);
		offset = 0;

		while (offset < BLOCK_SIZE && pos < (loff_t)dir->i_size) {
			de = (struct ext2_dir_entry *)(bh->b_data + offset);
			if (!ext2_dirent_ok(de, offset)) {
				ext2_dbg("EXT2: bad dentry ino %lu pos %lu\n",
					 (unsigned long)dir->i_ino,
					 (unsigned long)pos);
				unmap_brelse(bh);
				return NULL;
			}
			reclen = de->rec_len;
			if (ext2_match(len, name, de)) {
				*res_de = de;
				return bh;
			}
			offset += reclen;
			pos += reclen;
		}
		unmap_brelse(bh);
	}
	return NULL;
}

static void ext2_copy_name(char *dst, const char *name, size_t len, int fromfs)
{
	if (fromfs)
		memcpy_fromfs(dst, (void *)name, len);
	else
		memcpy(dst, name, len);
}

static int ext2_add_entry(register struct inode *dir, const char *name,
			  size_t len, ino_t ino, int fromfs)
{
	register struct buffer_head *bh;
	struct ext2_dir_entry *de;
	struct ext2_dir_entry *newde;
	loff_t pos;
	unsigned short offset;
	unsigned short reclen;
	unsigned short need;
	unsigned short actual;
	unsigned short slot_offset;
	unsigned short slot_actual;
	block_t block;
	block_t slot_block;
	int slot_split;
	int slot_found;

	if (len > EXT2_NAME_LEN)
		return -ENAMETOOLONG;
	need = EXT2_DIR_REC_LEN(len);

	block = 0;
	pos = 0;
	slot_found = 0;
	slot_split = 0;
	slot_block = 0;
	slot_offset = 0;
	slot_actual = 0;
	while (pos < (loff_t)dir->i_size) {
		bh = ext2_bread(dir, block, 0);
		block++;
		if (!bh) {
			pos += BLOCK_SIZE;
			continue;
		}
		map_buffer(bh);
		offset = 0;
		while (offset < BLOCK_SIZE && pos < (loff_t)dir->i_size) {
			de = (struct ext2_dir_entry *)(bh->b_data + offset);
			if (!ext2_dirent_ok(de, offset)) {
				unmap_brelse(bh);
				return -EIO;
			}
			reclen = de->rec_len;
			if (ext2_match(len, name, de)) {
				unmap_brelse(bh);
				return -EEXIST;
			}
			if (!de->inode) {
				if (reclen >= need && !slot_found) {
					slot_found = 1;
					slot_block = block - 1;
					slot_offset = offset;
					slot_split = 0;
				}
			} else {
				actual = EXT2_DIR_REC_LEN(de->name_len);
				if (reclen < actual) {
					unmap_brelse(bh);
					return -EIO;
				}
				if (reclen - actual >= need && !slot_found) {
					slot_found = 1;
					slot_block = block - 1;
					slot_offset = offset;
					slot_actual = actual;
					slot_split = 1;
				}
			}
			offset += reclen;
			pos += reclen;
		}
		unmap_brelse(bh);
	}

	if (slot_found) {
		bh = ext2_bread(dir, slot_block, 0);
		if (!bh)
			return -EIO;
		map_buffer(bh);
		de = (struct ext2_dir_entry *)(bh->b_data + slot_offset);
		if (!ext2_dirent_ok(de, slot_offset)) {
			unmap_brelse(bh);
			return -EIO;
		}
		reclen = de->rec_len;
		if (slot_split) {
			actual = slot_actual;
			if (reclen < actual || reclen - actual < need) {
				unmap_brelse(bh);
				return -EIO;
			}
			newde = (struct ext2_dir_entry *)((char *)de + actual);
			newde->inode = ino;
			newde->rec_len = reclen - actual;
			newde->name_len = len;
			ext2_copy_name(newde->name, name, len, fromfs);
			de->rec_len = actual;
		} else {
			if (reclen < need) {
				unmap_brelse(bh);
				return -EIO;
			}
			if (reclen - need >= 8) {
				newde = (struct ext2_dir_entry *)((char *)de + need);
				newde->inode = 0;
				newde->rec_len = reclen - need;
				newde->name_len = 0;
				de->rec_len = need;
			}
			de->inode = ino;
			de->name_len = len;
			ext2_copy_name(de->name, name, len, fromfs);
		}
		mark_buffer_dirty(bh);
		unmap_brelse(bh);
		dir->i_mtime = dir->i_ctime = current_time();
		dir->i_dirt = 1;
		return 0;
	}

	bh = ext2_bread(dir, (block_t)(dir->i_size >> BLOCK_SIZE_BITS), 1);
	if (!bh)
		return -ENOSPC;
	map_buffer(bh);
	de = (struct ext2_dir_entry *)bh->b_data;
	de->inode = ino;
	de->rec_len = BLOCK_SIZE;
	de->name_len = len;
	ext2_copy_name(de->name, name, len, fromfs);
	mark_buffer_dirty(bh);
	unmap_brelse(bh);
	dir->i_size += BLOCK_SIZE;
	dir->i_mtime = dir->i_ctime = current_time();
	dir->i_dirt = 1;
	return 0;
}

static int ext2_empty_dir(register struct inode *inode)
{
	register struct buffer_head *bh;
	struct ext2_dir_entry *de;
	block_t block;
	loff_t pos;
	unsigned short offset;
	unsigned short reclen;
	unsigned short namelen;

	block = 0;
	pos = 0;
	while (pos < (loff_t)inode->i_size) {
		bh = ext2_bread(inode, block++, 0);
		if (!bh) {
			pos += BLOCK_SIZE;
			continue;
		}
		map_buffer(bh);
		offset = 0;
		while (offset < BLOCK_SIZE && pos < (loff_t)inode->i_size) {
			de = (struct ext2_dir_entry *)(bh->b_data + offset);
			if (!ext2_dirent_ok(de, offset)) {
				unmap_brelse(bh);
				return 0;
			}
			reclen = de->rec_len;
			namelen = de->name_len;
			if (de->inode) {
				if (!(namelen == 1 && de->name[0] == '.') &&
				    !(namelen == 2 && de->name[0] == '.' &&
				      de->name[1] == '.')) {
					unmap_brelse(bh);
					return 0;
				}
			}
			offset += reclen;
			pos += reclen;
		}
		unmap_brelse(bh);
	}
	return 1;
}

int ext2_lookup(register struct inode *dir, const char *name, size_t len,
		register struct inode **result)
{
	struct ext2_dir_entry *de;
	struct buffer_head *bh;
	int error;

	error = -ENOENT;
	*result = NULL;

	if (len > EXT2_NAME_LEN) {
		iput(dir);
		return -ENAMETOOLONG;
	}

	if (S_ISDIR(dir->i_mode)) {
		bh = ext2_find_entry(dir, name, len, &de);
		if (bh) {
			*result = iget(dir->i_sb, (ino_t)de->inode);
			unmap_brelse(bh);
			error = (!*result) ? -EACCES : 0;
		}
	}

	iput(dir);
	return error;
}

int ext2_create(register struct inode *dir, const char *name, size_t len,
		mode_t mode, struct inode **result)
{
	register struct inode *inode;
	int error;

	*result = NULL;
	error = -ENOSPC;
	inode = ext2_new_inode(dir, mode);
	if (!inode) {
		iput(dir);
		return error;
	}

	error = ext2_add_entry(dir, name, len, inode->i_ino, 1);
	if (error) {
		inode->i_nlink = 0;
		iput(inode);
		iput(dir);
		return error;
	}

	inode->i_dirt = 1;
	iput(dir);
	*result = inode;
	return 0;
}

int ext2_mknod(register struct inode *dir, const char *name, size_t len,
	       mode_t mode, int rdev)
{
	register struct inode *inode;
	int error;

	error = -ENOSPC;
	inode = ext2_new_inode(dir, mode);
	if (!inode) {
		iput(dir);
		return error;
	}
	if (S_ISBLK(mode) || S_ISCHR(mode))
		inode->i_rdev = to_kdev_t(rdev);
	inode->i_dirt = 1;

	error = ext2_add_entry(dir, name, len, inode->i_ino, 1);
	if (error) {
		inode->i_nlink = 0;
		iput(inode);
		iput(dir);
		return error;
	}
	iput(inode);
	iput(dir);
	return 0;
}

int ext2_mkdir(register struct inode *dir, const char *name, size_t len,
	       mode_t mode)
{
	register struct inode *inode;
	register struct buffer_head *bh;
	struct ext2_dir_entry *de;
	struct ext2_dir_entry *de2;
	int error;

	error = -ENOSPC;
	inode = ext2_new_inode(dir, mode);
	if (!inode) {
		iput(dir);
		return error;
	}

	inode->i_nlink = 2;
	inode->i_dirt = 1;
	bh = ext2_bread(inode, 0, 1);
	if (!bh) {
		inode->i_nlink = 0;
		iput(inode);
		iput(dir);
		return -ENOSPC;
	}
	inode->i_size = BLOCK_SIZE;
	map_buffer(bh);
	de = (struct ext2_dir_entry *)bh->b_data;
	de->inode = inode->i_ino;
	de->rec_len = EXT2_DIR_REC_LEN(1);
	de->name_len = 1;
	de->name[0] = '.';
	de2 = (struct ext2_dir_entry *)((char *)de + de->rec_len);
	de2->inode = dir->i_ino;
	de2->rec_len = BLOCK_SIZE - de->rec_len;
	de2->name_len = 2;
	de2->name[0] = '.';
	de2->name[1] = '.';
	mark_buffer_dirty(bh);
	unmap_brelse(bh);

	error = ext2_add_entry(dir, name, len, inode->i_ino, 1);
	if (error) {
		inode->i_nlink = 0;
		iput(inode);
		iput(dir);
		return error;
	}

	dir->i_nlink++;
	dir->i_mtime = dir->i_ctime = current_time();
	dir->i_dirt = 1;
	iput(inode);
	iput(dir);
	return 0;
}

int ext2_link(register struct inode *dir, char *name, size_t len,
	      register struct inode *oldinode)
{
	int error;

	if (S_ISDIR(oldinode->i_mode))
		error = -EPERM;
	else if (oldinode->i_nlink >= 255)
		error = -EMLINK;
	else if (!(error = ext2_add_entry(dir, name, len,
					  oldinode->i_ino, 1))) {
		oldinode->i_nlink++;
		oldinode->i_ctime = current_time();
		oldinode->i_dirt = 1;
	}

	iput(dir);
	iput(oldinode);
	return error;
}

int ext2_unlink(register struct inode *dir, char *name, size_t len)
{
	register struct inode *inode;
	struct buffer_head *bh;
	struct ext2_dir_entry *de;
	int error;

	error = -ENOENT;
	bh = ext2_find_entry(dir, name, len, &de);
	if (!bh) {
		iput(dir);
		return error;
	}
	inode = iget(dir->i_sb, (ino_t)de->inode);
	if (!inode) {
		unmap_brelse(bh);
		iput(dir);
		return -EACCES;
	}
	if (S_ISDIR(inode->i_mode)) {
		iput(inode);
		unmap_brelse(bh);
		iput(dir);
		return -EPERM;
	}
	de->inode = 0;
	mark_buffer_dirty(bh);
	dir->i_ctime = dir->i_mtime = current_time();
	dir->i_dirt = 1;
	if (inode->i_nlink)
		inode->i_nlink--;
	inode->i_ctime = dir->i_ctime;
	inode->i_dirt = 1;
	iput(inode);
	unmap_brelse(bh);
	iput(dir);
	return 0;
}

int ext2_symlink(struct inode *dir, char *name, size_t len, char *symname)
{
	struct inode *inode;
	struct buffer_head *bh;
	struct ext2_inode *raw_inode;
	int error;

	error = strlen_fromfs(symname, EXT2_LINK_MAX);
	if (error > EXT2_LINK_MAX)
		error = EXT2_LINK_MAX;
	if (error < 0)
		goto out_dir;

	inode = ext2_new_inode(dir, S_IFLNK | 0777);
	if (!inode) {
		error = -ENOSPC;
		goto out_dir;
	}

	inode->i_size = (size_t)error;
	inode->i_dirt = 1;

	bh = ext2_get_inode(inode, &raw_inode);
	if (!bh) {
		error = -ENOSPC;
		goto out_inode;
	}

	memset(raw_inode->i_block, 0, sizeof(raw_inode->i_block));
	if (inode->i_size <= sizeof(raw_inode->i_block)) {
		memcpy_fromfs(raw_inode->i_block, symname, inode->i_size);
		mark_buffer_dirty(bh);
		unmap_brelse(bh);
	} else {
		unmap_brelse(bh);
		bh = ext2_bread(inode, 0, 1);
		if (!bh) {
			error = -ENOSPC;
			goto out_inode;
		}
		map_buffer(bh);
		memcpy_fromfs(bh->b_data, symname, inode->i_size);
		bh->b_data[inode->i_size] = 0;
		mark_buffer_dirty(bh);
		unmap_brelse(bh);
	}

	error = ext2_add_entry(dir, name, len, inode->i_ino, 1);
	if (!error) {
		iput(inode);
		iput(dir);
		return 0;
	}

out_inode:
	inode->i_nlink = 0;
	inode->i_dirt = 1;
	iput(inode);
out_dir:
	iput(dir);
	return error;
}

int ext2_rmdir(register struct inode *dir, char *name, size_t len)
{
	register struct inode *inode;
	struct buffer_head *bh;
	struct ext2_dir_entry *de;
	int error;

	error = -ENOENT;
	bh = ext2_find_entry(dir, name, len, &de);
	if (!bh) {
		iput(dir);
		return error;
	}
	inode = iget(dir->i_sb, (ino_t)de->inode);
	if (!inode) {
		unmap_brelse(bh);
		iput(dir);
		return -EACCES;
	}
	if (!S_ISDIR(inode->i_mode)) {
		iput(inode);
		unmap_brelse(bh);
		iput(dir);
		return -ENOTDIR;
	}
	if (dir->i_dev != inode->i_dev || dir == inode || inode->i_count > 1 ||
	    !ext2_empty_dir(inode)) {
		iput(inode);
		unmap_brelse(bh);
		iput(dir);
		return -ENOTEMPTY;
	}
	de->inode = 0;
	mark_buffer_dirty(bh);
	inode->i_nlink = 0;
	inode->i_dirt = 1;
	inode->i_ctime = dir->i_ctime = dir->i_mtime = current_time();
	if (dir->i_nlink)
		dir->i_nlink--;
	dir->i_dirt = 1;
	iput(inode);
	unmap_brelse(bh);
	iput(dir);
	return 0;
}

static int ext2_readlink_data(struct inode *inode, char *buffer,
			      size_t buflen, int nulterm)
{
	register struct buffer_head *bh;
	struct ext2_inode *raw_inode;
	size_t len;

	if (!S_ISLNK(inode->i_mode))
		return -EINVAL;

	if (inode->i_size > EXT2_LINK_MAX)
		return -ENAMETOOLONG;

	len = inode->i_size;
	if (len > buflen)
		len = buflen;

	bh = ext2_get_inode(inode, &raw_inode);
	if (!bh)
		return -EIO;

	if (inode->i_size <= sizeof(raw_inode->i_block)) {
		memcpy(buffer, raw_inode->i_block, len);
		unmap_brelse(bh);
		if (nulterm)
			buffer[len] = 0;
		return len;
	}

	unmap_brelse(bh);
	if (inode->i_size > BLOCK_SIZE)
		return -ENAMETOOLONG;

	bh = ext2_bread(inode, 0, 0);
	if (!bh)
		return -EIO;
	map_buffer(bh);
	memcpy(buffer, bh->b_data, len);
	unmap_brelse(bh);
	if (nulterm)
		buffer[len] = 0;
	return len;
}

static int ext2_follow_link(struct inode *dir, register struct inode *inode,
			    int flag, mode_t mode, struct inode **res_inode)
{
	int error;
	static int link_count = 0;
	char link[EXT2_LINK_MAX + 1];
	seg_t ds, *pds;

	*res_inode = NULL;
	if (!dir) {
		dir = current->fs.root;
		dir->i_count++;
	}
	if (!inode)
		error = -ENOENT;
	else if (!S_ISLNK(inode->i_mode)) {
		*res_inode = inode;
		error = 0;
	} else if (link_count > 5) {
		iput(inode);
		error = -ELOOP;
	} else {
		error = ext2_readlink_data(inode, link, EXT2_LINK_MAX, 1);
		iput(inode);
		if (error < 0) {
			iput(dir);
			return error;
		}
		link_count++;
		pds = &current->t_regs.ds;
		ds = *pds;
		*pds = kernel_ds;
		error = open_namei(link, flag, mode, res_inode, dir);
		*pds = ds;
		link_count--;
		return error;
	}

	iput(dir);
	return error;
}

static int ext2_readlink(register struct inode *inode, char *buffer,
			 size_t buflen)
{
	register struct buffer_head *bh;
	struct ext2_inode *raw_inode;
	size_t len;

	if (!S_ISLNK(inode->i_mode))
		len = -EINVAL;
	else if (inode->i_size > EXT2_LINK_MAX)
		len = -ENAMETOOLONG;
	else {
		len = inode->i_size;
		if (len > buflen)
			len = buflen;
		bh = ext2_get_inode(inode, &raw_inode);
		if (!bh)
			len = -EIO;
		else if (inode->i_size <= sizeof(raw_inode->i_block)) {
			memcpy_tofs(buffer, raw_inode->i_block, len);
			unmap_brelse(bh);
		} else {
			unmap_brelse(bh);
			if (inode->i_size > BLOCK_SIZE)
				len = -ENAMETOOLONG;
			else {
				bh = ext2_bread(inode, 0, 0);
				if (!bh)
					len = -EIO;
				else {
					map_buffer(bh);
					memcpy_tofs(buffer, bh->b_data, len);
					unmap_brelse(bh);
				}
			}
		}
	}
	iput(inode);
	return len;
}

struct inode_operations ext2_symlink_inode_operations = {
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	ext2_readlink,
	ext2_follow_link,
	NULL,
	NULL
};
