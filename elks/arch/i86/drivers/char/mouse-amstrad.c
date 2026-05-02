/*
 * Minimal Amstrad PC1640 mouse driver.
 *
 * The Amstrad mouse has signed X/Y movement counters at ports 0x78 and 0x7a.
 * Button transitions arrive as keyboard matrix codes and are shadowed by the
 * scancode keyboard driver.  This driver exposes movement as PS/2-style
 * 3-byte packets on /dev/psaux so existing ELKS/Nano-X mouse code can use
 * MOUSE_TYPE=ps2 without a new userspace decoder.
 */

#include <linuxmt/config.h>
#include <linuxmt/wait.h>
#include <linuxmt/chqueue.h>
#include <linuxmt/sched.h>
#include <linuxmt/timer.h>
#include <linuxmt/errno.h>
#include <linuxmt/ntty.h>
#include <linuxmt/kernel.h>
#include <linuxmt/memory.h>
#include <arch/param.h>
#include <arch/io.h>
#include <arch/irq.h>
#include <arch/system.h>

#ifdef CONFIG_MOUSE_AMSTRAD

#define AMSTRAD_MOUSE_X         0x78
#define AMSTRAD_MOUSE_Y         0x7a
#define AMSTRAD_MOUSE_POLL      (((HZ + 27) / 55) ? ((HZ + 27) / 55) : 1)

#define AMSTRAD_PPI_A           0x60
#define AMSTRAD_PPI_B           0x61
#define AMSTRAD_STATUS1_SELECT  0x80
#define AMSTRAD_STATUS1_FIXED   0x0d
#define AMSTRAD_STATUS1_MASK    0x8d

#define AMSTRAD_CGA_INDEX       0x3d4
#define AMSTRAD_PRINTER_CTRL    0x37a
#define AMSTRAD_PC1640_OPT      0x20

#define AMSTRAD_MOUSE_BTN1      0x7e
#define AMSTRAD_MOUSE_BTN2      0x7d

#define PS2_ALWAYS              0x08
#define PS2_LEFT                0x01
#define PS2_RIGHT               0x02
#define PS2_X_SIGN              0x10
#define PS2_Y_SIGN              0x20

static struct timer_list amstrad_mouse_timer;
static unsigned char amstrad_buttons;
static unsigned char amstrad_last_buttons;
static unsigned char amstrad_open;

static void amstrad_mouse_poll(int data);

static int amstrad_rom_string(const char *sig, int len)
{
    unsigned int off;
    int i;

    for (off = 0; off < 0x10000 - len; off++) {
        for (i = 0; i < len; i++)
            if (peekb(off + i, 0xf000) != sig[i])
                break;
        if (i == len)
            return 1;
    }
    return 0;
}

static int amstrad_bios_present(void)
{
    return amstrad_rom_string("Amstrad PC", 10)
        || amstrad_rom_string("AMSTRAD PC", 10);
}

static int amstrad_status1_present(void)
{
    unsigned int flags;
    unsigned char portb;
    unsigned char status;

    save_flags(flags);
    clr_irq();

    portb = inb(AMSTRAD_PPI_B);
    outb(portb | AMSTRAD_STATUS1_SELECT, AMSTRAD_PPI_B);
    status = inb(AMSTRAD_PPI_A);
    outb(portb, AMSTRAD_PPI_B);

    restore_flags(flags);

    return (status & AMSTRAD_STATUS1_MASK) == AMSTRAD_STATUS1_FIXED;
}

static int amstrad_pc1640_present(void)
{
    unsigned int flags;
    unsigned char opt;

    if (sys_caps & CAP_PC_AT)
        return 0;

    save_flags(flags);
    clr_irq();

    (void)inb(AMSTRAD_CGA_INDEX);
    opt = inb(AMSTRAD_PRINTER_CTRL);

    restore_flags(flags);

    return (opt & AMSTRAD_PC1640_OPT) == 0;
}

int amstrad_mouse_button(unsigned char code, int released)
{
    unsigned char mask;

    if (code == AMSTRAD_MOUSE_BTN1)
        mask = PS2_LEFT;
    else if (code == AMSTRAD_MOUSE_BTN2)
        mask = PS2_RIGHT;
    else
        return 0;

    if (released)
        amstrad_buttons &= ~mask;
    else
        amstrad_buttons |= mask;
    return 1;
}

static void amstrad_restart_timer(void)
{
    amstrad_mouse_timer.tl_expires = jiffies + AMSTRAD_MOUSE_POLL;
    amstrad_mouse_timer.tl_function = amstrad_mouse_poll;
    add_timer(&amstrad_mouse_timer);
}

static int amstrad_mouse_present(void)
{
    int i;

    outb(0, AMSTRAD_MOUSE_X);
    outb(0, AMSTRAD_MOUSE_Y);

    for (i = 0; i < 4; i++) {
        if (inb(AMSTRAD_MOUSE_X) != 0 || inb(AMSTRAD_MOUSE_Y) != 0)
            return 0;
    }
    return 1;
}

static void amstrad_queue_packet(struct ch_queue *q, unsigned char buttons,
                                 signed char dx, signed char dy)
{
    unsigned char flags = PS2_ALWAYS | buttons;

    if (dx < 0)
        flags |= PS2_X_SIGN;
    if (dy < 0)
        flags |= PS2_Y_SIGN;

    chq_addch_nowakeup(q, flags);
    chq_addch_nowakeup(q, (unsigned char)dx);
    chq_addch_nowakeup(q, (unsigned char)dy);
}

static void amstrad_mouse_poll(int data)
{
    struct ch_queue *q = &ttys[NR_CONSOLES + NR_PTYS + NR_SERIAL].inq;
    signed char dx = (signed char)inb(AMSTRAD_MOUSE_X);
    signed char dy = (signed char)inb(AMSTRAD_MOUSE_Y);
    unsigned char buttons = amstrad_buttons;

    outb(0, AMSTRAD_MOUSE_X);
    outb(0, AMSTRAD_MOUSE_Y);

    if ((dx || dy || buttons != amstrad_last_buttons) && q->size - q->len >= 3) {
        amstrad_last_buttons = buttons;
        amstrad_queue_packet(q, buttons, dx, dy);
        wake_up(&q->wait);
    }

    if (amstrad_open)
        amstrad_restart_timer();
}

static int amstrad_open_mouse(struct tty *tty)
{
    int err;

    if (tty->usecount)
        return -EBUSY;

    if (!amstrad_bios_present() || !amstrad_status1_present()
        || !amstrad_pc1640_present() || !amstrad_mouse_present())
        return -ENODEV;

    err = tty_allocq(tty, MOUSEINQ_SIZE, 0);
    if (err)
        return err;

    amstrad_buttons = 0;
    amstrad_last_buttons = 0;
    amstrad_open = 1;
    tty->usecount = 1;
    amstrad_restart_timer();

    return 0;
}

static void amstrad_release_mouse(struct tty *tty)
{
    if (--tty->usecount == 0) {
        amstrad_open = 0;
        del_timer(&amstrad_mouse_timer);
        tty_freeq(tty);
    }
}

static int amstrad_write_mouse(struct tty *tty)
{
    return 0;
}

static int amstrad_ioctl_mouse(struct tty *tty, int cmd, char *arg)
{
    switch (cmd) {
    case TCSETS:
    case TCSETSW:
    case TCSETSF:
        return 0;
    }
    return -EINVAL;
}

struct tty_ops amstrad_mouse_ops = {
    amstrad_open_mouse,
    amstrad_release_mouse,
    amstrad_write_mouse,
    NULL,
    amstrad_ioctl_mouse,
    NULL
};
#endif
