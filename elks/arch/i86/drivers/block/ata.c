/**********************************************************************
 * ELKS Generic ATA/IDE functions
 *
 *  Supports dynamic I/O port setup of ATA, XTIDE, XTCF and SOLO/86 controllers:
 *
 *             ATA             XTIDE v1        XTCF                SOLO/86
 * BASE        0x1F0           0x300           0x300               0x40
 * BASE+reg    0x1F0+reg       0x300+reg       0x300+(reg<<1)      0x40+(reg<<1)
 * I/O         16-bit          8-bit           8-bit               16-bit
 *
 *                             XTIDE v2 (high speed mode)
 * BASE+reg                    0x300+swapA0A3(reg)
 *
 * CTRL        BASE+0x200+reg  BASE+0x08+reg   BASE+0x10+(reg<<1)  BASE+0x10+(reg<<1)
 * CTRL+6      BASE+0x206      BASE+0x0E       BASE+0x1C           BASE+0x1C
 * DEVCTL      0x3F6           0x30E           0x31C               0x5C
 *
 *                             XTIDE v2 (high speed mode)
 * CTRL                        BASE+swapA0A3(0x08+reg)
 * CTRL+6                      BASE+swapA0A3(0x0E)
 * DEVCTL                      0x307
 *
 * This code assumes only supports one ATA controller, and sets XTCF operation
 * on 8088/8086 CPUs, unless overriden using xtide= in /bootopts.
 *
 * This code uses LBA addressing for the disks. Any disks without
 * LBA support (they'd need to be pretty old) are simply ignored.
 *
 * All read/write methods follow this same process:
 * - wait till drive is not busy
 * - send drive/head information
 * - wait till drive is not busy
 * - send count and sector information
 * - wait till drive is not busy
 * - check ERR, DFE and DRQ bits
 * - read/write data
 * - wait till drive is not busy
 *
 * Caveat emptor: This code is based on the ATA specifications,
 * XTIDE Universal BIOS source, and some common sense.
 *
 * Ferry Hendrikx, June 2025
 * Greg Haerr, July 2025 Added XTCF and XTIDE support
 **********************************************************************/

#include <linuxmt/config.h>
#include <linuxmt/kernel.h>
#include <linuxmt/sched.h>
#include <linuxmt/genhd.h>
#include <linuxmt/ata.h>
#include <linuxmt/errno.h>
#include <linuxmt/heap.h>
#include <linuxmt/memory.h>
#include <linuxmt/debug.h>
#include <linuxmt/prectimer.h>
#include <arch/io.h>
#include <arch/irq.h>
#include <arch/segment.h>

/* hardware controller access modes, override using xtide= in /bootopts */
#define MODE_ATA        0       /* standard - ATA at ports 0x1F0/0x3F6 */
#define MODE_XTIDEv1    1       /* XTIDE rev1 - ATA at ports 0x300/0x30E, 8-bit I/O */
#define MODE_XTIDEv2    2       /* XTIDE rev2 - XTIDE v1 with registers a0/a3 swapped */
#define MODE_XTCF       3       /* XTCF - XTIDE v1 w/regs << 1 at ports 0x300/0x31C */
#define MODE_SOLO86     4       /* XTCF at ports 0x40/0x5C, 16-bit I/O */
#define MODE_MAX        4
#define AUTO            (-1)    /* use MODE_ATA for PC/AT (286+), MODE_XTCF on PC/XT */

/* hardware I/O port data xfer modes */
#define XFER_16         0       /* ATA/XTIDEv2 16-bit I/O */
#define XFER_8_XTCF     1       /* XTCF set 8-bit feature cmd, then xfer lo then hi */
#define XFER_8_XTIDEv1  2       /* XTIDEv1 8-bit xfer hi at data+8 then lo at data+0 */

#define ATA_DEV_NONE    0
#define ATA_DEV_DISK    1
#define ATA_DEV_ATAPI   2

#define ATA_RW_RETRIES  3
#define ATA_FAST_XFER_SECTORS  8
#define ATA_SLOW_XFER_SECTORS  4

/* configurable options */
#define FASTIO          1       /* =1 to use ASM in/out instructions for I/O */
#define EMUL_XTCF       0       /* =1 to force XTCF xfer mode for use with PCem */

#ifndef CONFIG_ATA_BASE_PORT
#define CONFIG_ATA_BASE_PORT       0x1F0
#endif
#ifndef CONFIG_ATA_XTIDE_BASE_PORT
#define CONFIG_ATA_XTIDE_BASE_PORT 0x300
#endif
#ifndef CONFIG_ATA_SOLO86_BASE_PORT
#define CONFIG_ATA_SOLO86_BASE_PORT 0x40
#endif

static int xfer_mode = AUTO;    /* change this to force a particular I/O xfer method */

/* default base port (change if required) ATA, XTIDEv1, XTIDEv2,  XTCF, SOLO86 */
static unsigned int def_base_ports[5] = {
    CONFIG_ATA_BASE_PORT,
    CONFIG_ATA_XTIDE_BASE_PORT,
    CONFIG_ATA_XTIDE_BASE_PORT,
    CONFIG_ATA_XTIDE_BASE_PORT,
    CONFIG_ATA_SOLO86_BASE_PORT
};

/* control register offsets from base ports (should not need changing) */
static unsigned int ctrl_offsets[5] =   { 0x200,  0x08,    0x08,  0x10, 0x10 };

/* table to translate from ATA -> XTIDEV2 register file */
static unsigned int xlate_XTIDEv2[8] = {
    XTIDEV2_DATA,
    XTIDEV2_ERR,
    XTIDEV2_CNT,
    XTIDEV2_LBA_LO,
    XTIDEV2_LBA_MD,
    XTIDEV2_LBA_HI,
    XTIDEV2_SELECT,
    XTIDEV2_STATUS
};

/* wait loop 10ms ticks while busy waiting */
#define WAIT_50MS   (5*HZ/100)      /* 5/100 sec = 50ms while busy */
#define WAIT_1SEC   (1*HZ)          /* 1 sec wait for identify */
#define WAIT_10SEC  (10*HZ)         /* 10 sec wait for read/write */
#define WAIT_20SEC  (20*HZ)         /* old hardware timeout */

/* end of configurable options */

static unsigned int ata_base_port;
static unsigned int ata_ctrl_port;
static unsigned char ata_devtype[2];
static unsigned char ata_use_lba[2];
static unsigned int ata_heads[2];
static unsigned int ata_sectors[2];
static sector_t ata_total_sectors[2];
static unsigned char ata_last_status;
static unsigned char ata_last_error;
#ifdef CONFIG_ATA_SLOW
int ata_slow_profile = 1;
#else
int ata_slow_profile;
#endif
#ifdef CONFIG_ATA_ATAPI
static unsigned char *atapi_buffer;
static unsigned char atapi_packet[12];
static unsigned char atapi_cache_valid;
static unsigned char atapi_cache_drive;
static sector_t atapi_cache_lba;
#endif

/**********************************************************************
 * ATA support functions
 **********************************************************************/

/* convert register number to command block port address */
static int ATPROC BASE(int reg)
{
    if (ata_mode >= MODE_XTCF)          /* XTCF uses SHL 1 register file */
        reg <<= 1;
    else if (ata_mode == MODE_XTIDEv2)  /* XTIDEv2 uses a3/a0 swapped register file */
        reg = xlate_XTIDEv2[reg];
    return ata_base_port + reg;
}

/* input byte from translated register number */
static unsigned char ATPROC INB(int reg)
{
    return inb(BASE(reg));
}

/* output byte to port from translated register number */
/* FIXME: compiler bug if 'unsigned char byte' declared below, bad code in ata_cmd */
static void ATPROC OUTB(unsigned int byte, int reg)
{
    outb(byte, BASE(reg));
}

static sector_t ATPROC ata_get_lba28_total(unsigned short *buffer)
{
    return ((sector_t)buffer[ATA_INFO_SECT_HI] << 16) | buffer[ATA_INFO_SECT_LO];
}

static void ATPROC ata_set_synth_geometry(struct drive_infot *drivep, sector_t total)
{
    sector_t cylinders;
    int heads = 16;
    int sectors = 63;

    if (total >= (sector_t)1024 * 16 * 63)
        heads = 255;

    cylinders = total / ((sector_t)heads * sectors);
    if (cylinders == 0)
        cylinders = 1;
    if (cylinders > 65535UL)
        cylinders = 65535UL;

    drivep->cylinders = (unsigned int)cylinders;
    drivep->heads = heads;
    drivep->sectors = sectors;
}

static int ATPROC ata_valid_chs(unsigned short *buffer)
{
    return buffer[ATA_INFO_CYLS] != 0 && buffer[ATA_INFO_CYLS] <= 0x7F00 &&
        buffer[ATA_INFO_HEADS] != 0 && buffer[ATA_INFO_HEADS] <= 16 &&
        buffer[ATA_INFO_SPT] != 0 && buffer[ATA_INFO_SPT] <= 63;
}

static sector_t ATPROC ata_get_chs_total(unsigned short *buffer)
{
    return (sector_t)buffer[ATA_INFO_CYLS] *
        buffer[ATA_INFO_HEADS] * buffer[ATA_INFO_SPT];
}

static void ATPROC ata_set_chs_geometry(struct drive_infot *drivep,
    unsigned short *buffer)
{
    drivep->cylinders = buffer[ATA_INFO_CYLS];
    drivep->heads = buffer[ATA_INFO_HEADS];
    drivep->sectors = buffer[ATA_INFO_SPT];
}

static void ATPROC ata_save_error(unsigned char status)
{
    ata_last_status = status;
    ata_last_error = (status & ATA_STATUS_ERR) ? INB(ATA_REG_ERR) : 0;
}

static void ATPROC ata_print_error(char dev, const char *op, int error,
    sector_t sector)
{
    printk("cf%c: %s error %d status=%x err=%x lba=%lu\n",
        dev, op, error, ata_last_status, ata_last_error, sector);
}

static unsigned int ATPROC ata_select_ticks(void)
{
    return ata_slow_profile ? WAIT_1SEC : WAIT_50MS;
}

static unsigned int ATPROC ata_identify_ticks(void)
{
    return ata_slow_profile ? WAIT_10SEC : WAIT_1SEC;
}

static unsigned int ATPROC ata_rw_ticks(void)
{
    return ata_slow_profile ? WAIT_20SEC : WAIT_10SEC;
}

static unsigned int ATPROC ata_first_write_ticks(void)
{
    return ata_slow_profile ? WAIT_1SEC : WAIT_50MS;
}

static unsigned int ATPROC ata_xfer_count(unsigned int count)
{
    unsigned int max = ata_slow_profile ?
        ATA_SLOW_XFER_SECTORS : ATA_FAST_XFER_SECTORS;

    if (count > max)
        count = max;
    if (count > 255)
        count = 255;
    return count;
}

/* delay 10ms */
static void ATPROC delay_10ms(void)
{
    jiff_t timeout = jiffies() + 1 + 1; /* guarantee at least 10ms interval */

    while (!time_after(jiffies(), timeout))
        continue;
}

/**
 * ATA wait 10ms ticks until not busy
 */
static int ATPROC ata_wait(unsigned int ticks)
{
    jiff_t timeout = jiffies() + ticks + 1;
    unsigned char status;

    do
    {
        status = INB(ATA_REG_STATUS);

        // are we done?

        if ((status & ATA_STATUS_BSY) == 0) {
            ata_last_status = status;
            return 0;
        }

    } while (!time_after(jiffies(), timeout));

    ata_save_error(status);
    return -ENXIO;
}


#ifdef __ia16__
static void ATPROC xtidev1_insw(int port, unsigned int seg,
    unsigned int offset, unsigned int count)
{
    unsigned int ax, cx, di;

    asm volatile (
        "push %%es\n"
        "mov %%ax,%%es\n"
        "cld\n"
        "1:\n"
        "in (%%dx),%%al\n"
        "stosb\n"
        "add $8,%%dx\n"
        "in (%%dx),%%al\n"
        "stosb\n"
        "sub $8,%%dx\n"
        "loop 1b\n"
        "pop %%es\n"
        : "=a" (ax), "=c" (cx), "=D" (di)
        : "d" (port), "a" (seg), "D" (offset), "c" (count)
        : "memory" );
}

static void ATPROC xtidev1_outsw(int port, unsigned int seg,
    unsigned int offset, unsigned int count)
{
    unsigned int ax, cx, si;

    asm volatile (
        "push %%ds\n"
        "mov %%ax,%%ds\n"
        "cld\n"
        "1:\n"
        "lodsb\n"
        "movb %%al,%%ah\n"
        "add $8,%%dx\n"
        "lodsb\n"
        "out %%al,(%%dx)\n"
        "sub $8,%%dx\n"
        "movb %%ah,%%al\n"
        "out %%al,(%%dx)\n"
        "loop 1b\n"
        "pop %%ds\n"
        : "=a" (ax), "=c" (cx), "=S" (si)
        : "d" (port), "a" (seg), "S" (offset), "c" (count)
        : "memory" );
}
#endif

/* read from I/O port into far buffer */
static void ATPROC read_ioport(int port, unsigned char __far *buffer, size_t count)
{
#if !FASTIO || !defined(__ia16__)
    size_t i;
#endif

    switch (xfer_mode) {
    case XFER_16:
#if FASTIO
        insw(port, _FP_SEG(buffer), _FP_OFF(buffer), count/2);
#else
        for (i = 0; i < count; i+=2)
        {
            unsigned short word = inw(port);

            *buffer++ = word;
            *buffer++ = word >> 8;
        }
#endif
        break;

    case XFER_8_XTCF:
#if FASTIO
        insb(port, _FP_SEG(buffer), _FP_OFF(buffer), count);
#else
        for (i = 0; i < count; i++)
            *buffer++ = inb(port);
#endif
        break;

    case XFER_8_XTIDEv1:
#ifdef __ia16__
        xtidev1_insw(port, _FP_SEG(buffer), _FP_OFF(buffer), count/2);
#else
        for (i = 0; i < count; i+=2)
        {
            *buffer++=  inb(port);      // lo byte first when reading
            *buffer++ = inb(port+8);    // then hi byte from port+8
        }
#endif
        break;
    }
}

/* write from far buffer to I/O port */
static void ATPROC write_ioport(int port, unsigned char __far *buffer, size_t count)
{
#if !FASTIO || !defined(__ia16__)
    size_t i;
    unsigned short word;
#endif

    switch (xfer_mode) {
    case XFER_16:
#if FASTIO
        outsw(port, _FP_SEG(buffer), _FP_OFF(buffer), count/2);
#else
        for (i = 0; i < count; i+=2)
        {
            word = *buffer++;
            word |= *buffer++ << 8;
            outw(word, port);
        }
#endif
        break;

    case XFER_8_XTCF:
#if FASTIO
        outsb(port, _FP_SEG(buffer), _FP_OFF(buffer), count);
#else
        for (i = 0; i < count; i++)
            outb(*buffer++, port);
#endif
        break;

    case XFER_8_XTIDEv1:
#ifdef __ia16__
        xtidev1_outsw(port, _FP_SEG(buffer), _FP_OFF(buffer), count/2);
#else
        for (i = 0; i < count; i+=2)
        {
            word = *buffer++;           // save low byte
            outb(*buffer++, port+8);    // hi byte first to port+8
            outb(word, port);           // then lo byte to port+0
        }
#endif
        break;
    }
}

/**********************************************************************
 * ATA functions
 **********************************************************************/

/**
 * ATA try setting 8-bit transfer mode, may be unimplemented by controller
 */
#pragma GCC diagnostic ignored "-Wunused-function"
static int ATPROC ata_set8bitmode(void)
{
    int error;
    unsigned char status;

    // set 8-bit transfer mode

    OUTB(0x01, ATA_REG_FEAT);
    OUTB(ATA_CMD_FEAT, ATA_REG_CMD);


    // wait for drive to be not-busy

    error = ata_wait(ata_select_ticks());
    if (error)
        return error;


    // check for error

    status = INB(ATA_REG_STATUS);

    if (status & ATA_STATUS_ERR)
    {
        printk("cfa: can't set 8-bit xfer\n");
        return -EINVAL;
    }

    printk("cfa: 8-bit xfer on\n");
    return 0;
}

/**
 * ATA select drive
 */
static int ATPROC ata_select(unsigned int drive, int lba, sector_t sector,
    unsigned int head)
{
    unsigned char select = 0xA0 | (drive << 4);
    int error;

    if (lba)
        select |= 0x40 | ((sector >> 24) & 0x0F);
    else
        select |= head & 0x0F;

    // wait for current drive to be non-busy

    error = ata_wait(ata_select_ticks());
    if (error)
        return error;

    // select drive, 400ns wait not required since BSY clear above

    OUTB(select, ATA_REG_SELECT);

    return 0;
}

/**
 * send an ATA command to the drive
 */
static int ATPROC ata_cmd(unsigned int drive, unsigned int cmd, sector_t sector,
    unsigned int count)
{
    int error, wait;
    unsigned char status;
    unsigned int cyl, head, sect;
    sector_t tmp;

    if (cmd == ATA_CMD_READ || cmd == ATA_CMD_WRITE) {
        if (ata_use_lba[drive]) {
            if (sector >= ATA_LBA28_SECTORS)
                return -EINVAL;
            error = ata_select(drive, 1, sector, 0);
            if (error)
                return error;

            OUTB(0x00, ATA_REG_FEAT);
            OUTB(count, ATA_REG_CNT);
            OUTB((unsigned char) (sector),       ATA_REG_LBA_LO);
            OUTB((unsigned char) (sector >> 8),  ATA_REG_LBA_MD);
            OUTB((unsigned char) (sector >> 16), ATA_REG_LBA_HI); // FIXME OUTB compiler bug here
        } else {
            if (!ata_heads[drive] || !ata_sectors[drive] ||
                    sector >= ata_total_sectors[drive])
                return -EINVAL;

            tmp = sector / ata_sectors[drive];
            sect = (unsigned int)(sector % ata_sectors[drive]) + 1;
            head = (unsigned int)(tmp % ata_heads[drive]);
            cyl = (unsigned int)(tmp / ata_heads[drive]);
            if (cyl > 0xFFFF)
                return -EINVAL;

            error = ata_select(drive, 0, 0, head);
            if (error)
                return error;

            OUTB(0x00, ATA_REG_FEAT);
            OUTB(count, ATA_REG_CNT);
            OUTB((unsigned char)sect,        ATA_REG_LBA_LO);
            OUTB((unsigned char)cyl,         ATA_REG_LBA_MD);
            OUTB((unsigned char)(cyl >> 8),  ATA_REG_LBA_HI);
        }
    } else {
        error = ata_select(drive, 0, 0, 0);
        if (error)
            return error;

        OUTB(0x00, ATA_REG_FEAT);
        OUTB(count, ATA_REG_CNT);
        OUTB(0x00, ATA_REG_LBA_LO);
        OUTB(0x00, ATA_REG_LBA_MD);
        OUTB(0x00, ATA_REG_LBA_HI);
    }

    // send command
    OUTB(cmd, ATA_REG_CMD);


    // wait for drive to be not-busy


    switch (cmd) {
    case ATA_CMD_READ:  wait = ata_rw_ticks();          break;
    case ATA_CMD_WRITE: wait = ata_first_write_ticks(); break;
    case ATA_CMD_ID:    wait = ata_identify_ticks();    break;
    default:            wait = WAIT_50MS;               break;
    }
    error = ata_wait(wait);
    if (error)
        return error;


    // check for error

    status = INB(ATA_REG_STATUS);

    if (status & (ATA_STATUS_ERR|ATA_STATUS_DFE)) {
        ata_save_error(status);
        return -EIO;
    }

    if (! (status & ATA_STATUS_DRQ)) {
        ata_save_error(status);
        return -EINVAL;
    }
    return 0;
}

static int ATPROC ata_wait_drq(unsigned int ticks)
{
    int error;
    unsigned char status;

    error = ata_wait(ticks);
    if (error)
        return error;

    status = INB(ATA_REG_STATUS);
    if (status & (ATA_STATUS_ERR|ATA_STATUS_DFE)) {
        ata_save_error(status);
        return -EIO;
    }
    if (!(status & ATA_STATUS_DRQ)) {
        ata_save_error(status);
        return -EINVAL;
    }
    return 0;
}

static int ATPROC ata_nondata_cmd(unsigned int drive, unsigned int cmd,
    unsigned int ticks)
{
    int error;
    unsigned char status;

    error = ata_select(drive, 0, 0, 0);
    if (error)
        return error;

    OUTB(0x00, ATA_REG_FEAT);
    OUTB(0x00, ATA_REG_CNT);
    OUTB(0x00, ATA_REG_LBA_LO);
    OUTB(0x00, ATA_REG_LBA_MD);
    OUTB(0x00, ATA_REG_LBA_HI);
    OUTB(cmd, ATA_REG_CMD);

    error = ata_wait(ticks);
    if (error)
        return error;

    status = INB(ATA_REG_STATUS);
    if (status & (ATA_STATUS_ERR|ATA_STATUS_DFE)) {
        ata_save_error(status);
        return -EIO;
    }
    return 0;
}


/**
 * identify an ATA device
 *
 * drive  : physical drive number (0 or 1)
 * *buffer: pointer to buffer containing 512 bytes of space
 */
static int ATPROC ata_identify_cmd(unsigned int drive, unsigned int cmd,
    unsigned char __far *buf)
{
    int error;


    // send command

    error = ata_cmd(drive, cmd, 0, 0);
    if (error)
        return error;

    // read data

    read_ioport(BASE(ATA_REG_DATA), buf, ATA_SECTOR_SIZE);

    return 0;
}

#ifdef CONFIG_ATA_ATAPI
static void ATPROC atapi_clear_packet(void)
{
    int i;

    for (i = 0; i < 12; i++)
        atapi_packet[i] = 0;
}

static sector_t ATPROC atapi_get_be32(unsigned char *p)
{
    return ((sector_t)p[0] << 24) | ((sector_t)p[1] << 16) |
        ((sector_t)p[2] << 8) | p[3];
}

static int ATPROC atapi_packet_cmd(unsigned int drive, unsigned int bytes,
    unsigned int ticks)
{
    int error;
    unsigned int got;
    unsigned char status;

    error = ata_select(drive, 0, 0, 0);
    if (error)
        return error;

    OUTB(0x00, ATA_REG_FEAT);
    OUTB(0x00, ATA_REG_CNT);
    OUTB(0x00, ATA_REG_LBA_LO);
    OUTB((unsigned char)bytes, ATA_REG_LBA_MD);
    OUTB((unsigned char)(bytes >> 8), ATA_REG_LBA_HI);
    OUTB(ATA_CMD_PACKET, ATA_REG_CMD);

    error = ata_wait(ata_identify_ticks());
    if (error)
        return error;

    status = INB(ATA_REG_STATUS);
    if ((status & (ATA_STATUS_ERR|ATA_STATUS_DFE)) || !(status & ATA_STATUS_DRQ)) {
        ata_save_error(status);
        return -EIO;
    }

    write_ioport(BASE(ATA_REG_DATA), (unsigned char __far *)atapi_packet, 12);

    error = ata_wait(ticks);
    if (error)
        return error;

    status = INB(ATA_REG_STATUS);
    if (status & (ATA_STATUS_ERR|ATA_STATUS_DFE)) {
        ata_save_error(status);
        return -EIO;
    }
    if (!(status & ATA_STATUS_DRQ)) {
        ata_save_error(status);
        return -EINVAL;
    }

    got = (unsigned int)INB(ATA_REG_LBA_MD) |
        ((unsigned int)INB(ATA_REG_LBA_HI) << 8);
    if (got == 0 || got > bytes)
        got = bytes;
    read_ioport(BASE(ATA_REG_DATA), (unsigned char __far *)atapi_buffer, got);
    (void)ata_wait(ata_select_ticks());
    return 0;
}

static int ATPROC atapi_read_capacity(unsigned int drive, sector_t *sectorsp)
{
    int error;
    sector_t last_lba;
    sector_t block_len;

    atapi_clear_packet();
    atapi_packet[0] = 0x25;         /* READ CAPACITY(10) */
    error = atapi_packet_cmd(drive, 8, WAIT_10SEC);
    if (error)
        return error;

    last_lba = atapi_get_be32(atapi_buffer);
    block_len = atapi_get_be32(atapi_buffer + 4);
    if (block_len != ATAPI_SECTOR_SIZE) {
        printk("cf%c: ATAPI sector size %lu unsupported\n",
            drive + 'a', block_len);
        return -EINVAL;
    }

    *sectorsp = (last_lba + 1) << 2;    /* expose as 512-byte sectors */
    return 0;
}

static int ATPROC atapi_read_512(unsigned int drive, sector_t sector,
    char *buf, ramdesc_t seg)
{
    int error;
    sector_t cd_lba = sector >> 2;
    unsigned int offset = (unsigned int)(sector & 3) << 9;

    if (!atapi_cache_valid || atapi_cache_drive != drive ||
            atapi_cache_lba != cd_lba) {
        atapi_clear_packet();
        atapi_packet[0] = 0x28;     /* READ(10) */
        atapi_packet[2] = (unsigned char)(cd_lba >> 24);
        atapi_packet[3] = (unsigned char)(cd_lba >> 16);
        atapi_packet[4] = (unsigned char)(cd_lba >> 8);
        atapi_packet[5] = (unsigned char)cd_lba;
        atapi_packet[8] = 1;        /* one 2048-byte block */

        error = atapi_packet_cmd(drive, ATAPI_SECTOR_SIZE, WAIT_10SEC);
        if (error)
            return error;

        atapi_cache_valid = 1;
        atapi_cache_drive = (unsigned char)drive;
        atapi_cache_lba = cd_lba;
    }

    xms_fmemcpyb(buf, seg, atapi_buffer + offset, kernel_ds, ATA_SECTOR_SIZE);
    return 0;
}
#endif


/**********************************************************************
 * ATA exported functions
 **********************************************************************/

/**
 * reset the ATA interface
 */
int ATPROC ata_reset(void)
{
    unsigned char byte;

#ifdef CONFIG_ARCH_SOLO86
    ata_mode = MODE_SOLO86;
#endif
#ifdef CONFIG_ARCH_NECV25
    ata_mode = MODE_XTIDEv2;
    xfer_mode = XFER_8_XTCF;
#endif
#if EMUL_XTCF
    xfer_mode = XFER_8_XTCF;
#endif

    // dynamically set controller access method and I/O port addresses

    if (ata_mode == AUTO || ata_mode > MODE_MAX)
    {
        if (arch_cpu < CPU_80286)       // XTCF is default for 8088/8086 systems
            ata_mode = MODE_XTCF;
        else
            ata_mode = MODE_ATA;        // otherwise standard ATA
    }

    if (xfer_mode == AUTO)
    {
        switch (ata_mode) {
        case MODE_ATA:
        case MODE_SOLO86:
        case MODE_XTIDEv2:
            xfer_mode = XFER_16;
            break;
        case MODE_XTIDEv1:
            xfer_mode = XFER_8_XTIDEv1;
            break;
        case MODE_XTCF:
            xfer_mode = XFER_8_XTCF;
            break;
        }
    }

    // set base port I/O address from emulation mode

    ata_base_port = def_base_ports[ata_mode];

    // set device control register 6 I/O address

    ata_ctrl_port = BASE(6) + ctrl_offsets[ata_mode];
    if (ata_mode == MODE_XTIDEv2)
        ata_ctrl_port ^= 0b1001;        // tricky swap A0 and A3 works only for reg 6

    // controller reset

    byte = INB(ATA_REG_SELECT);
    OUTB(0xA0, ATA_REG_SELECT);
    delay_10ms();
    if (INB(ATA_REG_SELECT) != 0xA0)    // probe fail doesn't stop driver reset for now
    {
        printk("cf:  ATA at %x/%x xtide=%d,%d probe fail (%x)\n",
            ata_base_port, ata_ctrl_port, ata_mode, xfer_mode, byte);
        OUTB(byte, ATA_REG_SELECT);
        //return -ENODEV;
    }

    // set nIEN and SRST bits

    outb(0x06, ata_ctrl_port);
    delay_10ms();

    // set nIEN bit (and clear SRST bit)

    outb(0x02, ata_ctrl_port);
    delay_10ms();

    // try and turn on 8-bit mode, fallback to 16-bit if controller can't handle it

#if !EMUL_XTCF
    if (xfer_mode == XFER_8_XTCF)
    {
        if (ata_set8bitmode() < 0)
            xfer_mode = XFER_16;
    }
#endif
    return 0;
}


/**
 * initialise an ATA device, return zero if not found
 */
int ATPROC ata_init(int drive, struct drive_infot *drivep)
{
    unsigned short *buffer;
    sector_t total;
    int error;

    drivep->cylinders = 0;      // invalidate device
    drivep->total_sectors = 0;
    drivep->read_only = 0;
    drivep->removable = 0;
    ata_devtype[drive] = ATA_DEV_NONE;
    ata_use_lba[drive] = 0;
    ata_heads[drive] = 0;
    ata_sectors[drive] = 0;
    ata_total_sectors[drive] = 0;

    buffer = (unsigned short *) heap_alloc(ATA_SECTOR_SIZE, HEAP_TAG_DRVR|HEAP_TAG_CLEAR);
    if (!buffer)
        return 0;

    // identify drive

    error = ata_identify_cmd(drive, ATA_CMD_ID, (unsigned char __far *)buffer);
    if (error == 0)
    {
        int lba;

        total = ata_get_lba28_total(buffer);
        lba = (total != 0 && (buffer[ATA_INFO_CAPS] & ATA_CAPS_LBA));
        if (lba) {
            if (total > ATA_LBA28_SECTORS) {
                printk("cf%c: LBA28 limit, clipping %lu sectors\n",
                    drive+'a', total);
                total = ATA_LBA28_SECTORS;
            }
            ata_set_synth_geometry(drivep, total);
            ata_use_lba[drive] = 1;
        } else if (ata_valid_chs(buffer)) {
            total = ata_get_chs_total(buffer);
            ata_set_chs_geometry(drivep, buffer);
            ata_use_lba[drive] =
                (ata_mode == MODE_XTIDEv1 || ata_mode == MODE_XTIDEv2);
        } else {
            printk("cf%c: ATA identify unsupported format VER %x/%d xtide=%d,%d\n",
                drive+'a', buffer[ATA_INFO_VER_MAJ], buffer[ATA_INFO_SECT_SZ],
                ata_mode, xfer_mode);
            goto out;
        }

        drivep->total_sectors = total;
        drivep->sector_size = ATA_SECTOR_SIZE;
        drivep->fdtype = HARDDISK;
        ata_devtype[drive] = ATA_DEV_DISK;
        ata_heads[drive] = drivep->heads;
        ata_sectors[drive] = drivep->sectors;
        ata_total_sectors[drive] = total;
        show_drive_info(drivep, "cf", drive, 1, " ");

        // now display extra info: ATA sector total, version and sector size

        printk("%luK %s VER %x/%d xtide=%d,%d\n", total >> 1,
            lba ? "LBA28" : (ata_use_lba[drive] ? "CHS/LBA" : "CHS"),
            buffer[ATA_INFO_VER_MAJ], buffer[ATA_INFO_SECT_SZ],
            ata_mode, xfer_mode);
    }
#ifdef CONFIG_ATA_ATAPI
    else if (ata_identify_cmd(drive, ATA_CMD_PKT_ID,
                (unsigned char __far *)buffer) == 0)
    {
        if (!atapi_buffer) {
            atapi_buffer = heap_alloc(ATAPI_SECTOR_SIZE,
                HEAP_TAG_DRVR|HEAP_TAG_CLEAR);
            if (!atapi_buffer)
                goto out;
        }

        if (atapi_read_capacity(drive, &total) == 0) {
            ata_set_synth_geometry(drivep, total);
            drivep->total_sectors = total;
            drivep->sector_size = ATA_SECTOR_SIZE;
            drivep->fdtype = HARDDISK;
            drivep->read_only = 1;
            drivep->removable = 1;
            ata_devtype[drive] = ATA_DEV_ATAPI;
            atapi_cache_valid = 0;
            show_drive_info(drivep, "cf", drive, 1, " ");
            printk("%luK ATAPI CD-ROM xtide=%d,%d\n", total >> 1,
                ata_mode, xfer_mode);
        } else {
            printk("cf%c: ATAPI device not ready\n", drive+'a');
        }
    }
#endif
    else
    {
        printk("cf%c: ATA at %x/%x xtide=%d,%d not found (%d)\n",
            drive+'a', ata_base_port, ata_ctrl_port, ata_mode, xfer_mode,
            error);
    }

out:
    heap_free(buffer);

    return drivep->cylinders;
}


/**
 * read from an ATA device
 *
 * drive : physical drive number (0 or 1)
 * sector: sector number
 * buf/seg: I/O buffer address
 */
int ATPROC ata_read(unsigned int drive, sector_t sector, char *buf, ramdesc_t seg,
    unsigned int count)
{
    unsigned char __far *buffer;
    int error;
    int retry;
    unsigned int pass, done;
#ifdef CONFIG_FS_XMS
#pragma GCC diagnostic ignored "-Wshift-count-overflow"
    int use_xms = seg >> 16;
#endif

#ifdef CONFIG_ATA_ATAPI
    if (ata_devtype[drive] == ATA_DEV_ATAPI) {
        while (count--) {
            for (retry = 0; retry < ATA_RW_RETRIES; retry++) {
                error = atapi_read_512(drive, sector, buf, seg);
                if (!error)
                    break;
            }
            if (error) {
                ata_print_error((char)(drive+'a'), "atapi read", error, sector);
                return error;
            }
            sector++;
            buf += ATA_SECTOR_SIZE;
        }
        return 0;
    }
#endif

    while (count) {
        pass = ata_xfer_count(count);

        for (retry = 0; retry < ATA_RW_RETRIES; retry++) {
            error = ata_cmd(drive, ATA_CMD_READ, sector, pass);
            if (!error)
                break;
        }
        if (error) {
            ata_print_error((char)(drive+'a'), "read", error, sector);
            return error;
        }

        for (done = 0; done < pass; done++) {
#ifdef CONFIG_FS_XMS
            if (use_xms)
            {
                buffer = _MK_FP(DMASEG, 0);
                read_ioport(BASE(ATA_REG_DATA), buffer, ATA_SECTOR_SIZE);
                xms_fmemcpyw(buf, seg, 0, DMASEG, ATA_SECTOR_SIZE / 2);
            }
            else
#endif
            {
                buffer = _MK_FP(seg, buf);
                read_ioport(BASE(ATA_REG_DATA), buffer, ATA_SECTOR_SIZE);
            }

            buf += ATA_SECTOR_SIZE;
            if (done + 1 < pass) {
                error = ata_wait_drq(ata_rw_ticks());
                if (error) {
                    ata_print_error((char)(drive+'a'), "read", error,
                        sector + done + 1);
                    return error;
                }
            }
        }
        sector += pass;
        count -= pass;
    }

    return 0;
}


/**
 * write to an ATA device
 *
 * drive : physical drive number (0 or 1)
 * sector: sector number
 * buf/seg: I/O buffer address
 */
int ATPROC ata_write(unsigned int drive, sector_t sector, char *buf, ramdesc_t seg,
    unsigned int count)
{
    unsigned char __far *buffer;
    int error;
    int retry;
    unsigned char status;
    unsigned int pass, done;
#ifdef CONFIG_FS_XMS
#pragma GCC diagnostic ignored "-Wshift-count-overflow"
    int use_xms = seg >> 16;
#endif

    if (ata_devtype[drive] == ATA_DEV_ATAPI)
        return -EROFS;

    while (count) {
        pass = ata_xfer_count(count);

        for (retry = 0; retry < ATA_RW_RETRIES; retry++) {
            error = ata_cmd(drive, ATA_CMD_WRITE, sector, pass);
            if (!error)
                break;
        }
        if (error) {
            ata_print_error((char)(drive+'a'), "write", error, sector);
            return error;
        }

        for (done = 0; done < pass; done++) {
#ifdef CONFIG_FS_XMS
            if (use_xms)
            {
                xms_fmemcpyw(0, DMASEG, buf, seg, ATA_SECTOR_SIZE / 2);
                buffer = _MK_FP(DMASEG, 0);
                write_ioport(BASE(ATA_REG_DATA), buffer, ATA_SECTOR_SIZE);
            }
            else
#endif
            {
                buffer = _MK_FP(seg, buf);
                write_ioport(BASE(ATA_REG_DATA), buffer, ATA_SECTOR_SIZE);
            }

            buf += ATA_SECTOR_SIZE;
            if (done + 1 < pass) {
                error = ata_wait_drq(ata_rw_ticks());
                if (error) {
                    ata_print_error((char)(drive+'a'), "write", error,
                        sector + done + 1);
                    return error;
                }
            }
        }

        error = ata_wait(ata_rw_ticks());
        if (error)
            return error;

        status = INB(ATA_REG_STATUS);
        if (status & (ATA_STATUS_ERR|ATA_STATUS_DFE)) {
            ata_save_error(status);
            ata_print_error((char)(drive+'a'), "write", -EIO, sector);
            return -EIO;
        }

        sector += pass;
        count -= pass;
    }

    return 0;
}

int ATPROC ata_flush(unsigned int drive)
{
    if (ata_devtype[drive] != ATA_DEV_DISK)
        return 0;
    return ata_nondata_cmd(drive, ATA_CMD_FLUSH, ata_rw_ticks());
}
