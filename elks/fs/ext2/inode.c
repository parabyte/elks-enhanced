/*
 *  linux/fs/ext2/inode.c
 *
 *  Minimal ext2 support for ELKS.
 *  Current implementation supports ext2 rev0, 1KiB blocks, and
 *  experimental read-write access for regular files and basic directory
 *  mutation.
 */

#include <linuxmt/types.h>
#include <linuxmt/config.h>
#include <linuxmt/sched.h>
#include <linuxmt/ext2_fs.h>
#include <linuxmt/kernel.h>
#include <linuxmt/string.h>
#include <linuxmt/stat.h>
#include <linuxmt/errno.h>
#include <linuxmt/debug.h>
#include <linuxmt/kdev_t.h>

#include <arch/system.h>
#include <arch/bitops.h>

#ifdef CONFIG_EXT2_DEBUG
#define ext2_dbg printk
#else
#define ext2_dbg(...)
#endif

#define EXT2_I_BLOCKS_PER_BLOCK	(BLOCK_SIZE >> 9)
#define EXT2_PREALLOC_BLOCKS		3

static void ext2_write_inode(register struct inode *inode);
static void ext2_put_inode(register struct inode *inode);
static void ext2_free_inode(register struct inode *inode);
static void ext2_write_super(register struct super_block *sb);
static void ext2_set_ops(register struct inode *inode);
static void ext2_discard_prealloc(register struct super_block *sb);
static void ext2_discard_prealloc_inode(register struct inode *inode);

static struct super_operations ext2_sops = {
	ext2_read_inode,
	ext2_write_inode,
	ext2_put_inode,
	ext2_put_super,
	ext2_write_super,
	ext2_remount,
	ext2_statfs
};

static unsigned short ext2_group_blocks(struct super_block *sb,
					unsigned short group)
{
	struct ext2_sb_info *sbi;
	unsigned long left;

	sbi = EXT2_SB(sb);
	left = sbi->s_blocks_count - sbi->s_first_data_block;
	left -= (unsigned long)group * sbi->s_blocks_per_group;
	if (left > sbi->s_blocks_per_group)
		left = sbi->s_blocks_per_group;
	return (unsigned short)left;
}

static unsigned short ext2_group_inodes(struct super_block *sb,
					unsigned short group)
{
	struct ext2_sb_info *sbi;
	unsigned long left;

	sbi = EXT2_SB(sb);
	left = sbi->s_inodes_count - (unsigned long)group * sbi->s_inodes_per_group;
	if (left > sbi->s_inodes_per_group)
		left = sbi->s_inodes_per_group;
	return (unsigned short)left;
}

static block32_t ext2_group_first_block(struct super_block *sb,
					unsigned short group)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);

	return sbi->s_first_data_block +
		(block32_t)group * sbi->s_blocks_per_group;
}

static void ext2_commit_super(struct super_block *sb)
{
	register struct buffer_head *bh;
	struct ext2_super_block *es;
	struct ext2_sb_info *sbi;

	bh = bread(sb->s_dev, 1);
	if (!bh) {
		ext2_dbg("EXT2: commit super bread blk 1 on %D failed\n", sb->s_dev);
		return;
	}
	map_buffer(bh);
	es = (struct ext2_super_block *)bh->b_data;
	sbi = EXT2_SB(sb);
	es->s_free_blocks_count = sbi->s_free_blocks_count;
	es->s_free_inodes_count = sbi->s_free_inodes_count;
	es->s_mnt_count = sbi->s_mnt_count;
	es->s_state = sbi->s_state;
	mark_buffer_dirty(bh);
	unmap_brelse(bh);
}

static void ext2_mark_super_dirty(struct super_block *sb)
{
	if (sb)
		sb->s_dirt = 1;
}

static struct buffer_head *ext2_get_group_desc(struct super_block *sb,
					       unsigned short group,
					       struct ext2_group_desc **res_gdp)
{
	register struct buffer_head *bh;
	block32_t block;
	unsigned short offset;
	struct ext2_sb_info *sbi;

	*res_gdp = NULL;
	sbi = EXT2_SB(sb);
	if (group >= sbi->s_groups_count) {
		ext2_dbg("EXT2: group %u >= ngroups %u on %D\n",
			 group, sbi->s_groups_count, sb->s_dev);
		return NULL;
	}

	block = sbi->s_first_data_block + 1 +
		(group / sbi->s_desc_per_block);
	offset = (group % sbi->s_desc_per_block) *
		sizeof(struct ext2_group_desc);

	bh = bread32(sb->s_dev, block);
	if (!bh) {
		ext2_dbg("EXT2: group %u desc block %lu on %D failed\n",
			 group, (unsigned long)block, sb->s_dev);
		return NULL;
	}
	map_buffer(bh);
	*res_gdp = (struct ext2_group_desc *)(bh->b_data + offset);
	return bh;
}

static void ext2_inc_iblocks(struct ext2_inode *raw_inode)
{
	raw_inode->i_blocks += EXT2_I_BLOCKS_PER_BLOCK;
}

static void ext2_dec_iblocks(struct ext2_inode *raw_inode)
{
	if (raw_inode->i_blocks >= EXT2_I_BLOCKS_PER_BLOCK)
		raw_inode->i_blocks -= EXT2_I_BLOCKS_PER_BLOCK;
	else
		raw_inode->i_blocks = 0;
}

static block32_t ext2_new_block(struct super_block *sb, int zero)
{
	register struct buffer_head *bh;
	register struct buffer_head *gdbh;
	struct ext2_group_desc *gdp;
	struct ext2_sb_info *sbi;
	block32_t block;
	unsigned short bit;
	unsigned short group;
	unsigned short group_blocks;
	unsigned short i;

	sbi = EXT2_SB(sb);
	for (i = 0; i < sbi->s_groups_count; i++) {
		group = (unsigned short)(((unsigned int)sbi->s_last_block_group +
					  i) %
					 sbi->s_groups_count);
		gdbh = ext2_get_group_desc(sb, group, &gdp);
		if (!gdbh)
			continue;
		if (!gdp->bg_free_blocks_count) {
			unmap_brelse(gdbh);
			continue;
		}
		group_blocks = ext2_group_blocks(sb, group);
		bh = bread32(sb->s_dev, gdp->bg_block_bitmap);
		if (!bh) {
			ext2_dbg("EXT2: block bitmap %lu grp %u on %D failed\n",
				 (unsigned long)gdp->bg_block_bitmap, group,
				 sb->s_dev);
			unmap_brelse(gdbh);
			continue;
		}
		map_buffer(bh);
		bit = find_first_zero_bit((int *)bh->b_data, group_blocks);
		if (bit >= group_blocks) {
			unmap_brelse(bh);
			unmap_brelse(gdbh);
			continue;
		}
		if (set_bit(bit, bh->b_data)) {
			unmap_brelse(bh);
			unmap_brelse(gdbh);
			continue;
		}
		mark_buffer_dirty(bh);
		block = ext2_group_first_block(sb, group) + bit;
		gdp->bg_free_blocks_count--;
		sbi->s_free_blocks_count--;
		mark_buffer_dirty(gdbh);
		unmap_brelse(bh);
		unmap_brelse(gdbh);
		sbi->s_last_block_group = group;
		ext2_mark_super_dirty(sb);
		if (zero) {
			bh = getblk32(sb->s_dev, block);
			if (bh) {
				zero_buffer(bh, 0, BLOCK_SIZE);
				mark_buffer_uptodate(bh, 1);
				mark_buffer_dirty(bh);
				brelse(bh);
			}
		}
		return block;
	}
	return 0;
}

static void ext2_free_block(struct super_block *sb, block32_t block)
{
	register struct buffer_head *bh;
	register struct buffer_head *gdbh;
	struct ext2_group_desc *gdp;
	struct ext2_sb_info *sbi;
	unsigned short group;
	unsigned short bit;

	if (!block)
		return;
	sbi = EXT2_SB(sb);
	if (block < sbi->s_first_data_block || block >= sbi->s_blocks_count)
		return;

	group = (unsigned short)((block - sbi->s_first_data_block) /
		sbi->s_blocks_per_group);
	bit = (unsigned short)(block - ext2_group_first_block(sb, group));

	gdbh = ext2_get_group_desc(sb, group, &gdp);
	if (!gdbh)
		return;
	bh = bread32(sb->s_dev, gdp->bg_block_bitmap);
	if (!bh) {
		unmap_brelse(gdbh);
		return;
	}
	map_buffer(bh);
	if (!clear_bit(bit, bh->b_data))
		printk("EXT2: block %lu on %D already free\n",
		       (unsigned long)block, sb->s_dev);
	else {
		mark_buffer_dirty(bh);
		gdp->bg_free_blocks_count++;
		sbi->s_free_blocks_count++;
		mark_buffer_dirty(gdbh);
		ext2_mark_super_dirty(sb);
	}
	unmap_brelse(bh);
	unmap_brelse(gdbh);

	bh = getblk32(sb->s_dev, block);
	if (bh) {
		mark_buffer_clean(bh);
		brelse(bh);
	}
}

static void ext2_clear_prealloc(struct ext2_sb_info *sbi)
{
	sbi->s_prealloc_ino = 0;
	sbi->s_prealloc_block = 0;
	sbi->s_prealloc_lblock = 0;
	sbi->s_prealloc_count = 0;
}

static void ext2_discard_prealloc(register struct super_block *sb)
{
	struct ext2_sb_info *sbi;
	block32_t block;
	unsigned char count;

	if (!sb)
		return;
	sbi = EXT2_SB(sb);
	count = sbi->s_prealloc_count;
	if (!count)
		return;

	block = sbi->s_prealloc_block;
	ext2_clear_prealloc(sbi);
	while (count--)
		ext2_free_block(sb, block++);
}

static void ext2_discard_prealloc_inode(register struct inode *inode)
{
	struct ext2_sb_info *sbi;

	if (!inode || !inode->i_sb)
		return;
	sbi = EXT2_SB(inode->i_sb);
	if (sbi->s_prealloc_count && sbi->s_prealloc_ino == inode->i_ino)
		ext2_discard_prealloc(inode->i_sb);
}

static block32_t ext2_take_prealloc(register struct inode *inode, block_t lblock)
{
	struct ext2_sb_info *sbi;
	block32_t nr;

	sbi = EXT2_SB(inode->i_sb);
	if (!sbi->s_prealloc_count)
		return 0;
	if (sbi->s_prealloc_ino != inode->i_ino ||
	    sbi->s_prealloc_lblock != (block32_t)lblock) {
		ext2_discard_prealloc(inode->i_sb);
		return 0;
	}

	nr = sbi->s_prealloc_block++;
	sbi->s_prealloc_lblock++;
	sbi->s_prealloc_count--;
	if (!sbi->s_prealloc_count)
		ext2_clear_prealloc(sbi);
	return nr;
}

static void ext2_prealloc_after(register struct inode *inode, block_t lblock,
				block32_t first)
{
	register struct buffer_head *bh;
	register struct buffer_head *gdbh;
	struct ext2_group_desc *gdp;
	struct ext2_sb_info *sbi;
	unsigned short group;
	unsigned short bit;
	unsigned short group_blocks;
	unsigned char count;

	if (!inode || !inode->i_sb || !S_ISREG(inode->i_mode))
		return;
	sbi = EXT2_SB(inode->i_sb);
	if (sbi->s_prealloc_count)
		ext2_discard_prealloc(inode->i_sb);
	if (!first || first < sbi->s_first_data_block ||
	    first >= sbi->s_blocks_count)
		return;

	group = (unsigned short)((first - sbi->s_first_data_block) /
		sbi->s_blocks_per_group);
	bit = (unsigned short)(first - ext2_group_first_block(inode->i_sb,
		group));
	group_blocks = ext2_group_blocks(inode->i_sb, group);
	if (bit >= group_blocks)
		return;

	gdbh = ext2_get_group_desc(inode->i_sb, group, &gdp);
	if (!gdbh)
		return;
	if (!gdp->bg_free_blocks_count) {
		unmap_brelse(gdbh);
		return;
	}
	bh = bread32(inode->i_sb->s_dev, gdp->bg_block_bitmap);
	if (!bh) {
		unmap_brelse(gdbh);
		return;
	}
	map_buffer(bh);
	count = 0;
	while (count < EXT2_PREALLOC_BLOCKS &&
	       (unsigned short)(bit + count) < group_blocks &&
	       first + count < sbi->s_blocks_count) {
		if (test_bit(bit + count, bh->b_data))
			break;
		if (set_bit(bit + count, bh->b_data))
			break;
		count++;
	}
	if (count) {
		mark_buffer_dirty(bh);
		if (gdp->bg_free_blocks_count >= count)
			gdp->bg_free_blocks_count -= count;
		else
			gdp->bg_free_blocks_count = 0;
		if (sbi->s_free_blocks_count >= count)
			sbi->s_free_blocks_count -= count;
		else
			sbi->s_free_blocks_count = 0;
		mark_buffer_dirty(gdbh);
		sbi->s_prealloc_ino = inode->i_ino;
		sbi->s_prealloc_block = first;
		sbi->s_prealloc_lblock = (block32_t)lblock;
		sbi->s_prealloc_count = count;
		ext2_mark_super_dirty(inode->i_sb);
	}
	unmap_brelse(bh);
	unmap_brelse(gdbh);
}

static block32_t ext2_new_data_block(register struct inode *inode,
				     block_t lblock, int create)
{
	struct ext2_sb_info *sbi;
	block32_t nr;
	int nozero;

	nozero = create & GETBLK_NOZERO;
	if (!nozero)
		ext2_discard_prealloc_inode(inode);

	nr = nozero ? ext2_take_prealloc(inode, lblock) : 0;
	if (nr) {
		sbi = EXT2_SB(inode->i_sb);
		if (!sbi->s_prealloc_count)
			ext2_prealloc_after(inode, lblock + 1, nr + 1);
		return nr;
	}

	nr = ext2_new_block(inode->i_sb, !nozero);
	if (nr && nozero)
		ext2_prealloc_after(inode, lblock + 1, nr + 1);
	return nr;
}

struct inode *ext2_new_inode(struct inode *dir, mode_t mode)
{
	register struct inode *inode;
	register struct buffer_head *bh;
	register struct buffer_head *gdbh;
	struct ext2_group_desc *gdp;
	struct ext2_sb_info *sbi;
	struct ext2_inode *raw_inode;
	unsigned short group;
	unsigned short bit;
	unsigned short group_inodes;
	unsigned short i;

	if (!dir || !(inode = new_inode(dir, mode)))
		return NULL;

	/* new_inode installs only generic non-filesystem ops; override them. */
	ext2_set_ops(inode);

	sbi = EXT2_SB(dir->i_sb);
	for (i = 0; i < sbi->s_groups_count; i++) {
		group = (unsigned short)(((unsigned int)sbi->s_last_inode_group +
					  i) %
					 sbi->s_groups_count);
		gdbh = ext2_get_group_desc(dir->i_sb, group, &gdp);
		if (!gdbh)
			continue;
		if (!gdp->bg_free_inodes_count) {
			unmap_brelse(gdbh);
			continue;
		}
		group_inodes = ext2_group_inodes(dir->i_sb, group);
		bh = bread32(dir->i_sb->s_dev, gdp->bg_inode_bitmap);
		if (!bh) {
			ext2_dbg("EXT2: inode bitmap %lu grp %u on %D failed\n",
				 (unsigned long)gdp->bg_inode_bitmap, group,
				 dir->i_sb->s_dev);
			unmap_brelse(gdbh);
			continue;
		}
		map_buffer(bh);
		bit = find_first_zero_bit((int *)bh->b_data, group_inodes);
		if (bit >= group_inodes) {
			unmap_brelse(bh);
			unmap_brelse(gdbh);
			continue;
		}
		if (set_bit(bit, bh->b_data)) {
			unmap_brelse(bh);
			unmap_brelse(gdbh);
			continue;
		}
		mark_buffer_dirty(bh);
		inode->i_ino = (ino_t)group * sbi->s_inodes_per_group + bit + 1;
		gdp->bg_free_inodes_count--;
		if (S_ISDIR(mode))
			gdp->bg_used_dirs_count++;
		sbi->s_free_inodes_count--;
		mark_buffer_dirty(gdbh);
		unmap_brelse(bh);
		unmap_brelse(gdbh);
		sbi->s_last_inode_group = group;
		ext2_mark_super_dirty(dir->i_sb);

		bh = ext2_get_inode(inode, &raw_inode);
		if (!bh) {
			ext2_free_inode(inode);
			return NULL;
		}
		memset(raw_inode, 0, sizeof(*raw_inode));
		raw_inode->i_mode = inode->i_mode;
		raw_inode->i_uid = inode->i_uid;
		raw_inode->i_gid = inode->i_gid;
		raw_inode->i_links_count = inode->i_nlink;
		raw_inode->i_atime = inode->i_atime;
		raw_inode->i_ctime = inode->i_ctime;
		raw_inode->i_mtime = inode->i_mtime;
		mark_buffer_dirty(bh);
		unmap_brelse(bh);
		inode->i_dirt = 0;
		return inode;
	}

	printk("EXT2: out of inodes on %E\n", dir->i_sb->s_dev);
	iput(inode);
	return NULL;
}

static void ext2_free_inode(register struct inode *inode)
{
	register struct buffer_head *bh;
	register struct buffer_head *gdbh;
	struct ext2_group_desc *gdp;
	struct ext2_inode *raw_inode;
	struct ext2_sb_info *sbi;
	unsigned short group;
	unsigned short bit;

	if (!inode->i_sb || !inode->i_ino)
		return;

	sbi = EXT2_SB(inode->i_sb);
	if (inode->i_ino > sbi->s_inodes_count)
		return;

	group = (unsigned short)((inode->i_ino - 1) / sbi->s_inodes_per_group);
	bit = (unsigned short)((inode->i_ino - 1) % sbi->s_inodes_per_group);

	bh = ext2_get_inode(inode, &raw_inode);
	if (bh) {
		memset(raw_inode, 0, sizeof(*raw_inode));
		mark_buffer_dirty(bh);
		unmap_brelse(bh);
	}

	gdbh = ext2_get_group_desc(inode->i_sb, group, &gdp);
	if (!gdbh)
		return;
	bh = bread32(inode->i_sb->s_dev, gdp->bg_inode_bitmap);
	if (!bh) {
		unmap_brelse(gdbh);
		return;
	}
	map_buffer(bh);
	if (!clear_bit(bit, bh->b_data))
		printk("EXT2: inode %lu on %D already free\n",
		       (unsigned long)inode->i_ino, inode->i_dev);
	else {
		mark_buffer_dirty(bh);
		gdp->bg_free_inodes_count++;
		if (S_ISDIR(inode->i_mode) && gdp->bg_used_dirs_count)
			gdp->bg_used_dirs_count--;
		sbi->s_free_inodes_count++;
		mark_buffer_dirty(gdbh);
		ext2_mark_super_dirty(inode->i_sb);
	}
	unmap_brelse(bh);
	unmap_brelse(gdbh);
	clear_inode(inode);
}

struct buffer_head *ext2_get_inode(struct inode *inode,
				   struct ext2_inode **raw_inode)
{
	register struct buffer_head *bh;
	register struct buffer_head *gdbh;
	struct ext2_group_desc *gdp;
	struct ext2_sb_info *sbi;
	unsigned long ino;
	unsigned long group;
	unsigned long index;
	block32_t block;

	*raw_inode = NULL;
	sbi = EXT2_SB(inode->i_sb);
	ino = inode->i_ino;
	if (!ino || ino > sbi->s_inodes_count) {
		printk("EXT2: inode %lu out of range (max %lu) on %D\n",
		       ino, (unsigned long)sbi->s_inodes_count, inode->i_dev);
		return NULL;
	}

	group = (ino - 1) / sbi->s_inodes_per_group;
	index = (ino - 1) % sbi->s_inodes_per_group;

	if (!sbi->s_inode_table_valid ||
	    sbi->s_inode_table_group != (unsigned short)group) {
		gdbh = ext2_get_group_desc(inode->i_sb, (unsigned short)group, &gdp);
		if (!gdbh) {
			ext2_dbg("EXT2: no group desc ino %lu grp %lu on %D\n",
				 ino, group, inode->i_dev);
			return NULL;
		}
		sbi->s_inode_table_block = gdp->bg_inode_table;
		sbi->s_inode_table_group = (unsigned short)group;
		sbi->s_inode_table_valid = 1;
		unmap_brelse(gdbh);
	}
	block = sbi->s_inode_table_block + (index / EXT2_INODES_PER_BLOCK);

	bh = bread32(inode->i_dev, block);
	if (!bh) {
		printk("EXT2: inode %lu block %lu bread failed on %D\n",
		       ino, (unsigned long)block, inode->i_dev);
		return NULL;
	}
	map_buffer(bh);
	*raw_inode = ((struct ext2_inode *)bh->b_data) +
		(index % EXT2_INODES_PER_BLOCK);
	return bh;
}

static block32_t ext2_bmap_indirect(kdev_t dev, block32_t block,
				    unsigned short index)
{
	register struct buffer_head *bh;
	block32_t nr;

	if (!block)
		return 0;
	bh = bread32(dev, block);
	if (!bh) {
		ext2_dbg("EXT2: indirect bread %lu idx %u on %D failed\n",
			 (unsigned long)block, index, dev);
		return 0;
	}
	map_buffer(bh);
	nr = ((__u32 *)bh->b_data)[index];
	unmap_brelse(bh);
	return nr;
}

block32_t ext2_bmap(struct inode *inode, block_t block)
{
	register struct buffer_head *bh;
	struct ext2_inode *raw_inode;
	block32_t nr;
	block32_t ind_block;
	unsigned short index;

	bh = ext2_get_inode(inode, &raw_inode);
	if (!bh)
		return 0;

	if (block < EXT2_NDIR_BLOCKS) {
		nr = raw_inode->i_block[block];
		unmap_brelse(bh);
		return nr;
	}

	block -= EXT2_NDIR_BLOCKS;
	ind_block = raw_inode->i_block[EXT2_IND_BLOCK];
	if (block < EXT2_ADDR_PER_BLOCK) {
		unmap_brelse(bh);
		return ext2_bmap_indirect(inode->i_dev, ind_block,
					  (unsigned short)block);
	}

	block -= EXT2_ADDR_PER_BLOCK;
	ind_block = raw_inode->i_block[EXT2_DIND_BLOCK];
	unmap_brelse(bh);

	index = (unsigned short)(block / EXT2_ADDR_PER_BLOCK);
	nr = ext2_bmap_indirect(inode->i_dev, ind_block, index);
	if (!nr)
		return 0;
	return ext2_bmap_indirect(inode->i_dev, nr,
				  (unsigned short)(block % EXT2_ADDR_PER_BLOCK));
}

struct buffer_head *ext2_getblk(struct inode *inode, block_t block, int create)
{
	register struct buffer_head *ibh;
	register struct buffer_head *bh;
	register struct buffer_head *bh2;
	struct ext2_inode *raw_inode;
	__u32 *ind;
	block32_t nr;
	block_t lblock;
	unsigned short index;
	unsigned short index2;

	lblock = block;
	ibh = ext2_get_inode(inode, &raw_inode);
	if (!ibh)
		return NULL;

	if (block < EXT2_NDIR_BLOCKS) {
		if (!raw_inode->i_block[block] && create) {
			raw_inode->i_block[block] =
				ext2_new_data_block(inode, lblock, create);
			if (raw_inode->i_block[block]) {
				ext2_inc_iblocks(raw_inode);
				mark_buffer_dirty(ibh);
				inode->i_ctime = current_time();
				inode->i_dirt = 1;
			}
		}
		nr = raw_inode->i_block[block];
		unmap_brelse(ibh);
		return nr ? getblk32(inode->i_dev, nr) : NULL;
	}

	block -= EXT2_NDIR_BLOCKS;
	if (block < EXT2_ADDR_PER_BLOCK) {
		if (!raw_inode->i_block[EXT2_IND_BLOCK] && create) {
			raw_inode->i_block[EXT2_IND_BLOCK] =
				ext2_new_block(inode->i_sb, 1);
			if (raw_inode->i_block[EXT2_IND_BLOCK]) {
				ext2_inc_iblocks(raw_inode);
				mark_buffer_dirty(ibh);
			}
		}
		nr = raw_inode->i_block[EXT2_IND_BLOCK];
		if (!nr) {
			unmap_brelse(ibh);
			return NULL;
		}
		bh = bread32(inode->i_dev, nr);
		if (!bh) {
			unmap_brelse(ibh);
			return NULL;
		}
		map_buffer(bh);
		ind = (__u32 *)bh->b_data;
		if (!ind[block] && create) {
			ind[block] = ext2_new_data_block(inode, lblock, create);
			if (ind[block]) {
				ext2_inc_iblocks(raw_inode);
				mark_buffer_dirty(bh);
				mark_buffer_dirty(ibh);
				inode->i_ctime = current_time();
				inode->i_dirt = 1;
			}
		}
		nr = ind[block];
		unmap_brelse(bh);
		unmap_brelse(ibh);
		return nr ? getblk32(inode->i_dev, nr) : NULL;
	}

	block -= EXT2_ADDR_PER_BLOCK;
	index = (unsigned short)(block / EXT2_ADDR_PER_BLOCK);
	index2 = (unsigned short)(block % EXT2_ADDR_PER_BLOCK);

	if (!raw_inode->i_block[EXT2_DIND_BLOCK] && create) {
		raw_inode->i_block[EXT2_DIND_BLOCK] =
			ext2_new_block(inode->i_sb, 1);
		if (raw_inode->i_block[EXT2_DIND_BLOCK]) {
			ext2_inc_iblocks(raw_inode);
			mark_buffer_dirty(ibh);
		}
	}
	nr = raw_inode->i_block[EXT2_DIND_BLOCK];
	if (!nr) {
		unmap_brelse(ibh);
		return NULL;
	}

	bh = bread32(inode->i_dev, nr);
	if (!bh) {
		unmap_brelse(ibh);
		return NULL;
	}
	map_buffer(bh);
	ind = (__u32 *)bh->b_data;
	if (!ind[index] && create) {
		ind[index] = ext2_new_block(inode->i_sb, 1);
		if (ind[index]) {
			ext2_inc_iblocks(raw_inode);
			mark_buffer_dirty(bh);
			mark_buffer_dirty(ibh);
		}
	}
	nr = ind[index];
	if (!nr) {
		unmap_brelse(bh);
		unmap_brelse(ibh);
		return NULL;
	}

	bh2 = bread32(inode->i_dev, nr);
	if (!bh2) {
		unmap_brelse(bh);
		unmap_brelse(ibh);
		return NULL;
	}
	map_buffer(bh2);
	ind = (__u32 *)bh2->b_data;
	if (!ind[index2] && create) {
		ind[index2] = ext2_new_data_block(inode, lblock, create);
		if (ind[index2]) {
			ext2_inc_iblocks(raw_inode);
			mark_buffer_dirty(bh2);
			mark_buffer_dirty(ibh);
			inode->i_ctime = current_time();
			inode->i_dirt = 1;
		}
	}
	nr = ind[index2];
	unmap_brelse(bh2);
	unmap_brelse(bh);
	unmap_brelse(ibh);
	return nr ? getblk32(inode->i_dev, nr) : NULL;
}

struct buffer_head *ext2_bread(struct inode *inode, block_t block, int create)
{
	register struct buffer_head *bh;

	bh = ext2_getblk(inode, block, create);
	if (!bh) {
		ext2_dbg("EXT2: getblk ino %lu lblk %u create %d on %D failed\n",
			 (unsigned long)inode->i_ino, (unsigned int)block, create,
			 inode->i_dev);
		return NULL;
	}
	bh = readbuf(bh);
	if (!bh)
		ext2_dbg("EXT2: readbuf ino %lu lblk %u on %D failed\n",
			 (unsigned long)inode->i_ino, (unsigned int)block,
			 inode->i_dev);
	return bh;
}

static void ext2_set_ops(register struct inode *inode)
{
	inode->i_rdev = NODEV;

	switch (inode->i_mode & S_IFMT) {
	case S_IFDIR:
		inode->i_op = &ext2_dir_inode_operations;
		break;
	case S_IFREG:
		inode->i_op = &ext2_file_inode_operations;
		break;
	case S_IFLNK:
		inode->i_op = &ext2_symlink_inode_operations;
		break;
	case S_IFCHR:
		inode->i_op = &chrdev_inode_operations;
		break;
	case S_IFBLK:
		inode->i_op = &blkdev_inode_operations;
		break;
	default:
		inode->i_op = NULL;
		break;
	}
}

void ext2_read_inode(register struct inode *inode)
{
	register struct buffer_head *bh;
	struct ext2_inode *raw_inode;
	__u16 links;

	bh = ext2_get_inode(inode, &raw_inode);
	if (!bh)
		return;

	inode->i_mode = raw_inode->i_mode;
	inode->i_uid = raw_inode->i_uid;
	inode->i_gid = (__u8)raw_inode->i_gid;
	inode->i_size = raw_inode->i_size;
	inode->i_atime = raw_inode->i_atime;
	inode->i_ctime = raw_inode->i_ctime;
	inode->i_mtime = raw_inode->i_mtime;

	links = raw_inode->i_links_count;
	inode->i_nlink = (links > 255) ? 255 : (__u8)links;

	ext2_set_ops(inode);
	if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode))
		inode->i_rdev = to_kdev_t((__u16)raw_inode->i_block[0]);

	unmap_brelse(bh);
}

static void ext2_write_inode(register struct inode *inode)
{
	register struct buffer_head *bh;
	struct ext2_inode *raw_inode;

	bh = ext2_get_inode(inode, &raw_inode);
	if (!bh)
		return;

	raw_inode->i_mode = inode->i_mode;
	raw_inode->i_uid = inode->i_uid;
	raw_inode->i_gid = inode->i_gid;
	raw_inode->i_size = inode->i_size;
	raw_inode->i_atime = inode->i_atime;
	raw_inode->i_ctime = inode->i_ctime;
	raw_inode->i_mtime = inode->i_mtime;
	raw_inode->i_links_count = inode->i_nlink;
	if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode))
		raw_inode->i_block[0] = kdev_t_to_nr(inode->i_rdev);
	inode->i_dirt = 0;
	mark_buffer_dirty(bh);
	unmap_brelse(bh);
}

static void ext2_write_super(register struct super_block *sb)
{
	if (!(sb->s_flags & MS_RDONLY)) {
		ext2_discard_prealloc(sb);
		ext2_commit_super(sb);
	}
	sb->s_dirt = 0;
}

static int ext2_truncate_indirect(register struct inode *inode,
				  struct ext2_inode *raw_inode,
				  block32_t *p, unsigned short start)
{
	register struct buffer_head *bh;
	__u32 *ind;
	block32_t blk;
	unsigned short i;
	int empty = 1;

	if (!*p)
		return 0;
	bh = bread32(inode->i_dev, *p);
	if (!bh) {
		return 0;
	}
	map_buffer(bh);
	ind = (__u32 *)bh->b_data;
	for (i = start; i < EXT2_ADDR_PER_BLOCK; i++) {
		if (!ind[i])
			continue;
		ext2_free_block(inode->i_sb, ind[i]);
		ind[i] = 0;
		ext2_dec_iblocks(raw_inode);
		mark_buffer_dirty(bh);
	}
	for (i = 0; i < EXT2_ADDR_PER_BLOCK; i++)
		if (ind[i]) {
			empty = 0;
			break;
		}
	unmap_brelse(bh);
	if (empty) {
		blk = *p;
		*p = 0;
		ext2_free_block(inode->i_sb, blk);
		ext2_dec_iblocks(raw_inode);
		return 1;
	}
	return 0;
}

static int ext2_truncate_dindirect(register struct inode *inode,
				   struct ext2_inode *raw_inode,
				   block32_t *p, unsigned short start)
{
	register struct buffer_head *bh;
	__u32 *ind;
	unsigned short i;
	unsigned short first;
	unsigned short offset;
	int empty = 1;
	block32_t blk;

	if (!*p)
		return 0;
	bh = bread32(inode->i_dev, *p);
	if (!bh) {
		return 0;
	}
	map_buffer(bh);
	ind = (__u32 *)bh->b_data;
	first = start / EXT2_ADDR_PER_BLOCK;
	offset = start % EXT2_ADDR_PER_BLOCK;

	for (i = first; i < EXT2_ADDR_PER_BLOCK; i++) {
		if (!ind[i])
			continue;
		if (ext2_truncate_indirect(inode, raw_inode, &ind[i],
					   (i == first) ? offset : 0))
			mark_buffer_dirty(bh);
	}
	for (i = 0; i < EXT2_ADDR_PER_BLOCK; i++)
		if (ind[i]) {
			empty = 0;
			break;
		}
	unmap_brelse(bh);
	if (empty) {
		blk = *p;
		*p = 0;
		ext2_free_block(inode->i_sb, blk);
		ext2_dec_iblocks(raw_inode);
		return 1;
	}
	return 0;
}

void ext2_truncate(register struct inode *inode)
{
	register struct buffer_head *bh;
	struct ext2_inode *raw_inode;
	block32_t ind_block;
	block32_t dind_block;
	unsigned short block;
	unsigned short i;

	if (!(S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode) ||
	      S_ISLNK(inode->i_mode)))
		return;
	ext2_discard_prealloc_inode(inode);

	bh = ext2_get_inode(inode, &raw_inode);
	if (!bh)
		return;

	block = (unsigned short)((inode->i_size + BLOCK_SIZE - 1) >> BLOCK_SIZE_BITS);
	for (i = block; i < EXT2_NDIR_BLOCKS; i++) {
		if (!raw_inode->i_block[i])
			continue;
		ext2_free_block(inode->i_sb, raw_inode->i_block[i]);
		raw_inode->i_block[i] = 0;
		ext2_dec_iblocks(raw_inode);
	}
	ind_block = raw_inode->i_block[EXT2_IND_BLOCK];
	if (block <= EXT2_NDIR_BLOCKS)
		ext2_truncate_indirect(inode, raw_inode, &ind_block, 0);
	else
		ext2_truncate_indirect(inode, raw_inode, &ind_block,
				       block - EXT2_NDIR_BLOCKS);
	raw_inode->i_block[EXT2_IND_BLOCK] = ind_block;

	dind_block = raw_inode->i_block[EXT2_DIND_BLOCK];
	if (block <= EXT2_NDIR_BLOCKS + EXT2_ADDR_PER_BLOCK)
		ext2_truncate_dindirect(inode, raw_inode, &dind_block, 0);
	else
		ext2_truncate_dindirect(inode, raw_inode, &dind_block,
					block - EXT2_NDIR_BLOCKS -
					EXT2_ADDR_PER_BLOCK);
	raw_inode->i_block[EXT2_DIND_BLOCK] = dind_block;

	inode->i_mtime = inode->i_ctime = current_time();
	inode->i_dirt = 1;
	mark_buffer_dirty(bh);
	unmap_brelse(bh);
}

static void ext2_put_inode(register struct inode *inode)
{
	ext2_discard_prealloc_inode(inode);
	if (!inode->i_nlink) {
		inode->i_size = 0;
		ext2_truncate(inode);
		ext2_free_inode(inode);
	}
}

void ext2_put_super(register struct super_block *sb)
{
	lock_super(sb);
	if (!(sb->s_flags & MS_RDONLY)) {
		ext2_discard_prealloc(sb);
		EXT2_SB(sb)->s_state = EXT2_SB(sb)->s_mount_state;
		ext2_commit_super(sb);
	}
	sb->s_dirt = 0;
	sb->s_dev = 0;
	unlock_super(sb);
}

int ext2_remount(register struct super_block *sb, int *flags, char *data)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);

	(void)data;
	if ((*flags & MS_RDONLY) == (sb->s_flags & MS_RDONLY))
		return 0;

	if (*flags & MS_RDONLY) {
		ext2_discard_prealloc(sb);
		sbi->s_state = sbi->s_mount_state;
		ext2_commit_super(sb);
		sb->s_dirt = 0;
	} else {
		sbi->s_mount_state = sbi->s_state;
		sbi->s_state &= ~EXT2_VALID_FS;
		sbi->s_mnt_count++;
		sb->s_dirt = 1;
		sync_dev(sb->s_dev);
	}
	return 0;
}

struct super_block *ext2_read_super(register struct super_block *s, char *data,
				    int silent)
{
	register struct buffer_head *bh;
	struct ext2_super_block *es;
	struct ext2_sb_info *sbi;
	unsigned long groups;
	const char *msgerr = NULL;
	static const char *err1 = "EXT2: cannot read superblock\n";
	static const char *err3 = "EXT2: cannot get root inode\n";

	(void)data;

	lock_super(s);
	bh = bread(s->s_dev, 1);
	if (!bh) {
		msgerr = err1;
		goto err_out_unlock;
	}
	map_buffer(bh);
	es = (struct ext2_super_block *)bh->b_data;

	if (es->s_magic != EXT2_SUPER_MAGIC) {
		if (!silent)
			printk("EXT2: %E (%D) not ext2 (magic 0x%x)\n",
			       s->s_dev, s->s_dev, es->s_magic);
		goto err_out_brelse;
	}

	if (es->s_rev_level != EXT2_GOOD_OLD_REV) {
		if (!silent)
			printk("EXT2: %E needs ext2 rev0 (got %u)\n",
			       s->s_dev, es->s_rev_level);
		goto err_out_brelse;
	}
	if (es->s_log_block_size != 0) {
		if (!silent)
			printk("EXT2: %E needs 1KiB blocks (log_block_size %u)\n",
			       s->s_dev, es->s_log_block_size);
		goto err_out_brelse;
	}
	if (!es->s_blocks_per_group || !es->s_inodes_per_group) {
		if (!silent)
			printk("EXT2: %E bad blocks/inodes per group\n",
			       s->s_dev);
		goto err_out_brelse;
	}

	sbi = EXT2_SB(s);
	sbi->s_inodes_count = es->s_inodes_count;
	sbi->s_blocks_count = es->s_blocks_count;
	sbi->s_r_blocks_count = es->s_r_blocks_count;
	sbi->s_free_blocks_count = es->s_free_blocks_count;
	sbi->s_free_inodes_count = es->s_free_inodes_count;
	sbi->s_first_data_block = es->s_first_data_block;
	sbi->s_blocks_per_group = es->s_blocks_per_group;
	sbi->s_inodes_per_group = es->s_inodes_per_group;
	sbi->s_desc_per_block = BLOCK_SIZE / sizeof(struct ext2_group_desc);
	sbi->s_mount_state = es->s_state;
	sbi->s_state = es->s_state;
	sbi->s_mnt_count = es->s_mnt_count;
	sbi->s_last_block_group = 0;
	sbi->s_last_inode_group = 0;
	sbi->s_inode_table_valid = 0;
	ext2_clear_prealloc(sbi);

	groups = es->s_blocks_count - es->s_first_data_block;
	groups += es->s_blocks_per_group - 1;
	groups /= es->s_blocks_per_group;
	if (!groups) {
		if (!silent)
			printk("EXT2: %E invalid block group count\n", s->s_dev);
		goto err_out_brelse;
	}
	if (groups > 0xFFFFUL) {
		if (!silent)
			printk("EXT2: %E too many block groups (max 65535)\n",
			       s->s_dev);
		goto err_out_brelse;
	}
	sbi->s_groups_count = (unsigned short)groups;

	unlock_super(s);
	s->s_op = &ext2_sops;
	s->s_mounted = iget(s, (ino_t)EXT2_ROOT_INO);
	if (!s->s_mounted) {
		lock_super(s);
		msgerr = err3;
		goto err_out_brelse;
	}

	if (!(s->s_flags & MS_RDONLY)) {
		sbi->s_state &= ~EXT2_VALID_FS;
		sbi->s_mnt_count++;
		s->s_dirt = 1;
		sync_dev(s->s_dev);
	}

	unmap_brelse(bh);
	return s;

err_out_brelse:
	unmap_brelse(bh);
err_out_unlock:
	unlock_super(s);
	if (msgerr)
		printk(msgerr);
	return NULL;
}

void ext2_statfs(struct super_block *s, struct statfs *sf, int flags)
{
	struct ext2_sb_info *sbi;
	unsigned long bavail;

	(void)flags;

	sbi = EXT2_SB(s);
	bavail = sbi->s_free_blocks_count;
	if (bavail > sbi->s_r_blocks_count)
		bavail -= sbi->s_r_blocks_count;
	else
		bavail = 0;

	sf->f_bsize = BLOCK_SIZE;
	sf->f_blocks = sbi->s_blocks_count;
	sf->f_bfree = sbi->s_free_blocks_count;
	sf->f_bavail = bavail;
	sf->f_files = sbi->s_inodes_count;
	sf->f_ffree = sbi->s_free_inodes_count;
}

struct file_system_type ext2_fs_type = {
	ext2_read_super,
	FST_EXT2
};
