//------------------------------------------------------------------------------
// EXT2 Boot Block payload (second disk sector)
//------------------------------------------------------------------------------

#include "linuxmt/config.h"
#include "linuxmt/boot.h"

// Global constants

#define LOADSEG                 DEF_INITSEG
#define OPTSEG                  DEF_OPTSEG

#define BLOCK_SIZE              1024
#define EXT2_ROOT_INO           2
#define EXT2_NDIR_BLOCKS        12
#define EXT2_IND_BLOCK          EXT2_NDIR_BLOCKS
#define EXT2_ADDR_PER_BLOCK     (BLOCK_SIZE / 4)
#define EXT2_GDESC_PER_BLOCK    (BLOCK_SIZE / 32)
#define EXT2_INODES_PER_BLOCK   (BLOCK_SIZE / 128)
#define SECT_PER_BLK 2

// Global variables

typedef unsigned char  byte_t;
typedef unsigned short word_t;
typedef unsigned long  dword_t;
typedef long           file_pos;

typedef struct {
	word_t lo;
	word_t hi;
} __attribute__((packed)) block_ptr_t;

struct ext2_super_block {
	dword_t s_inodes_count;
	dword_t s_blocks_count;
	dword_t s_r_blocks_count;
	dword_t s_free_blocks_count;
	dword_t s_free_inodes_count;
	dword_t s_first_data_block;
	dword_t s_log_block_size;
	dword_t s_log_frag_size;
	dword_t s_blocks_per_group;
	dword_t s_frags_per_group;
	dword_t s_inodes_per_group;
} __attribute__((packed));

/* On-disk group descriptors are 32 bytes; keep full stride for indexing. */
struct ext2_group_desc {
	block_ptr_t bg_block_bitmap;
	block_ptr_t bg_inode_bitmap;
	block_ptr_t bg_inode_table;
	byte_t bg_pad[20]; /* 32 - 3*4: remainder of ext2_group_desc */
} __attribute__((packed));

struct ext2_inode {
	word_t i_mode;
	word_t i_uid;
	file_pos i_size;
	dword_t i_atime;
	dword_t i_ctime;
	dword_t i_mtime;
	dword_t i_dtime;
	word_t i_gid;
	word_t i_links_count;
	dword_t i_blocks;
	dword_t i_flags;
	dword_t i_osd1;
	block_ptr_t i_block[15];
} __attribute__((packed));

/* On-disk directory entry: inode, rec_len, 16-bit name_len (e2fsprogs), then name */
struct ext2_dir_entry {
	dword_t inode;
	word_t rec_len;
	word_t name_len;
	char name[1];
} __attribute__((packed));

/* Some BIOS EDD implementations are picky about low-memory buffer alignment. */
static byte_t io_pad[0x14] __attribute__((used));
static byte_t i_block[BLOCK_SIZE];
static struct ext2_inode *i_data;
static byte_t z_block[1][BLOCK_SIZE];

static word_t i_now;
static word_t i_ipg;
static word_t i_linux;
static word_t blocks_left;
static unsigned long loadaddr;

//------------------------------------------------------------------------------

// Helpers from boot sector

void except(char code);
void run_prog(void);
void mark_bootopts_nonfat(void);
int seg_data(void);
void disk_read_blk(unsigned block, const int count, const byte_t *buf,
		   const int seg);

//------------------------------------------------------------------------------

#define seg_data()      __builtin_ia16_near_data_segment()
#define _MK_FP(seg,off) ((void __far *)((((unsigned long)(seg)) << 16) | (off)))

static void load_super(void);
static void load_inode(void);
static void load_extent(register block_ptr_t *z, register block_ptr_t *zend);
static void load_file(void);

//------------------------------------------------------------------------------
// This must occur right at the start of the payload.  Kept first in this file
// and built with -fno-toplevel-reorder (see Makefile), like boot_minix.c.

void load_prog(void)
{
	load_super();

	i_linux = 0;
	i_now = EXT2_ROOT_INO;
	loadaddr = 0;
	load_file();

	if (i_linux) {
		i_now = i_linux;
		loadaddr = LOADSEG << 4;
		load_file();
		/* Tell setup.S to skip FAT /bootopts (wrong BPB on ext2). */
		mark_bootopts_nonfat();
		run_prog();
	}
}

static void load_super(void)
{
	register struct ext2_super_block *sb;

	disk_read_blk(1, 1, i_block, seg_data());
	sb = (struct ext2_super_block *)i_block;
	i_ipg = (word_t)sb->s_inodes_per_group;
}

static void load_inode(void)
{
	register struct ext2_group_desc *gdp;
	word_t ino;
	word_t group;
	word_t index;
	word_t block;

	ino = i_now - 1;
	group = ino / i_ipg;
	index = ino % i_ipg;

	block = 2 + (group / EXT2_GDESC_PER_BLOCK);
	disk_read_blk(block, SECT_PER_BLK, i_block, seg_data());

	gdp = (struct ext2_group_desc *)i_block;
	gdp += group % EXT2_GDESC_PER_BLOCK;

	block = gdp->bg_inode_table.lo + (index / EXT2_INODES_PER_BLOCK);
	disk_read_blk(block, SECT_PER_BLK, i_block, seg_data());

	/* Inodes are 128 bytes on disk; do not use sizeof(struct ext2_inode). */
	i_data = (struct ext2_inode *)((byte_t *)i_block +
		(index % EXT2_INODES_PER_BLOCK) * 128);
}

static void load_extent(register block_ptr_t *z, register block_ptr_t *zend)
{
	byte_t *p;
	word_t reclen;

	for (; z < zend && blocks_left; z++) {
		if (z->lo) {
			if (loadaddr) {
				disk_read_blk(z->lo, SECT_PER_BLK,
					      (byte_t *)(unsigned)loadaddr,
					      ((unsigned)(loadaddr >> 4)) & 0xf000);
				loadaddr += BLOCK_SIZE;
			} else {
				disk_read_blk(z->lo, SECT_PER_BLK, z_block[0], seg_data());
				p = z_block[0];
				while (p < z_block[0] + BLOCK_SIZE) {
					reclen = *(word_t *)(p + 4);
					if (!reclen)
						break;
					if (*(dword_t *)p) {
						if (*(word_t *)(p + 6) == 5 &&
						    *(dword_t *)(p + 8) == 0x756e696c &&
						    p[12] == 'x')
								i_linux = (word_t)(*(dword_t *)p);
						}
					p += reclen;
				}
			}
		}
		blocks_left--;
	}
}

static void load_file(void)
{
	load_inode();
	blocks_left = (word_t)((i_data->i_size + BLOCK_SIZE - 1) >> 10);

	load_extent(&i_data->i_block[0], &i_data->i_block[EXT2_IND_BLOCK]);
	if (!blocks_left)
		return;

	if (i_data->i_block[EXT2_IND_BLOCK].lo) {
		if (loadaddr) {
			disk_read_blk(i_data->i_block[EXT2_IND_BLOCK].lo, SECT_PER_BLK,
				      z_block[0], seg_data());
			load_extent((block_ptr_t *)z_block[0],
				    (block_ptr_t *)(z_block[0] + BLOCK_SIZE));
		} else {
			disk_read_blk(i_data->i_block[EXT2_IND_BLOCK].lo, SECT_PER_BLK,
				      i_block, seg_data());
			load_extent((block_ptr_t *)i_block,
				    (block_ptr_t *)(i_block + BLOCK_SIZE));
		}
	}
}
