/*
 * mkfs_ext2.c - lean ext2 formatter for the ELKS ext2 subset.
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
#define EXT2_LABEL_OFFSET		120
#define EXT2_LABEL_LEN			16

static const char *progname = "mkfs.ext2";
static const char *device_name;
static int dev_fd = -1;
static unsigned long blocks_count;
static unsigned long blocks_per_group = EXT2_BITS_PER_BLOCK;
static unsigned long inodes_count;
static unsigned short groups_count;
static unsigned short desc_blocks;
static unsigned short inodes_per_group;
static unsigned short inode_table_blocks;
static unsigned short reserve_pct;
static char volume_label[EXT2_LABEL_LEN + 1];
static struct ext2_group_desc *group_desc;

static unsigned char zero_block[BLOCK_SIZE];
static unsigned char bitmap_block[BLOCK_SIZE];
static unsigned char super_block[BLOCK_SIZE];
static unsigned char root_block[BLOCK_SIZE];

static void die(const char *msg)
{
	fprintf(stderr, "%s: %s\n", progname, msg);
	exit(1);
}

static void usage(void)
{
	fprintf(stderr,
		"Usage: %s [-F] [-b 1024] [-m pct] [-N inodes] [-L label] [-E revision=0] device [blocks]\n",
		progname);
	exit(16);
}

static unsigned short round_up_short(unsigned short val, unsigned short mult)
{
	if (!mult)
		return val;
	val += mult - 1;
	val /= mult;
	val *= mult;
	return val;
}

static unsigned short group_blocks(unsigned short group)
{
	unsigned long left;

	left = blocks_count - 1UL;
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
	return 1UL + (unsigned long)group * blocks_per_group;
}

static void seek_block(unsigned long block)
{
	off_t off;

	off = (off_t)block << BLOCK_SIZE_BITS;
	if (lseek(dev_fd, off, SEEK_SET) != off)
		die("seek failed");
}

static void write_block(unsigned long block, const void *buf)
{
	seek_block(block);
	if (write(dev_fd, buf, BLOCK_SIZE) != BLOCK_SIZE)
		die("write failed");
}

static void set_bitmap(unsigned char *map, unsigned short bit)
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

static unsigned long detect_blocks(void)
{
	struct stat st;
	unsigned long blocks;
	off_t off;

	if (fstat(dev_fd, &st) < 0)
		die("unable to stat device");
	blocks = (unsigned long)(st.st_size >> BLOCK_SIZE_BITS);
	if (blocks)
		return blocks;

	off = lseek(dev_fd, 0L, SEEK_END);
	if (off <= 0)
		die("unable to determine device size");
	return (unsigned long)(off >> BLOCK_SIZE_BITS);
}

static void parse_args(int argc, char **argv)
{
	char *endp;
	unsigned long req_inodes = 0;

	progname = strrchr(argv[0], '/');
	progname = progname ? progname + 1 : argv[0];

	argc--;
	argv++;
	while (argc > 0 && argv[0][0] == '-') {
		if (!strcmp(argv[0], "-F")) {
			argc--;
			argv++;
			continue;
		}
		if (!strcmp(argv[0], "-b")) {
			if (argc < 2)
				usage();
			if (strcmp(argv[1], "1024"))
				die("only 1KiB block size is supported");
			argc -= 2;
			argv += 2;
			continue;
		}
		if (!strcmp(argv[0], "-m")) {
			if (argc < 2)
				usage();
			reserve_pct = (unsigned short)strtoul(argv[1], &endp, 0);
			if (*endp || reserve_pct > 99)
				usage();
			argc -= 2;
			argv += 2;
			continue;
		}
		if (!strcmp(argv[0], "-N")) {
			if (argc < 2)
				usage();
			req_inodes = strtoul(argv[1], &endp, 0);
			if (*endp || !req_inodes)
				usage();
			argc -= 2;
			argv += 2;
			continue;
		}
		if (!strcmp(argv[0], "-L")) {
			if (argc < 2)
				usage();
			strncpy(volume_label, argv[1], EXT2_LABEL_LEN);
			volume_label[EXT2_LABEL_LEN] = '\0';
			argc -= 2;
			argv += 2;
			continue;
		}
		if (!strcmp(argv[0], "-E")) {
			if (argc < 2)
				usage();
			if (strcmp(argv[1], "revision=0"))
				die("only ext2 revision 0 is supported");
			argc -= 2;
			argv += 2;
			continue;
		}
		usage();
	}

	if (argc < 1 || argc > 2)
		usage();

	device_name = argv[0];
	if (argc == 2) {
		blocks_count = strtoul(argv[1], &endp, 0);
		if (*endp)
			usage();
	}

	dev_fd = open(device_name, O_RDWR);
	if (dev_fd < 0)
		die("unable to open device");

	if (!blocks_count)
		blocks_count = detect_blocks();
	if (blocks_count < 32)
		die("filesystem is too small");

	groups_count = (unsigned short)((blocks_count - 1UL + blocks_per_group - 1UL) /
					 blocks_per_group);
	if (!groups_count)
		die("invalid block count");

	if (!req_inodes)
		req_inodes = blocks_count >> 3;
	if (req_inodes < EXT2_GOOD_OLD_FIRST_INO)
		req_inodes = EXT2_GOOD_OLD_FIRST_INO;

	inodes_per_group = (unsigned short)
		((req_inodes + groups_count - 1UL) / groups_count);
	inodes_per_group = round_up_short(inodes_per_group, EXT2_INODES_PER_BLOCK);
	if (!inodes_per_group || inodes_per_group > EXT2_BITS_PER_BLOCK)
		die("inode count is out of range");

	inodes_count = (unsigned long)inodes_per_group * groups_count;
	inode_table_blocks = (unsigned short)
		((inodes_per_group + EXT2_INODES_PER_BLOCK - 1) /
		 EXT2_INODES_PER_BLOCK);

	desc_blocks = (unsigned short)
		((groups_count * sizeof(struct ext2_group_desc) + BLOCK_SIZE - 1) /
		 BLOCK_SIZE);
}

static unsigned short metadata_blocks(void)
{
	return (unsigned short)(1 + desc_blocks + 1 + 1 + inode_table_blocks);
}

static void build_group_descs(void)
{
	unsigned short group;
	unsigned short gblocks;
	unsigned short ginodes;
	unsigned short meta;
	unsigned short used_inodes;
	unsigned long free_blocks;
	unsigned long free_inodes;
	unsigned long reserved;

	group_desc = calloc(groups_count, sizeof(*group_desc));
	if (!group_desc)
		die("out of memory");

	meta = metadata_blocks();
	free_blocks = 0;
	free_inodes = 0;

	for (group = 0; group < groups_count; group++) {
		gblocks = group_blocks(group);
		ginodes = group_inodes(group);
		if (gblocks <= meta + (group == 0 ? 1 : 0))
			die("filesystem is too small for metadata");

		group_desc[group].bg_block_bitmap =
			group_first_block(group) + 1UL + desc_blocks;
		group_desc[group].bg_inode_bitmap =
			group_desc[group].bg_block_bitmap + 1UL;
		group_desc[group].bg_inode_table =
			group_desc[group].bg_inode_bitmap + 1UL;

		used_inodes = 0;
		if (group == 0) {
			used_inodes = EXT2_GOOD_OLD_FIRST_INO - 1;
			if (used_inodes > ginodes)
				used_inodes = ginodes;
		}

		group_desc[group].bg_free_blocks_count =
			gblocks - meta - (group == 0 ? 1 : 0);
		group_desc[group].bg_free_inodes_count = ginodes - used_inodes;
		group_desc[group].bg_used_dirs_count = (group == 0) ? 1 : 0;
		free_blocks += group_desc[group].bg_free_blocks_count;
		free_inodes += group_desc[group].bg_free_inodes_count;
	}

	memset(super_block, 0, sizeof(super_block));
	{
		struct ext2_super_block *es;
		time_t now;

		now = time(NULL);
		es = (struct ext2_super_block *)super_block;
		es->s_inodes_count = inodes_count;
		es->s_blocks_count = blocks_count;
		reserved = (blocks_count - 1UL) * reserve_pct / 100UL;
		if (reserved > free_blocks)
			reserved = free_blocks;
		es->s_r_blocks_count = reserved;
		es->s_free_blocks_count = free_blocks;
		es->s_free_inodes_count = free_inodes;
		es->s_first_data_block = 1;
		es->s_log_block_size = 0;
		es->s_log_frag_size = 0;
		es->s_blocks_per_group = blocks_per_group;
		es->s_frags_per_group = blocks_per_group;
		es->s_inodes_per_group = inodes_per_group;
		es->s_mtime = 0;
		es->s_wtime = now;
		es->s_mnt_count = 0;
		es->s_max_mnt_count = (__s16)-1;
		es->s_magic = EXT2_SUPER_MAGIC;
		es->s_state = EXT2_VALID_FS;
		es->s_errors = EXT2_ERRORS_CONTINUE;
		es->s_lastcheck = now;
		es->s_checkinterval = 0;
		es->s_creator_os = 0;
		es->s_rev_level = EXT2_GOOD_OLD_REV;
	}

	if (volume_label[0]) {
		memset(super_block + EXT2_LABEL_OFFSET, 0, EXT2_LABEL_LEN);
		memcpy(super_block + EXT2_LABEL_OFFSET, volume_label,
		       strlen(volume_label));
	}
}

static void write_group_desc_table(unsigned long block)
{
	unsigned short i;
	unsigned short copied;

	for (i = 0; i < desc_blocks; i++) {
		memset(bitmap_block, 0, sizeof(bitmap_block));
		copied = BLOCK_SIZE / sizeof(struct ext2_group_desc);
		copied *= i;
		if (copied < groups_count) {
			unsigned short left;
			left = groups_count - copied;
			if (left > BLOCK_SIZE / sizeof(struct ext2_group_desc))
				left = BLOCK_SIZE / sizeof(struct ext2_group_desc);
			memcpy(bitmap_block,
			       group_desc + copied,
			       left * sizeof(struct ext2_group_desc));
		}
		write_block(block + i, bitmap_block);
	}
}

static void write_super_and_backups(void)
{
	unsigned short group;

	write_block(1, super_block);
	write_group_desc_table(2);

	for (group = 1; group < groups_count; group++) {
		write_block(group_first_block(group), super_block);
		write_group_desc_table(group_first_block(group) + 1UL);
	}
}

static void write_group_bitmaps(void)
{
	unsigned short group;
	unsigned short gblocks;
	unsigned short meta;
	unsigned short ginodes;
	unsigned short i;

	meta = metadata_blocks();

	for (group = 0; group < groups_count; group++) {
		memset(bitmap_block, 0, sizeof(bitmap_block));
		gblocks = group_blocks(group);
		for (i = 0; i < meta && i < gblocks; i++)
			set_bitmap(bitmap_block, i);
		if (group == 0)
			set_bitmap(bitmap_block, meta);
		fill_bitmap_tail(bitmap_block, gblocks);
		write_block(group_desc[group].bg_block_bitmap, bitmap_block);

		memset(bitmap_block, 0, sizeof(bitmap_block));
		ginodes = group_inodes(group);
		if (group == 0) {
			for (i = 0; i < ginodes && i < EXT2_GOOD_OLD_FIRST_INO - 1; i++)
				set_bitmap(bitmap_block, i);
		}
		fill_bitmap_tail(bitmap_block, ginodes);
		write_block(group_desc[group].bg_inode_bitmap, bitmap_block);
	}
}

static void write_inode_tables(void)
{
	unsigned short group;
	unsigned short i;

	for (group = 0; group < groups_count; group++) {
		for (i = 0; i < inode_table_blocks; i++)
			write_block(group_desc[group].bg_inode_table + i, zero_block);
	}
}

static void write_root_inode(void)
{
	struct ext2_inode *inode;
	time_t now;
	unsigned long root_data_block;

	now = time(NULL);
	root_data_block = group_desc[0].bg_inode_table + inode_table_blocks;

	memset(bitmap_block, 0, sizeof(bitmap_block));
	inode = ((struct ext2_inode *)bitmap_block) + 1;
	inode->i_mode = EXT2_S_IFDIR | 0755;
	inode->i_size = BLOCK_SIZE;
	inode->i_atime = now;
	inode->i_ctime = now;
	inode->i_mtime = now;
	inode->i_links_count = 2;
	inode->i_blocks = BLOCK_SIZE >> 9;
	inode->i_block[0] = root_data_block;
	write_block(group_desc[0].bg_inode_table, bitmap_block);
}

static void write_root_dir(void)
{
	struct ext2_dir_entry *de;
	unsigned long root_data_block;

	root_data_block = group_desc[0].bg_inode_table + inode_table_blocks;

	memset(root_block, 0, sizeof(root_block));
	de = (struct ext2_dir_entry *)root_block;
	de->inode = EXT2_ROOT_INO;
	de->rec_len = EXT2_DIR_REC_LEN(1);
	de->name_len = 1;
	(void)memcpy(de->name, ".", 1);

	de = (struct ext2_dir_entry *)(root_block + EXT2_DIR_REC_LEN(1));
	de->inode = EXT2_ROOT_INO;
	de->rec_len = BLOCK_SIZE - EXT2_DIR_REC_LEN(1);
	de->name_len = 2;
	(void)memcpy(de->name, "..", 2);

	write_block(root_data_block, root_block);
}

int main(int argc, char **argv)
{
	parse_args(argc, argv);
	build_group_descs();
	write_super_and_backups();
	write_group_bitmaps();
	write_inode_tables();
	write_root_inode();
	write_root_dir();
	sync();
	close(dev_fd);
	return 0;
}
