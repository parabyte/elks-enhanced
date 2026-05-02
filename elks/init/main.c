#include <linuxmt/config.h>
#include <linuxmt/init.h>
#include <linuxmt/mm.h>
#include <linuxmt/sched.h>
#include <linuxmt/types.h>
#include <linuxmt/fcntl.h>
#include <linuxmt/ntty.h>
#include <linuxmt/memory.h>
#include <linuxmt/kernel.h>
#include <linuxmt/string.h>
#include <linuxmt/fs.h>
#include <linuxmt/utsname.h>
#include <linuxmt/netstat.h>
#include <linuxmt/trace.h>
#include <linuxmt/devnum.h>
#include <linuxmt/heap.h>
#include <linuxmt/prectimer.h>
#include <linuxmt/timer.h>
#include <linuxmt/debug.h>
#include <arch/segment.h>
#include <arch/ports.h>
#include <arch/irq.h>
#include <arch/io.h>

/*
 *  System variable setups
 */
#define ENV             1       /* allow environ variables as bootopts*/
#define DEBUG           0       /* display parsing at boot*/

#define MAX_INIT_ARGS   6       /* max # arguments to /bin/init or init= program */
#define MAX_INIT_ENVS   12      /* max # environ variables passed to /bin/init */
#define MAX_INIT_SLEN   80      /* max # words of args + environ passed to /bin/init */
#define MAX_UMB         3       /* max umb= segments in /bootopts */

#define ARRAYLEN(a)     (sizeof(a)/sizeof(a[0]))

struct netif_parms netif_parms[MAX_ETHS] = {

    /* NOTE:  The order must match the defines in netstat.h:
     * ETH_NE2K, ETH_WD, ETH_EL3    */
    { NE2K_IRQ, NE2K_PORT, 0, NE2K_FLAGS },
    { WD_IRQ, WD_PORT, WD_RAM, WD_FLAGS },
    { EL3_IRQ, EL3_PORT, 0, EL3_FLAGS },
};
seg_t kernel_cs, kernel_ds;
int root_mountflags;
int tracing;
int nr_ext_bufs, nr_xms_bufs, nr_map_bufs;
int xms_bootopts;
#ifdef CONFIG_ATA_MODE_DEFAULT
int ata_mode = CONFIG_ATA_MODE_DEFAULT; /* /bootopts xtide= overrides this */
#else
int ata_mode = -1;              /* =AUTO default set ATA CF driver mode automatically */
#endif
#ifdef CONFIG_ATA_BIOSLESS_PROBE
int ata_raw_probe = 1;          /* probe raw ATA/XTIDE hardware without BIOS */
#else
int ata_raw_probe;
#endif
#ifdef CONFIG_BLK_DEV_MFMHD
extern int mfmhd_slow_profile;  /* /bootopts mfm=slow */
#endif
#ifdef CONFIG_BLK_DEV_ATA_CF
extern int ata_slow_profile;    /* /bootopts ata=slow */
#endif
char running_qemu;
static int boot_console;
static segext_t umbtotal;
static kdev_t disabled[4];      /* disabled devices using disable= */
static char bininit[] = "/bin/init";
static char *init_command = bininit;

#ifdef CONFIG_BOOTOPTS
/*
 * Parse /bootopts startup options
 */
#define STR(x)          __STRING(x)
/* bootopts error message are duplicated below so static here for space */
char errmsg_initargs[] = "init args > " STR(MAX_INIT_ARGS) "\n";
char errmsg_initenvs[] = "init envs > " STR(MAX_INIT_ENVS) "\n";
char errmsg_initslen[] = "init words > " STR(MAX_INIT_SLEN) "\n";

/* argv_init doubles as sptr data for sys_execv later*/
static char *argv_init[MAX_INIT_SLEN] = { NULL, bininit, NULL };
static char hasopts;
static int args = 2;    /* room for argc and av[0] */
static int envs;
static int argv_slen;
static kdev_t boot_source_dev;
static char no_local_root;
#if ENV
static char *envp_init[MAX_INIT_ENVS];
#endif

/* this entire structure is released to kernel heap after /bootopts parsing */
static struct {
    struct umbseg {                     /* save umb= lines during /bootopts parse */
        seg_t base;
        segext_t len;
    } umbseg[MAX_UMB], *nextumb;
    unsigned char options[OPTSEGSZ];    /* near data parsing buffer */
} opts;

extern int boot_rootdev;
static int INITPROC parse_options(void);
static void INITPROC finalize_options(void);
static char * INITPROC option(char *s);
static int INITPROC is_local_block_dev(kdev_t dev);
#endif /* CONFIG_BOOTOPTS */

static void FARPROC far_start_kernel(void);
static void INITPROC early_kernel_init(void);
static void INITPROC kernel_init(void);
static void INITPROC kernel_banner(seg_t init, seg_t extra);
static void init_task(void);
static void idle_loop(void);

/*
 * This function is called using the interrupt stack as a temporary stack.
 * The stack is then switched to an unused task struct's kernel stack area while
 * performing the majority of kernel initialization. After that, the stack is
 * switched again to the tiny idle task struct stack area and then becomes the
 * idle task. Must be compiled using -fno-defer-pop, as otherwise stack pointer
 * cleanup is delayed after function calls, which interferes with SP resets.
 * No return is allowed since SP is switched, and the memory used by far_start_kernel
 * is released after kernel initialization is complete.
 */
void start_kernel(void)
{
    //tracing = TRACE_KSTACK | TRACE_ISTACK;
    far_start_kernel();             /* start executing in reusable memory */
}

static void FARPROC far_start_kernel(void)
{
    flag_t flags;                   /* get CPU flag word */
    save_flags(flags);
    clr_irq();                      /* we're running on the kernel interrupt stack! */
    printk("INT %x ", flags);       /* to show interrupt status after setup.S */
    printk("START\n");

    early_kernel_init();            /* read bootopts using kernel interrupt stack */

     /*
      * Allocate the task array + smaller task struct for the idle task.
      * The idle task struct has a smaller stack in t_kstack[] and no t_regs.
      * This works because the idle task always runs at intr_count 1, so
      * interrupts will always save registers onto istack, and never
      * to the t_regs struct at the end of a normal task struct.
      */
     task = heap_alloc(max_tasks * sizeof(struct task_struct) +
         TASK_KSTACK + IDLESTACK_BYTES, HEAP_TAG_TASK|HEAP_TAG_CLEAR);
     if (!task) panic("No task mem");
     idle_task = (struct task_struct *)
         ((char *)task + max_tasks * sizeof(struct task_struct));
    setsp(&(task+1)->t_regs.ax);    /* change to a large temp stack (unused task #1) */
    debug("SP SWITCH\n");

    debug("endbss %x task %x idle_task %x idle_stack %x\n",
        _endbss, task, idle_task, &idle_task->t_kstack[IDLESTACK_BYTES/2]);

    sched_init();                   /* init the idle and other task structs */
    kernel_init();                  /* continue kernel init running on large stack */

    /* allocate task struct #0/pid 1 and setup init_task() to run on next reschedule */
    kfork_proc(init_task);
    wake_up_process(&task[0]);

    idle_loop();                    /* no return */
}

/* the idle task loop, no return */
static void idle_loop(void)
{
    /*
     * Set SP to the small stack in the special idle task struct.
     * We then become the idle task and are only switched to when the last runnable
     * user mode process sleeps from its kernel stack and schedule() is called.
     * As a result, the idle task always runs with intr_count 1, which guarantees
     * interrupt register saves will be on the interrupt stack, not the idle stack.
     *
     * NOTE: Any calls to printk after the small idle stack is set below can cause idle
     * stack overflow. The good news is that the overflow shouldn't cause much harm
     * since it overflows into relatively unused areas of the idle task's task_struct.
     */
    setsp(&idle_task->t_kstack[IDLESTACK_BYTES/2]);
    debug("IDLE LOOP %x\n", getsp());
    //hexdump(idle_task->t_kstack, kernel_ds, IDLESTACK_BYTES, 0, NULL);

    init_bh(TIMER_BH, timer_bh);    /* finally enable timer bottom halves */

    /*
     * In the call to schedule below, the init_task function will run, which
     * completes kernel initialization by mounting the root filesystem, then
     * loads an executable and executes ret_from_syscall, and the system returns
     * from the kernel and enters user mode until the next clock tick or system call.
     */
    while (1) {
#if defined(CHECK_KSTACK) || 1
        if (idle_task->kstack_magic != KSTACK_MAGIC) {
            printk("IDLE STACK OFLOW\n");
            idle_task->kstack_magic = KSTACK_MAGIC;
        }
#endif
        schedule();
#ifdef CONFIG_TIMER_INT0F
        int0F();        /* simulate timer interrupt hooked on IRQ 7 */
#else
        idle_halt();    /* halt until interrupt to save power */
#endif
    }
}

static void INITPROC early_kernel_init(void)
{
    unsigned int heapofs;

    /* Note: no memory allocation available until after heap_init */
    tty_init();                     /* parse_options may call rs_setbaud */
#ifdef CONFIG_TIME_TZ
    tz_init(CONFIG_TIME_TZ);        /* parse_options may call tz_init */
#endif
    ROOT_DEV = SETUP_ROOT_DEV;      /* default root device from boot loader */
#ifdef CONFIG_BOOTOPTS
    boot_source_dev = ROOT_DEV;
    opts.nextumb = opts.umbseg;     /* init static structure variables */
    init_command = argv_init[1];    /* default startup task 1 */
    hasopts = parse_options();      /* parse options found in /bootops */
#endif

    /* create near heap at end of kernel bss */
    heap_init();                    /* init near memory allocator */
    heapofs = setup_arch();          /* sets membase and memend globals */
    heap_add((void *)heapofs, heapsize);
    mm_init(membase, memend);       /* init far/main memory allocator */

#ifdef CONFIG_BOOTOPTS
    struct umbseg *p;
    /* now able to add umb memory segments */
    for (p = opts.umbseg; p < &opts.umbseg[MAX_UMB]; p++) {
        if (p->base) {
            debug("umb segment from %x to %x\n", p->base, p->base + p->len);
            seg_add(p->base, p->base + p->len);
            umbtotal += p->len;
        }
    }
#endif
}

static void INITPROC kernel_init(void)
{
#ifdef CONFIG_ARCH_IBMPC
    outw(0, 0x510);
    if (inb(0x511) == 'Q' && inb(0x511) == 'E')
        running_qemu = 1;
#endif
    irq_init();                     /* installs timer and div fault handlers */

    debug("INT ENB\n");
    set_irq();                      /* interrupts enabled early for jiffie timers */

#ifdef CONFIG_CHAR_DEV_RS
    serial_init();                  /* must init serial before console for ser console */
#endif
    set_console(boot_console);      /* change to /bootopts console= or default */
    console_init();                 /* init direct, bios or headless console */

    inode_init();
    if (buffer_init())  /* also enables xms and unreal mode if configured and possible*/
        panic("No buf mem");
#ifdef CONFIG_SOCKET
    sock_init();
#endif
    device_init();                  /* init char and block devices */

#ifdef CONFIG_BOOTOPTS
    finalize_options();
    if (!hasopts) printk("/bootopts not found or bad format/size\n");
#endif

#ifdef CONFIG_FARTEXT_KERNEL
    /* add .farinit.init section to main memory free list */
    seg_t     init_seg = ((unsigned long)(void __far *)__start_fartext_init) >> 16;
    seg_t s = init_seg + (((word_t)(void *)__start_fartext_init + 15) >> 4);
    seg_t e = init_seg + (((word_t)(void *)  __end_fartext_init + 15) >> 4);
    debug("init: seg %04x to %04x size %04x (%d)\n", s, e, (e - s) << 4, (e - s) << 4);
    seg_add(s, e);
#else
    seg_t s = 0, e = 0;
#endif

    kernel_banner(s, e - s);
}

static void INITPROC kernel_banner(seg_t init, seg_t extra)
{
    kernel_banner_arch();
    printk("syscaps %x, %uK base ram, %d tasks, %d files, %d inodes\n",
        sys_caps, SETUP_MEM_KBYTES, max_tasks, nr_file, nr_inode);
    printk("ELKS %s (%u text, %u ftext, %u data, %u bss, %u heap)\n",
           system_utsname.release,
           (unsigned)_endtext, (unsigned)_endftext, (unsigned)_enddata,
           (unsigned)_endbss - (unsigned)_enddata, heapsize);
    printk("Kernel text %x ", kernel_cs);
#ifdef CONFIG_FARTEXT_KERNEL
    printk("ftext %x init %x ", (unsigned)((long)kernel_init >> 16), init);
#endif
    printk("data %x end %x top %x %u+%u+%uK free\n",
           kernel_ds, membase, memend, (int) ((memend - membase) >> 6),
           extra >> 6, umbtotal >> 6);
}

static void INITPROC try_exec_process(const char *path)
{
    int num;

    num = run_init_process(path);
    if (num) printk("Can't run %s, errno %d\n", path, num);
}

static void INITPROC do_init_task(void)
{
    int num, execinit;
    const char *s;

    mount_root();

    /* when no /bin/init, force initial process group on console to make signals work*/
    execinit = (strcmp(init_command, bininit) == 0) && (sys_access(bininit, 1) == 0);
    if (!execinit)
        current->session = current->pgrp = 1;

    /* Don't open /dev/console for /bin/init, 0-2 closed immediately and fragments heap*/
    //if (!execinit) {
        /* Set stdin/stdout/stderr to /dev/console if not running /bin/init*/
        num = sys_open(s="/dev/console", O_RDWR, 0);
        if (num < 0)
            printk("Unable to open %s (error %d)\n", s, num);
        sys_dup(num);       /* open stdout*/
        sys_dup(num);       /* open stderr*/
    //}

#ifdef CONFIG_BOOTOPTS
    /* Release options parsing buffers and setup data seg */
    heap_add(&opts, sizeof(opts));
#ifdef CONFIG_FS_XMS
    if (xms_enabled == XMS_LOADALL) {
        seg_add(DEF_OPTSEG, 0x80);  /* carve out LOADALL buf 0x800-0x865 from release! */
        seg_add(0x87, DMASEG);
    } else  /* fall through */
#endif
    seg_add(DEF_OPTSEG, DMASEG);    /* DEF_OPTSEG through REL_INITSEG */

    /* run /bin/init or init= command w/argc/argv/env, normally no return*/
    run_init_process_sptr(init_command, (char *)argv_init, argv_slen);
#else
    try_exec_process(init_command);
#endif /* CONFIG_BOOTOPTS */

    printk("No %s - running sh\n", init_command);
    try_exec_process("/bin/sh");
    try_exec_process("/bin/sash");
    panic("No init or sh found");
}

/* this procedure runs in user mode as task 1*/
static void init_task(void)
{
    do_init_task();
}

static struct dev_name_struct {
    const char *name;
    int num;
} devices[] = {
	/* the 8 partitionable drives must be first */
	{ "hda",     DEV_HDA },         /* 0 */
	{ "hdb",     DEV_HDB },
	{ "hdc",     DEV_HDC },
	{ "hdd",     DEV_HDD },
	{ "cfa",     DEV_CFA },         /* direct ATA/IDE/CF */
	{ "cfb",     DEV_CFB },
	{ "mfma",    DEV_MFMA },
	{ "mfmb",    DEV_MFMB },
	{ "fd0",     DEV_FD0 },         /* 8 */
	{ "fd1",     DEV_FD1 },
	{ "df0",     DEV_DF0 },         /* 10 */
	{ "df1",     DEV_DF1 },
	{ "rom",     DEV_ROM },
	{ "ttyS0",   DEV_TTYS0 },       /* 13 */
	{ "ttyS1",   DEV_TTYS1 },
	{ "tty1",    DEV_TTY1 },
	{ "tty2",    DEV_TTY2 },
	{ "tty3",    DEV_TTY3 },
	{ "tty4",    DEV_TTY4 },
	{ "ata",     DEV_CFA },         /* media-neutral aliases for cfa/cfb */
	{ "atb",     DEV_CFB },
	{ NULL,           0 }
};

/*
 * Convert a root device number to name.
 * Device number could be bios device, not kdev_t.
 */
char *root_dev_name(kdev_t dev)
{
    int i;
    unsigned int mask;
#define NAMEOFF 13
    static char name[20] = "ROOTDEV=/dev/";

    name[8] = '/';
    for (i=0; i<13; i++) {
        mask = (i < 8)? 0xfff8: 0xffff;
        if (devices[i].num == (dev & mask)) {
            strcpy(&name[NAMEOFF], devices[i].name);
            if (i < 8) {
                if (dev & 0x07) {
                    int off = NAMEOFF + strlen(devices[i].name);
                    name[off] = '0' + (dev & 7);
                    name[off+1] = '\0';
                }
            }
            return name;
        }
    }
    name[8] = '\0';     /* just return "ROOTDEV=" on not found */
    return name;
}

/* return true if device disabled in disable= list */
int INITPROC dev_disabled(int dev)
{
    int i;

    for (i=0; i < ARRAYLEN(disabled); i++)
        if (disabled[i] == dev)
            return 1;
    return 0;
}

#ifdef CONFIG_BOOTOPTS
static int INITPROC is_local_block_dev(kdev_t dev)
{
    switch (MAJOR(dev)) {
    case RAM_MAJOR:
    case SSD_MAJOR:
    case BIOSHD_MAJOR:
    case FLOPPY_MAJOR:
    case ATHD_MAJOR:
    case ROMFLASH_MAJOR:
    case MFMHD_MAJOR:
        return 1;
    }
    return 0;
}

/*
 * Convert a /dev/ name to device number.
 */
static int INITPROC parse_dev(char * line)
{
    int base = 0;
    struct dev_name_struct *dev = devices;

    if (strncmp(line,"/dev/",5) == 0)
        line += 5;
    do {
        int len = strlen(dev->name);
        if (strncmp(line,dev->name,len) == 0) {
            line += len;
            base = dev->num;
            break;
        }
        dev++;
    } while (dev->name);
    return (base + atoi(line));
}

static int INITPROC str_is_uint(const char *p)
{
    if (!p || !*p)
        return 0;
    while (*p) {
        if (*p < '0' || *p > '9')
            return 0;
        p++;
    }
    return 1;
}

/*
 * console=ttyS1,115200 or console=tty1,ttyS1,115200
 * printk (kernel log) uses the last device in the list; a trailing numeric
 * field sets serial baud via rs_setbaud() on ttySn.
 */
static dev_t INITPROC parse_console_bootdev(char *spec, int *baudp)
{
    char *last_comma, *p, *sep;
    dev_t d, last = 0;

    *baudp = 0;
    last_comma = NULL;
    for (p = spec; *p; p++) {
        if (*p == ',')
            last_comma = p;
    }
    if (last_comma && str_is_uint(last_comma + 1)) {
        *baudp = (int)simple_strtol(last_comma + 1, 10);
        *last_comma = '\0';
    }
    p = spec;
    do {
        sep = strchr(p, ',');
        if (sep)
            *sep++ = '\0';
        d = (dev_t)parse_dev(p);
        if (d)
            last = d;
        p = sep;
    } while (p);
    return last;
}

static void INITPROC comirq(char *line)
{
#if defined(CONFIG_ARCH_IBMPC) && defined(CONFIG_CHAR_DEV_RS)
    int i;
    char *l, *m, c;

    l = line;
    for (i = 0; i < MAX_SERIAL; i++) {  /* assume decimal digits only */
        m = l;
        while ((*l) && (*l != ',')) l++;
        c = *l;     /* ensure robust eol handling */
        if (l > m) {
            *l = '\0';
            set_serial_irq(i, (int)simple_strtol(m, 0));
        }
        if (!c) break;
        l++;
    }
#endif
}

static void INITPROC parse_nic(char *line, struct netif_parms *parms)
{
    char *p;

    parms->irq = (int)simple_strtol(line, 0);
    if ((p = strchr(line, ','))) {
        parms->port = (int)simple_strtol(p+1, 16);
        if ((p = strchr(p+1, ','))) {
            parms->ram = (int)simple_strtol(p+1, 16);
            if ((p = strchr(p+1, ',')))
                parms->flags = (int)simple_strtol(p+1, 0);
        }
    }
}

/* umb= settings have to be saved and processed after parse_options */
static void INITPROC parse_umb(char *line)
{
    char *p = line-1; /* because we start reading at p+1 */
    seg_t base;

    do {
        base = (seg_t)simple_strtol(p+1, 16);
        if((p = strchr(p+1, ':'))) {
            if (opts.nextumb < &opts.umbseg[MAX_UMB]) {
                opts.nextumb->len = (segext_t)simple_strtol(p+1, 16);
                opts.nextumb->base = base;
                opts.nextumb++;
            }
        }
    } while((p = strchr(p+1, ',')));
}

static void INITPROC parse_disable(char *line)
{
    char *p = line;
    kdev_t dev;
    int n = 0;

    do {
        dev = parse_dev(p);
        disabled[n++] = dev;
        p = strchr(p+1, ',');
        if (p)
            *p++ = 0;
    } while (p && n < ARRAYLEN(disabled));
}

/*
 * Boot-time kernel and /bin/init configuration - /bootopts options parser,
 * read early in kernel startup.
 *
 * Known options of the form option=value are handled during kernel init.
 *
 * Unknown options of the same form are saved as var=value and
 * passed as /bin/init's envp array when it runs.
 *
 * Remaining option strings without the character '=' are passed in the order seen
 * as /bin/init's argv array.
 *
 * Note: no memory allocations allowed from this routine.
 */
static int INITPROC parse_options(void)
{
    char *line = (char *)opts.options;
    char *next;

    /* copy /bootopts loaded by boot loader at 0050:0000*/
    fmemcpyb(opts.options, kernel_ds, 0, DEF_OPTSEG, sizeof(opts.options));

#pragma GCC diagnostic ignored "-Wstrict-aliasing"
    /* check file starts with ##, one or two sectors, max 1023 bytes or 511 one sector */
    if (*(unsigned short *)opts.options != 0x2323 ||
        (opts.options[511] && opts.options[OPTSEGSZ-1]))
        return 0;

    next = line;
    while ((line = next) != NULL && *line) {
        if ((next = option(line)) != NULL) {
            if (*line == '#') { /* skip line after comment char*/
                next = line;
                while (*next != '\n' && *next != '\0')
                    next++;
                continue;
            } else *next++ = 0;
        }
        if (*line == 0)     /* skip spaces and linefeeds*/
            continue;
        debug("'%s',", line);
        /*
         * check for kernel options first..
         */
        if (!strncmp(line,"root=",5)) {
            char *root = line + 5;
            int dev;

            if (!strcmp(root, "boot") || !strcmp(root, "source")) {
                ROOT_DEV = boot_source_dev;
                boot_rootdev = 0;  /* allow BIOS boot-drive translation */
                continue;
            }

            dev = parse_dev(root);
            debug("root %s=%D\n", root, dev);
            ROOT_DEV = (kdev_t)dev;
            boot_rootdev = dev;    /* stop translation in device_setup*/
            continue;
        }
        if (!strncmp(line,"nolocalroot=",12)) {
            char *arg = line + 12;

            if (!strcmp(arg, "on") || !strcmp(arg, "yes") ||
                    !strcmp(arg, "1"))
                no_local_root = 1;
            else if (!strcmp(arg, "off") || !strcmp(arg, "no") ||
                    !strcmp(arg, "0"))
                no_local_root = 0;
            continue;
        }
        if (!strncmp(line,"console=",8)) {
            char spec[80];
            int baud = 0;
            dev_t dev;
            char *arg = line + 8;
            size_t i;

            /* no strncpy in kernel link */
            for (i = 0; i < sizeof(spec) - 1 && arg[i]; i++)
                spec[i] = arg[i];
            spec[i] = '\0';
            dev = parse_console_bootdev(spec, &baud);
            if (dev == 0)
                dev = (dev_t)parse_dev(arg);
#ifdef CONFIG_CHAR_DEV_RS
            if (baud > 0 && MINOR(dev) >= RS_MINOR_OFFSET)
                rs_setbaud(dev, (unsigned long)baud);
#endif
            debug("console %s=%D baud=%d,", arg, dev, baud);
            boot_console = dev;
            continue;
        }
        if (!strcmp(line,"ro")) {
            root_mountflags |= MS_RDONLY;
            continue;
        }
        if (!strcmp(line,"rw")) {
            root_mountflags &= ~MS_RDONLY;
            continue;
        }
        if (!strcmp(line,"noatime")) {
            root_mountflags |= MS_NOATIME;
            continue;
        }
        if (!strcmp(line,"strace")) {
            tracing |= TRACE_STRACE;
            continue;
        }
        if (!strcmp(line,"kstack")) {
            tracing |= TRACE_KSTACK;
            continue;
        }
        if (!strcmp(line,"istack")) {
            tracing |= TRACE_ISTACK;
            continue;
        }
        if (!strncmp(line,"init=",5)) {
            line += 5;
            init_command = argv_init[1] = line;
            continue;
        }
        if (!strncmp(line,"ne0=",4)) {
            parse_nic(line+4, &netif_parms[ETH_NE2K]);
            continue;
        }
        if (!strncmp(line,"wd0=",4)) {
            parse_nic(line+4, &netif_parms[ETH_WD]);
            continue;
        }
        if (!strncmp(line,"3c0=",4)) {
            parse_nic(line+4, &netif_parms[ETH_EL3]);
            continue;
        }
#ifdef CONFIG_CHAR_DEV_DSP
        if (!strncmp(line, "sb=", 3)) {
            sb_bootopts_parse(line + 3);
            continue;
        }
        if (!strncmp(line, "mad16=", 6)) {
            mad16_bootopts_parse(line + 6);
            continue;
        }
#endif
        if (!strncmp(line,"debug=", 6)) {
            debug_level = (int)simple_strtol(line+6, 10);
            continue;
        }
        if (!strncmp(line,"buf=",4)) {
            nr_ext_bufs = (int)simple_strtol(line+4, 10);
            continue;
        }
        if (!strncmp(line,"xms=",4)) {
            if (!strcmp(line+4, "on"))    xms_bootopts = XMS_UNREAL;
            if (!strcmp(line+4, "int15")) xms_bootopts = XMS_INT15;
            continue;
        }
        if (!strncmp(line,"xmsbuf=",7)) {
            nr_xms_bufs = (int)simple_strtol(line+7, 10);
            continue;
        }
        if (!strncmp(line,"xtide=",6)) {
            char *arg = line + 6;

            if (!strcmp(arg, "probe") || !strcmp(arg, "raw") ||
                    !strcmp(arg, "biosless")) {
                ata_raw_probe = 1;
            } else if (!strcmp(arg, "noprobe")) {
                ata_raw_probe = 0;
            } else {
                ata_mode = (int)simple_strtol(arg, 10);
            }
            continue;
        }
#ifdef CONFIG_BLK_DEV_ATA_CF
        if (!strncmp(line,"ata=",4) || !strncmp(line,"ide=",4)) {
            char *arg = line + 4;

            if (!strcmp(arg, "slow") || !strcmp(arg, "1") ||
                    !strcmp(arg, "on"))
                ata_slow_profile = 1;
            else if (!strcmp(arg, "fast") || !strcmp(arg, "0") ||
                    !strcmp(arg, "off"))
                ata_slow_profile = 0;
            continue;
        }
#endif
#ifdef CONFIG_BLK_DEV_MFMHD
        if (!strncmp(line,"mfm=",4)) {
            if (!strcmp(line+4, "slow") || !strcmp(line+4, "1") ||
                    !strcmp(line+4, "on"))
                mfmhd_slow_profile = 1;
            else if (!strcmp(line+4, "fast") || !strcmp(line+4, "0") ||
                    !strcmp(line+4, "off"))
                mfmhd_slow_profile = 0;
            continue;
        }
#endif
        if (!strncmp(line,"cache=",6)) {
            nr_map_bufs = (int)simple_strtol(line+6, 10);
            continue;
        }
        if (!strncmp(line,"heap=",5)) {
            heapsize = (unsigned int)simple_strtol(line+5, 10);
            continue;
        }
        if (!strncmp(line,"task=",5)) {
            max_tasks = (int)simple_strtol(line+5, 10);
            continue;
        }
        if (!strncmp(line,"inode=",6)) {
            nr_inode = (int)simple_strtol(line+6, 10);
            continue;
        }
        if (!strncmp(line,"file=",5)) {
            nr_file = (int)simple_strtol(line+5, 10);
            continue;
        }
        if (!strncmp(line,"comirq=",7)) {
            comirq(line+7);
            continue;
        }
        if (!strncmp(line,"umb=",4)) {
            parse_umb(line+4);
            continue;
        }
        if (!strncmp(line,"disable=",8)) {
            parse_disable(line+8);
            continue;
        }
        if (!strncmp(line,"TZ=",3)) {
            tz_init(line+3);
            /* fall through and add line to environment */
        }

        /*
         * Then check if it's an environment variable or an init argument.
         */
        if (!strchr(line,'=')) {    /* no '=' means init argument*/
            if (args < MAX_INIT_ARGS)
                argv_init[args++] = line;
            else printk(errmsg_initargs);
        }
#if ENV
        else {
            if (envs < MAX_INIT_ENVS)
                envp_init[envs++] = line;
            else printk(errmsg_initenvs);
        }
#endif
    }
    if (no_local_root && is_local_block_dev(ROOT_DEV)) {
        printk("root: local root disabled, using boot source\n");
        ROOT_DEV = boot_source_dev;
        boot_rootdev = 0;  /* allow BIOS boot-drive translation */
    }
    debug("\n");
    return 1;   /* success*/
}

static void INITPROC finalize_options(void)
{
    int i;

#if ENV
    /* set ROOTDEV environment variable for rc.sys fsck*/
    if (envs + running_qemu < MAX_INIT_ENVS) {
        envp_init[envs++] = root_dev_name(ROOT_DEV);
        if (running_qemu)
            envp_init[envs++] = (char *)"QEMU=1";
    } else printk(errmsg_initenvs);
#endif

#if DEBUG
    printk("args: ");
    for (i=1; i<args; i++)
        printk("'%s'", argv_init[i]);
    printk("\n");

#if ENV
    printk("envp: ");
    for (i=0; i<envs; i++)
        printk("'%s'", envp_init[i]);
    printk("\n");
#endif
#endif

    /* convert argv array to stack array for sys_execv*/
    args--;
    argv_init[0] = (char *)args;            /* 0 = argc*/
    char *q = (char *)&argv_init[args+2+envs+1];
    for (i=1; i<=args; i++) {               /* 1..argc = av*/
        char *p = argv_init[i];
        char *savq = q;
        while ((*q++ = *p++) != 0)
            ;
        argv_init[i] = (char *)(savq - (char *)argv_init);
    }
    /*argv_init[args+1] = NULL;*/           /* argc+1 = 0*/
#if ENV
    if (envs) {
        for (i=0; i<envs; i++) {
            char *p = envp_init[i];
            char *savq = q;
            while ((*q++ = *p++) != 0)
                ;
            argv_init[args+2+i] = (char *)(savq - (char *)argv_init);
        }

    }
#endif
    /*argv_init[args+2+envs] = NULL;*/
    argv_slen = q - (char *)argv_init;
    if (argv_slen > sizeof(argv_init))
        panic(errmsg_initslen);
}

/* return whitespace-delimited string*/
static char * INITPROC option(char *s)
{
    char *t = s;
    if (*s == '#')
        return s;
    for(; *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n'; ++s, ++t) {
        if (*s == '\0')
            return NULL;
        if (*s == '"') {
            s++;
            while (*s != '"') {
                if (*s == '\0')
                    return NULL;
                *t++ = *s++;
            }
            *t++ = 0;
            break;
        }
    }
    return s;
}
#endif /* CONFIG_BOOTOPTS*/
