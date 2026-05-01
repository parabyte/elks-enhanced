/*
 * e2fsck.c - lean ext2 checker for the ELKS ext2 subset.
 *
 * Supported layout:
 * - ext2 revision 0
 * - 1KiB blocks only
 * - no optional features
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <linuxmt/fs.h>
#include <linuxmt/ext2_fs.h>

#define EXT2_BITS_PER_BLOCK		(BLOCK_SIZE << 3)
#define EXT2_GOOD_OLD_FIRST_INO		11
#define EXT2_ERRORS_CONTINUE		1
#define EXT2_S_IFMT			0xF000

static const char *progname = "e2fsck";
static const char *device_name;
static int dev_fd = -1;
static int verbose;
static int force_check;
static int repair;
static int readonly_check;
static int changed;
static int uncorrected;
static int saw_errors;

static struct ext2_super_block super_copy;
static struct ext2_group_desc *group_desc;
static unsigned long blocks_count;
static unsigned long inodes_count;
static unsigned long blocks_per_group;
static unsigned long inodes_per_group;
static unsigned long first_data_block;
static unsigned short groups_count;
static unsigned short desc_blocks;
static unsigned short inode_table_blocks;

static unsigned char *disk_block_maps;
static unsigned char *disk_inode_maps;
static unsigned char *expect_block_map;
static unsigned char *expect_inode_map;
static unsigned char *live_inode_map;
static unsigned char *ref_counts;

static unsigned char super_block[BLOCK_SIZE];
static unsigned char inode_block[BLOCK_SIZE];
static unsigned char dir_block[BLOCK_SIZE];
static unsigned char indir1_block[BLOCK_SIZE];
static unsigned char indir2_block[BLOCK_SIZE];
static unsigned char rebuild_block[BLOCK_SIZE];
static unsigned char root_template[BLOCK_SIZE];

static void usage(void)
{
	fprintf(stderr, "Usage: %s [-apnyfv] device\n", progname);
	exit(16);
}

static void die(const char *msg)
{
	fprintf(stderr, "%s: %s\n", progname, msg);
	exit(8);
}

static void mark_error(void)
{
	saw_errors = 1;
	if (!repair)
		uncorrected = 1;
}

static void seek_block(unsigned long block)
{
	off_t off;

	off = (off_t)block << BLOCK_SIZE_BITS;
	if (lseek(dev_fd, off, SEEK_SET) != off)
		die("seek failed");
}

static void read_block(unsigned long block, void *buf)
{
	seek_block(block);
	if (read(dev_fd, buf, BLOCK_SIZE) != BLOCK_SIZE)
		die("read failed");
}

static void write_block(unsigned long block, const void *buf)
{
	if (readonly_check)
		return;
	seek_block(block);
	if (write(dev_fd, buf, BLOCK_SIZE) != BLOCK_SIZE)
		die("write failed");
}

static int test_bitmap(const unsigned char *map, unsigned long bit)
{
	return (map[bit >> 3] >> (bit & 7)) & 1;
}

static void set_bitmap(unsigned char *map, unsigned long bit)
{
	map[bit >> 3] |= (unsigned char)(1U << (bit & 7));
}

static void fill_bitmap_tail(unsigned char *map, unsigned short first_bit)
{
	unsigned short byte;

	if (first_bit >= EXT2_BITS_PER_BLOCK)
		return;

	byte = first_bit >> 3;
	if (first_bit & 7) {
		map[byte] |= (unsigned char)(0xFFU << (first_bit & 7));
		byte++;
	}
	if (byte < BLOCK_SIZE)
		memset(map + byte, 0xFF, BLOCK_SIZE - byte);
}

static unsigned short group_blocks(unsigned short group)
{
	unsigned long left;

	left = blocks_count - first_data_block;
	left -= (unsigned long)group * blocks_per_group;
	if (left > blocks_per_group)
		left = blocks_per_group;
	return (unsigned short)left;
}

static unsigned short group_inodes(unsigned short group)
{
	unsigned long left;

	left = inodes_count - (unsigned long)group * inodes_per_group;
	if (left > inodes_per_group)
		left = inodes_per_group;
	return (unsigned short)left;
}

static unsigned long group_first_block(unsigned short group)
{
	return first_data_block + (unsigned long)group * blocks_per_group;
}

static unsigned short metadata_blocks(void)
{
	return (unsigned short)(1 + desc_blocks + 1 + 1 + inode_table_blocks);
}

static void inode_location(unsigned long ino, unsigned long *block,
			   unsigned short *offset)
{
	unsigned long group;
	unsigned long index;

	group = (ino - 1UL) / inodes_per_group;
	index = (ino - 1UL) % inodes_per_group;
	*block = group_desc[group].bg_inode_table +
		(index / EXT2_INODES_PER_BLOCK);
	*offset = (unsigned short)(index % EXT2_INODES_PER_BLOCK);
}

static void read_inode_raw(unsigned long ino, struct ext2_inode *raw)
{
	unsigned long block;
	unsigned short offset;

	inode_location(ino, &block, &offset);
	read_block(block, inode_block);
	memcpy(raw,
	       ((struct ext2_inode *)inode_block) + offset,
	       sizeof(*raw));
}

static void write_inode_raw(unsigned long ino, const struct ext2_inode *raw)
{
	unsigned long block;
	unsigned short offset;

	if (readonly_check)
		return;
	inode_location(ino, &block, &offset);
	read_block(block, inode_block);
	memcpy(((struct ext2_inode *)inode_block) + offset,
	       raw, sizeof(*raw));
	write_block(block, inode_block);
}

static int inode_has_payload(const struct ext2_inode *raw)
{
	unsigned short i;

	if (raw->i_mode || raw->i_links_count || raw->i_size || raw->i_blocks)
		return 1;
	for (i = 0; i < EXT2_N_BLOCKS; i++)
		if (raw->i_block[i])
			return 1;
	return 0;
}

static int inode_is_live(unsigned long ino, const struct ext2_inode *raw)
{
	if (ino == EXT2_ROOT_INO)
		return raw->i_mode != 0;
	return raw->i_mode != 0 && raw->i_links_count != 0;
}

static unsigned short inode_file_type(const struct ext2_inode *raw)
{
	return raw->i_mode & EXT2_S_IFMT;
}

static int inode_stores_blocks(const struct ext2_inode *raw)
{
	switch (inode_file_type(raw)) {
	case EXT2_S_IFREG:
	case EXT2_S_IFDIR:
		return 1;
	case EXT2_S_IFLNK:
		return raw->i_blocks != 0;
	default:
		return 0;
	}
}

static int dirent_ok(const struct ext2_dir_entry *de, unsigned short off)
{
	unsigned short rl;
	unsigned short nl;

	rl = de->rec_len;
	nl = de->name_len;
	if (rl < 8 || rl > (unsigned short)(BLOCK_SIZE - off) || (rl & 3))
		return 0;
	if (de->inode && nl == 0)
		return 0;
	if (nl > EXT2_NAME_LEN)
		return 0;
	if (de->inode && EXT2_DIR_REC_LEN(nl) > rl)
		return 0;
	return 1;
}

static int claim_block_ptr(__u32 *p, unsigned long *sectors,
			   unsigned long ino, const char *what)
{
	unsigned long blk;

	blk = *p;
	if (!blk)
		return 0;
	if (blk < first_data_block || blk >= blocks_count) {
		printf("%s: inode %lu has out-of-range %s block %lu\n",
		       progname, ino, what, blk);
		mark_error();
		if (repair) {
			*p = 0;
			changed = 1;
		}
		return 0;
	}
	if (test_bitmap(expect_block_map, blk)) {
		printf("%s: inode %lu reuses %s block %lu\n",
		       progname, ino, what, blk);
		mark_error();
		if (repair) {
			*p = 0;
			changed = 1;
		}
		return 0;
	}
	set_bitmap(expect_block_map, blk);
	*sectors += BLOCK_SIZE >> 9;
	return 1;
}

static int scan_single_indirect_block(unsigned long blocknr,
				      unsigned long *sectors,
				      unsigned long ino)
{
	__u32 *entry;
	unsigned short i;
	int dirty;
	unsigned long old;

	read_block(blocknr, indir1_block);
	entry = (__u32 *)indir1_block;
	dirty = 0;
	for (i = 0; i < EXT2_ADDR_PER_BLOCK; i++) {
		old = entry[i];
		(void)claim_block_ptr(&entry[i], sectors, ino, "data");
		if (entry[i] != old)
			dirty = 1;
	}
	if (dirty && repair)
		write_block(blocknr, indir1_block);
	return dirty;
}

static int scan_single_indirect(__u32 *p, unsigned long *sectors,
				unsigned long ino)
{
	unsigned long old;
	unsigned long blocknr;
	int dirty;

	old = *p;
	if (!claim_block_ptr(p, sectors, ino, "indirect"))
		return *p != old;
	blocknr = *p;
	dirty = scan_single_indirect_block(blocknr, sectors, ino);
	return dirty || (*p != old);
}

static int scan_double_indirect(__u32 *p, unsigned long *sectors,
				unsigned long ino)
{
	__u32 *entry;
	unsigned short i;
	int dirty;
	unsigned long old;
	unsigned long blocknr;

	old = *p;
	if (!claim_block_ptr(p, sectors, ino, "double-indirect"))
		return *p != old;
	blocknr = *p;
	read_block(blocknr, indir2_block);
	entry = (__u32 *)indir2_block;
	dirty = 0;
	for (i = 0; i < EXT2_ADDR_PER_BLOCK; i++) {
		old = entry[i];
		if (claim_block_ptr(&entry[i], sectors, ino, "indirect")) {
			if (scan_single_indirect_block(entry[i], sectors, ino))
				dirty = 1;
		}
		if (entry[i] != old)
			dirty = 1;
	}
	if (dirty && repair)
		write_block(blocknr, indir2_block);
	return dirty || (*p != old);
}

static void init_root_template(unsigned long root_blocknr)
{
	struct ext2_dir_entry *de;

	memset(root_template, 0, sizeof(root_template));
	de = (struct ext2_dir_entry *)root_template;
	de->inode = EXT2_ROOT_INO;
	de->rec_len = EXT2_DIR_REC_LEN(1);
	de->name_len = 1;
	memcpy(de->name, ".", 1);
	de = (struct ext2_dir_entry *)(root_template + EXT2_DIR_REC_LEN(1));
	de->inode = EXT2_ROOT_INO;
	de->rec_len = BLOCK_SIZE - EXT2_DIR_REC_LEN(1);
	de->name_len = 2;
	memcpy(de->name, "..", 2);
	write_block(root_blocknr, root_template);
}

static int ensure_root_inode(struct ext2_inode *raw)
{
	unsigned long root_blocknr;
	time_t now;

	if (raw->i_mode == (EXT2_S_IFDIR | 0755) &&
	    raw->i_links_count >= 2 &&
	    raw->i_block[0] >= first_data_block &&
	    raw->i_block[0] < blocks_count)
		return 0;

	printf("%s: root inode is invalid\n", progname);
	mark_error();
	if (!repair)
		return 0;

	root_blocknr = group_desc[0].bg_inode_table + inode_table_blocks;
	now = time(NULL);
	memset(raw, 0, sizeof(*raw));
	raw->i_mode = EXT2_S_IFDIR | 0755;
	raw->i_size = BLOCK_SIZE;
	raw->i_atime = now;
	raw->i_ctime = now;
	raw->i_mtime = now;
	raw->i_links_count = 2;
	raw->i_blocks = BLOCK_SIZE >> 9;
	raw->i_block[0] = root_blocknr;
	init_root_template(root_blocknr);
	changed = 1;
	return 1;
}

static void scan_inode_pass1(unsigned long ino)
{
	struct ext2_inode raw;
	unsigned long sectors;
	int dirty;
	int bitmap_alloc;
	unsigned short group;
	unsigned short bit;

	read_inode_raw(ino, &raw);
	group = (unsigned short)((ino - 1UL) / inodes_per_group);
	bit = (unsigned short)((ino - 1UL) % inodes_per_group);
	bitmap_alloc = test_bitmap(disk_inode_maps + ((unsigned long)group * BLOCK_SIZE),
				  bit);

	if (ino == EXT2_ROOT_INO && ensure_root_inode(&raw))
		write_inode_raw(ino, &raw);

	if (ino <= EXT2_GOOD_OLD_FIRST_INO - 1) {
		set_bitmap(expect_inode_map, ino);
		if (ino == EXT2_ROOT_INO)
			set_bitmap(live_inode_map, ino);
		else
			return;
	}

	if (!inode_is_live(ino, &raw)) {
		if (bitmap_alloc || inode_has_payload(&raw)) {
			printf("%s: inode %lu is allocated but empty\n",
			       progname, ino);
			mark_error();
			if (repair) {
				memset(&raw, 0, sizeof(raw));
				write_inode_raw(ino, &raw);
				changed = 1;
			}
		}
		return;
	}

	set_bitmap(expect_inode_map, ino);
	set_bitmap(live_inode_map, ino);

	if (!inode_stores_blocks(&raw)) {
		if (raw.i_blocks) {
			printf("%s: inode %lu i_blocks %lu should be 0\n",
			       progname, ino, (unsigned long)raw.i_blocks);
			mark_error();
			if (repair) {
				raw.i_blocks = 0;
				write_inode_raw(ino, &raw);
				changed = 1;
			}
		}
		return;
	}

	sectors = 0;
	dirty = 0;

	{
		unsigned short i;
		unsigned long old;

		for (i = 0; i < EXT2_NDIR_BLOCKS; i++) {
			old = raw.i_block[i];
			(void)claim_block_ptr(&raw.i_block[i], &sectors, ino, "data");
			if (raw.i_block[i] != old)
				dirty = 1;
		}

		old = raw.i_block[EXT2_IND_BLOCK];
		if (scan_single_indirect(&raw.i_block[EXT2_IND_BLOCK],
					 &sectors, ino))
			dirty = 1;
		if (raw.i_block[EXT2_IND_BLOCK] != old)
			dirty = 1;

		old = raw.i_block[EXT2_DIND_BLOCK];
		if (scan_double_indirect(&raw.i_block[EXT2_DIND_BLOCK],
					 &sectors, ino))
			dirty = 1;
		if (raw.i_block[EXT2_DIND_BLOCK] != old)
			dirty = 1;
	}

	if (raw.i_blocks != sectors) {
		printf("%s: inode %lu i_blocks %lu should be %lu\n",
		       progname, ino, (unsigned long)raw.i_blocks, sectors);
		mark_error();
		if (repair) {
			raw.i_blocks = sectors;
			dirty = 1;
		}
	}

	if (dirty && repair) {
		write_inode_raw(ino, &raw);
		changed = 1;
	}
}

static void scan_dir_block(unsigned long dir_ino, unsigned long blocknr,
			   unsigned short limit, int *have_dot,
			   int *have_dotdot)
{
	struct ext2_dir_entry *de;
	unsigned short off;
	unsigned short reclen;
	int dirty;

	if (!blocknr)
		return;

	read_block(blocknr, dir_block);
	dirty = 0;
	off = 0;
	while (off < limit) {
		de = (struct ext2_dir_entry *)(dir_block + off);
		if (!dirent_ok(de, off)) {
			printf("%s: directory inode %lu has invalid entry at block %lu offset %u\n",
			       progname, dir_ino, blocknr, off);
			mark_error();
			if (repair) {
				de->inode = 0;
				de->rec_len = BLOCK_SIZE - off;
				de->name_len = 0;
				dirty = 1;
			}
			break;
		}
		reclen = de->rec_len;
		if (de->inode) {
			if (de->inode > inodes_count ||
			    !test_bitmap(live_inode_map, de->inode)) {
				printf("%s: directory inode %lu references bad inode %lu\n",
				       progname, dir_ino, (unsigned long)de->inode);
				mark_error();
				if (repair) {
					de->inode = 0;
					dirty = 1;
				}
			} else {
				if (de->name_len == 1 && de->name[0] == '.') {
					*have_dot = 1;
					if (de->inode != dir_ino) {
						printf("%s: directory inode %lu has bad '.' entry\n",
						       progname, dir_ino);
						mark_error();
						if (repair) {
							de->inode = dir_ino;
							dirty = 1;
						}
					}
				} else if (de->name_len == 2 &&
					   de->name[0] == '.' &&
					   de->name[1] == '.') {
					*have_dotdot = 1;
					if (dir_ino == EXT2_ROOT_INO &&
					    de->inode != EXT2_ROOT_INO) {
						printf("%s: root directory has bad '..' entry\n",
						       progname);
						mark_error();
						if (repair) {
							de->inode = EXT2_ROOT_INO;
							dirty = 1;
						}
					}
				}
				ref_counts[de->inode]++;
			}
		}
		off += reclen;
	}

	if (dirty && repair) {
		write_block(blocknr, dir_block);
		changed = 1;
	}
}

static void consume_dir_block(unsigned long dir_ino, unsigned long blocknr,
			      unsigned long *remaining, int *have_dot,
			      int *have_dotdot)
{
	unsigned short limit;

	if (!*remaining)
		return;
	limit = (*remaining > BLOCK_SIZE) ? BLOCK_SIZE : (unsigned short)*remaining;
	if (blocknr)
		scan_dir_block(dir_ino, blocknr, limit, have_dot, have_dotdot);
	*remaining -= limit;
}

static void walk_dir_single(unsigned long dir_ino, unsigned long blocknr,
			    unsigned long *remaining, int *have_dot,
			    int *have_dotdot)
{
	__u32 *entry;
	unsigned short i;

	if (!blocknr || !*remaining)
		return;
	read_block(blocknr, indir1_block);
	entry = (__u32 *)indir1_block;
	for (i = 0; i < EXT2_ADDR_PER_BLOCK && *remaining; i++)
		consume_dir_block(dir_ino, entry[i], remaining,
				  have_dot, have_dotdot);
}

static void walk_dir_double(unsigned long dir_ino, unsigned long blocknr,
			    unsigned long *remaining, int *have_dot,
			    int *have_dotdot)
{
	__u32 *entry;
	unsigned short i;

	if (!blocknr || !*remaining)
		return;
	read_block(blocknr, indir2_block);
	entry = (__u32 *)indir2_block;
	for (i = 0; i < EXT2_ADDR_PER_BLOCK && *remaining; i++)
		walk_dir_single(dir_ino, entry[i], remaining,
				have_dot, have_dotdot);
}

static void scan_inode_pass2(unsigned long ino)
{
	struct ext2_inode raw;
	unsigned long remaining;
	unsigned short i;
	int have_dot;
	int have_dotdot;

	if (!test_bitmap(live_inode_map, ino))
		return;
	read_inode_raw(ino, &raw);
	if (inode_file_type(&raw) != EXT2_S_IFDIR)
		return;

	have_dot = 0;
	have_dotdot = 0;
	remaining = raw.i_size;
	for (i = 0; i < EXT2_NDIR_BLOCKS && remaining; i++)
		consume_dir_block(ino, raw.i_block[i], &remaining,
				  &have_dot, &have_dotdot);
	if (remaining)
		walk_dir_single(ino, raw.i_block[EXT2_IND_BLOCK], &remaining,
				&have_dot, &have_dotdot);
	if (remaining)
		walk_dir_double(ino, raw.i_block[EXT2_DIND_BLOCK], &remaining,
				&have_dot, &have_dotdot);

	if (!have_dot || !have_dotdot) {
		printf("%s: directory inode %lu is missing '.' or '..'\n",
		       progname, ino);
		mark_error();
	}
}

static void scan_inode_pass3(unsigned long ino)
{
	struct ext2_inode raw;
	unsigned long refs;

	if (!test_bitmap(live_inode_map, ino))
		return;
	read_inode_raw(ino, &raw);
	refs = ref_counts[ino];
	if (ino != EXT2_ROOT_INO && refs == 0) {
		printf("%s: inode %lu is unreferenced\n", progname, ino);
		mark_error();
		return;
	}
	if (raw.i_links_count != refs) {
		printf("%s: inode %lu links %u should be %lu\n",
		       progname, ino, raw.i_links_count, refs);
		mark_error();
		if (repair) {
			raw.i_links_count = refs;
			write_inode_raw(ino, &raw);
			changed = 1;
		}
	}
}

static unsigned short count_group_blocks_used(unsigned short group)
{
	unsigned short used;
	unsigned short i;
	unsigned long first;

	used = 0;
	first = group_first_block(group);
	for (i = 0; i < group_blocks(group); i++)
		if (test_bitmap(expect_block_map, first + i))
			used++;
	return used;
}

static unsigned short count_group_inodes_used(unsigned short group)
{
	unsigned short used;
	unsigned short i;
	unsigned long first;

	used = 0;
	first = (unsigned long)group * inodes_per_group + 1UL;
	for (i = 0; i < group_inodes(group); i++)
		if (test_bitmap(expect_inode_map, first + i))
			used++;
	return used;
}

static void rebuild_group_bitmaps(void)
{
	unsigned short group;
	unsigned short i;
	unsigned short used;
	unsigned long first;
	unsigned long total_free_blocks;
	unsigned long total_free_inodes;

	total_free_blocks = 0;
	total_free_inodes = 0;
	for (group = 0; group < groups_count; group++) {
		memset(rebuild_block, 0, BLOCK_SIZE);
		first = group_first_block(group);
		for (i = 0; i < group_blocks(group); i++)
			if (test_bitmap(expect_block_map, first + i))
				set_bitmap(rebuild_block, i);
		fill_bitmap_tail(rebuild_block, group_blocks(group));
		if (memcmp(rebuild_block,
			   disk_block_maps + ((unsigned long)group * BLOCK_SIZE),
			   BLOCK_SIZE)) {
			printf("%s: block bitmap mismatch in group %u\n",
			       progname, group);
			mark_error();
			if (repair) {
				memcpy(disk_block_maps + ((unsigned long)group * BLOCK_SIZE),
				       rebuild_block, BLOCK_SIZE);
				write_block(group_desc[group].bg_block_bitmap, rebuild_block);
				changed = 1;
			}
		}

		memset(rebuild_block, 0, BLOCK_SIZE);
		first = (unsigned long)group * inodes_per_group + 1UL;
		for (i = 0; i < group_inodes(group); i++)
			if (test_bitmap(expect_inode_map, first + i))
				set_bitmap(rebuild_block, i);
		fill_bitmap_tail(rebuild_block, group_inodes(group));
		if (memcmp(rebuild_block,
			   disk_inode_maps + ((unsigned long)group * BLOCK_SIZE),
			   BLOCK_SIZE)) {
			printf("%s: inode bitmap mismatch in group %u\n",
			       progname, group);
			mark_error();
			if (repair) {
				memcpy(disk_inode_maps + ((unsigned long)group * BLOCK_SIZE),
				       rebuild_block, BLOCK_SIZE);
				write_block(group_desc[group].bg_inode_bitmap, rebuild_block);
				changed = 1;
			}
		}

		used = count_group_blocks_used(group);
		if (group_desc[group].bg_free_blocks_count !=
		    (unsigned short)(group_blocks(group) - used)) {
			printf("%s: group %u free block count %u should be %u\n",
			       progname, group,
			       group_desc[group].bg_free_blocks_count,
			       (unsigned short)(group_blocks(group) - used));
			mark_error();
			if (repair) {
				group_desc[group].bg_free_blocks_count =
					group_blocks(group) - used;
				changed = 1;
			}
		}
		total_free_blocks += group_desc[group].bg_free_blocks_count;

		used = count_group_inodes_used(group);
		if (group_desc[group].bg_free_inodes_count !=
		    (unsigned short)(group_inodes(group) - used)) {
			printf("%s: group %u free inode count %u should be %u\n",
			       progname, group,
			       group_desc[group].bg_free_inodes_count,
			       (unsigned short)(group_inodes(group) - used));
			mark_error();
			if (repair) {
				group_desc[group].bg_free_inodes_count =
					group_inodes(group) - used;
				changed = 1;
			}
		}
		total_free_inodes += group_desc[group].bg_free_inodes_count;
	}

	if (super_copy.s_free_blocks_count != total_free_blocks) {
		printf("%s: free block count %lu should be %lu\n",
		       progname,
		       (unsigned long)super_copy.s_free_blocks_count,
		       total_free_blocks);
		mark_error();
		if (repair) {
			super_copy.s_free_blocks_count = total_free_blocks;
			changed = 1;
		}
	}
	if (super_copy.s_free_inodes_count != total_free_inodes) {
		printf("%s: free inode count %lu should be %lu\n",
		       progname,
		       (unsigned long)super_copy.s_free_inodes_count,
		       total_free_inodes);
		mark_error();
		if (repair) {
			super_copy.s_free_inodes_count = total_free_inodes;
			changed = 1;
		}
	}
}

static void write_group_desc_table(unsigned long block)
{
	unsigned short i;
	unsigned short offset;
	unsigned short left;

	for (i = 0; i < desc_blocks; i++) {
		memset(rebuild_block, 0, BLOCK_SIZE);
		offset = i * (BLOCK_SIZE / sizeof(struct ext2_group_desc));
		if (offset < groups_count) {
			left = groups_count - offset;
			if (left > BLOCK_SIZE / sizeof(struct ext2_group_desc))
				left = BLOCK_SIZE / sizeof(struct ext2_group_desc);
			memcpy(rebuild_block, group_desc + offset,
			       left * sizeof(struct ext2_group_desc));
		}
		write_block(block + i, rebuild_block);
	}
}

static void write_super_and_desc(void)
{
	unsigned short group;
	time_t now;

	if (readonly_check)
		return;

	now = time(NULL);
	memcpy(super_block, &super_copy, sizeof(super_copy));
	super_copy.s_state = EXT2_VALID_FS;
	super_copy.s_errors = EXT2_ERRORS_CONTINUE;
	super_copy.s_wtime = now;
	memcpy(super_block, &super_copy, sizeof(super_copy));
	write_block(1, super_block);
	write_group_desc_table(2);

	for (group = 1; group < groups_count; group++) {
		write_block(group_first_block(group), super_block);
		write_group_desc_table(group_first_block(group) + 1UL);
	}
}

static void load_super(void)
{
	unsigned long groups;
	unsigned short i;

	read_block(1, super_block);
	memcpy(&super_copy, super_block, sizeof(super_copy));
	if (super_copy.s_magic != EXT2_SUPER_MAGIC)
		die("not an ext2 filesystem");
	if (super_copy.s_rev_level != EXT2_GOOD_OLD_REV)
		die("ext2 revision 0 is required");
	if (super_copy.s_log_block_size != 0)
		die("1KiB block size is required");
	if (!super_copy.s_blocks_per_group || !super_copy.s_inodes_per_group)
		die("bad superblock");

	blocks_count = super_copy.s_blocks_count;
	inodes_count = super_copy.s_inodes_count;
	blocks_per_group = super_copy.s_blocks_per_group;
	inodes_per_group = super_copy.s_inodes_per_group;
	first_data_block = super_copy.s_first_data_block;

	groups = blocks_count - first_data_block;
	groups += blocks_per_group - 1;
	groups /= blocks_per_group;
	if (!groups || groups > 0xFFFFUL)
		die("bad block group count");
	groups_count = (unsigned short)groups;
	desc_blocks = (unsigned short)
		((groups_count * sizeof(struct ext2_group_desc) + BLOCK_SIZE - 1) /
		 BLOCK_SIZE);
	inode_table_blocks = (unsigned short)
		((inodes_per_group + EXT2_INODES_PER_BLOCK - 1) /
		 EXT2_INODES_PER_BLOCK);

	group_desc = calloc(groups_count, sizeof(*group_desc));
	if (!group_desc)
		die("out of memory");

	for (i = 0; i < desc_blocks; i++) {
		read_block(2 + i, rebuild_block);
		memcpy(((unsigned char *)group_desc) + ((unsigned long)i * BLOCK_SIZE),
		       rebuild_block,
		       BLOCK_SIZE);
	}
}

static void load_bitmaps(void)
{
	unsigned short group;
	unsigned long block_bytes;
	unsigned long inode_bytes;

	disk_block_maps = malloc((unsigned long)groups_count * BLOCK_SIZE);
	disk_inode_maps = malloc((unsigned long)groups_count * BLOCK_SIZE);
	block_bytes = (blocks_count + 7UL) >> 3;
	inode_bytes = (inodes_count + 8UL) >> 3;
	expect_block_map = calloc(block_bytes, 1);
	expect_inode_map = calloc(inode_bytes, 1);
	live_inode_map = calloc(inode_bytes, 1);
	ref_counts = calloc(inodes_count + 1UL, 1);
	if (!disk_block_maps || !disk_inode_maps || !expect_block_map ||
	    !expect_inode_map || !live_inode_map || !ref_counts)
		die("out of memory");

	for (group = 0; group < groups_count; group++) {
		read_block(group_desc[group].bg_block_bitmap,
			   disk_block_maps + ((unsigned long)group * BLOCK_SIZE));
		read_block(group_desc[group].bg_inode_bitmap,
			   disk_inode_maps + ((unsigned long)group * BLOCK_SIZE));
	}
}

static void mark_metadata(void)
{
	unsigned short group;
	unsigned short i;
	unsigned short meta;
	unsigned long first;

	meta = metadata_blocks();
	for (group = 0; group < groups_count; group++) {
		first = group_first_block(group);
		for (i = 0; i < meta && i < group_blocks(group); i++)
			set_bitmap(expect_block_map, first + i);
	}
	for (i = 1; i < EXT2_GOOD_OLD_FIRST_INO; i++)
		set_bitmap(expect_inode_map, i);
}

static void parse_args(int argc, char **argv)
{
	progname = strrchr(argv[0], '/');
	progname = progname ? progname + 1 : argv[0];

	argc--;
	argv++;
	while (argc > 0 && argv[0][0] == '-') {
		if (!strcmp(argv[0], "-a") || !strcmp(argv[0], "-p") ||
		    !strcmp(argv[0], "-y")) {
			repair = 1;
			argc--;
			argv++;
			continue;
		}
		if (!strcmp(argv[0], "-n")) {
			readonly_check = 1;
			repair = 0;
			argc--;
			argv++;
			continue;
		}
		if (!strcmp(argv[0], "-f")) {
			force_check = 1;
			argc--;
			argv++;
			continue;
		}
		if (!strcmp(argv[0], "-v")) {
			verbose = 1;
			argc--;
			argv++;
			continue;
		}
		usage();
	}

	if (argc != 1)
		usage();
	device_name = argv[0];
	dev_fd = open(device_name, readonly_check ? O_RDONLY : O_RDWR);
	if (dev_fd < 0)
		die("unable to open device");
}

int main(int argc, char **argv)
{
	unsigned long ino;

	parse_args(argc, argv);
	load_super();

	if (!force_check &&
	    (super_copy.s_state & EXT2_VALID_FS) &&
	    !(super_copy.s_state & EXT2_ERROR_FS)) {
		if (verbose)
			printf("%s: %s is clean\n", progname, device_name);
		close(dev_fd);
		return 0;
	}

	load_bitmaps();
	mark_metadata();

	for (ino = 1; ino <= inodes_count; ino++)
		scan_inode_pass1(ino);
	for (ino = 1; ino <= inodes_count; ino++)
		scan_inode_pass2(ino);
	for (ino = 1; ino <= inodes_count; ino++)
		scan_inode_pass3(ino);

	rebuild_group_bitmaps();
	if (repair && !uncorrected)
		write_super_and_desc();

	if (verbose) {
		printf("%s: blocks=%lu inodes=%lu\n",
		       progname, blocks_count, inodes_count);
		if (changed)
			printf("%s: filesystem modified\n", progname);
	}

	close(dev_fd);
	if (uncorrected)
		return 4;
	return saw_errors ? (repair ? 1 : 4) : 0;
}
