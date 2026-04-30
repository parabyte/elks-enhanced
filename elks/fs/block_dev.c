/*
 *  linux/fs/block_dev.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include <linuxmt/config.h>
#include <linuxmt/errno.h>
#include <linuxmt/sched.h>
#include <linuxmt/kernel.h>
#include <linuxmt/fcntl.h>
#include <linuxmt/debug.h>

size_t block_read(struct inode *inode, struct file *filp, char *buf, size_t count)
{
#if defined(CONFIG_MINIX_FS) || defined(CONFIG_EXT2_FS) || defined(CONFIG_BLK_DEV_CHAR)
    loff_t pos;
    block32_t block;
    size_t chars;
    size_t read = 0;

    /* Amount we can do I/O over */
    pos = ((loff_t)inode->i_size) - filp->f_pos;
    if (pos <= 0)
	return 0;       /* EOF */

    if (check_disk_change(inode->i_rdev))
        return -ENXIO;

    if ((loff_t)count > pos) count = (size_t)pos;

    while (count > 0) {
	register struct buffer_head *bh;

	/*
	 *      Read the block in
	 */
	block = (block32_t)(filp->f_pos >> BLOCK_SIZE_BITS);
	if (inode->i_op->getblk) {
	    bh = inode->i_op->getblk(inode, (block_t)block, 0);
	} else {
	    bh = getblk32(inode->i_rdev, block);
	}
	/* Offset to block/offset */
	chars = BLOCK_SIZE - (((size_t)(filp->f_pos)) & (BLOCK_SIZE - 1));
	if (chars > count) chars = count;
	if (bh) {
	    if (!readbuf(bh)) {
		if (!read) read = -EIO;
		break;
	    }
	    xms_fmemcpyb(buf, current->t_regs.ds,
		buffer_data(bh) + (((size_t)(filp->f_pos)) & (BLOCK_SIZE - 1)),
		buffer_seg(bh), chars);
	    brelse(bh);
	} else fmemsetb(buf, current->t_regs.ds, 0, chars);
	buf += chars;
	filp->f_pos += chars;
	read += chars;
	count -= chars;
    }
#ifdef FIXME
    if (!IS_RDONLY(inode) && !IS_NOATIME(inode)) inode->i_atime = current_time();
#endif
    return read;
#else
    return -EINVAL;
#endif
}

size_t block_write(struct inode *inode, struct file *filp, char *buf, size_t count)
{
#if defined(CONFIG_MINIX_FS) || defined(CONFIG_EXT2_FS) || defined(CONFIG_BLK_DEV_CHAR)
    block32_t block;
    size_t chars, offset;
    size_t written = 0;

    if (check_disk_change(inode->i_rdev))
        return -ENXIO;

    if (filp->f_flags & O_APPEND) filp->f_pos = (loff_t)inode->i_size;

    while (count > 0) {
	register struct buffer_head *bh;
	int create;

	block = (block32_t)(filp->f_pos >> BLOCK_SIZE_BITS);
	/* Offset to block/offset */
	offset = ((size_t)filp->f_pos) & (BLOCK_SIZE - 1);
	chars = BLOCK_SIZE - offset;
	if (chars > count) chars = count;
	if (inode->i_op->getblk) {
	    create = GETBLK_CREATE;
	    if (offset == 0 && chars == BLOCK_SIZE)
		create |= GETBLK_NOZERO;
	    bh = inode->i_op->getblk(inode, (block_t)block, create);
	} else {
	    bh = getblk32(inode->i_rdev, block);
	}
	if (!bh) {
	    if (!written) written = -ENOSPC;
	    break;
	}
	/*
	 *      Read the block in, unless we
	 *      are writing a whole block.
	 */
	if (chars != BLOCK_SIZE) {
	    if (!readbuf(bh)) {
		if (!written) written = -EIO;
		break;
	    }
	}
	/*
	 *      Alter buffer, mark dirty
	 */
	xms_fmemcpyb(buffer_data(bh) + offset, buffer_seg(bh), buf,
		current->t_regs.ds, chars);
	mark_buffer_uptodate(bh, 1);
	mark_buffer_dirty(bh);
	brelse(bh);
	buf += chars;
	filp->f_pos += chars;
	written += chars;
	count -= chars;
    }
    if ((loff_t)inode->i_size < filp->f_pos)
        inode->i_size = (__u32) filp->f_pos;
    inode->i_mtime = inode->i_ctime = current_time();
    inode->i_dirt = 1;
    return written;
#else
    return -EINVAL;
#endif
}

#if UNUSED
#define BLOCK_READ	0
#define BLOCK_WRITE	1

/* could be used for raw char devices, instead block_read/block_write is used */
static int blk_rw(struct inode *inode, register struct file *filp,
		  char *buf, size_t count, int wr)
{
    register struct buffer_head *bh;
    block32_t block;
    size_t chars, offset;
    int written = 0;

    while (count > 0) {
    /*
     *      Offset to block/offset
     */
	offset = ((size_t)filp->f_pos) & (BLOCK_SIZE - 1);
	chars = BLOCK_SIZE - offset;
	if (chars > count)
	    chars = count;
	/*
	 *      Read the block in - use getblk on a write
	 *      of a whole block to avoid a read of the data.
	 */
	block = (block32_t)(filp->f_pos >> BLOCK_SIZE_BITS);
	bh = getblk32(inode->i_rdev, block);
	if ((wr == BLOCK_READ) || (chars != BLOCK_SIZE)) {
	    if (!readbuf(bh)) {
		if (!written) written = -EIO;
		break;
	    }
	}

	if (wr == BLOCK_WRITE) {
	    /*
	     *      Alter buffer, mark dirty
	     */
	    xms_fmemcpyb(buffer_data(bh) + offset, buffer_seg(bh), buf,
		current->t_regs.ds, chars);
	    mark_buffer_dirty(bh);
	    mark_buffer_uptodate(bh, 1);
	    /*
	     *      Writing: queue physical I/O
	     */
	    ll_rw_blk(WRITE, bh);
	    wait_on_buffer(bh);
	    if (!buffer_uptodate(bh)) { /* Write error. */
		brelse(bh);
		if (!written) written = -EIO;
		break;
	    }
	} else {
	    /*
	     *      Empty buffer data. Buffer unchanged
	     */
	    xms_fmemcpyb(buf, current->t_regs.ds, buffer_data(bh) + offset,
		buffer_seg(bh), chars);
	}
	/*
	 *      Move on and release buffer
	 */

	brelse(bh);

	buf += chars;
	filp->f_pos += chars;
	written += chars;
	count -= chars;
    }
    return written;
}

size_t blk_read(struct inode *inode, struct file *filp, char *buf, size_t count)
{
    return blk_rw(inode, filp, buf, count, BLOCK_READ);
}

size_t blk_write(struct inode *inode, struct file *filp, char *buf, size_t count)
{
    return blk_rw(inode, filp, buf, count, BLOCK_WRITE);
}
#endif
