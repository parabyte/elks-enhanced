/*
 * 8-bit ISA DMA only: primary 8237, channels 1 or 3, memory-to-I/O playback.
 * OSS /dev/dsp ioctls; mono U8. No 16-bit ISA DMA (channels 5–7), no SB16
 * high-DMA mode—only the classic SB 2.0–style DSP 0x14 / time-constant path.
 *
 * Tested-class compatibles: Sound Blaster 2.0/Pro layout; OPTi 82C929 (MAD16 Pro)
 * in Sound Blaster Pro mode (same DSP ports; use SET BLASTER=A220 I5 D1 or
 * vendor SNDINIT, then matching sb= or kernel SB_* config).
 *
 * SNDCTL_DSP_SPEED: pass *arg==0 to read current Hz without changing it.
 *
 * SNDCTL_DSP_SETTRIGGER: clearing PCM_ENABLE_OUTPUT makes writes succeed but
 * discards samples (no DMA); GETTRIGGER returns the stored mask.
 *
 * FIONREAD: always 0 (no recording buffer). SNDCTL_DSP_CHANNELS: *arg==0
 * returns channel count (1) without changing configuration.
 */

#include <linuxmt/config.h>

#ifdef CONFIG_CHAR_DEV_DSP

#include <linuxmt/types.h>
#include <linuxmt/major.h>
#include <linuxmt/fs.h>
#include <linuxmt/kernel.h>
#include <linuxmt/sched.h>
#include <linuxmt/mm.h>
#include <linuxmt/errno.h>
#include <linuxmt/string.h>
#include <linuxmt/soundcard.h>
#include <linuxmt/termios.h>
#include <linuxmt/init.h>

#include <arch/io.h>
#include <arch/irq.h>
#include <arch/dma.h>
#include <arch/segment.h>

#define SB_BOUNCE       4096

#define SB_LINADDR(seg, offs) ((unsigned long)(((unsigned long)(seg) << 4) + (unsigned)(offs)))

/* Optional /bootopts sb=port,irq,dma (port 0xNNN or decimal); sb=off disables */
static unsigned char sb_cfg_disable;
static unsigned char sb_cfg_from_bootopts;
static unsigned int sb_cfg_port;
static unsigned char sb_cfg_irq;
static unsigned char sb_cfg_dma;

static unsigned int sb_base;
static unsigned char sb_dma;
static unsigned char sb_irq_line;
static unsigned int sb_rate = 8000;
static unsigned char sb_present;
static volatile unsigned char sb_dma_done;
static unsigned char sb_opened;
static __u32 sb_bytes_played;
static oss_int32_t sb_play_underruns;
static unsigned char sb_vol_l = 100;
static unsigned char sb_vol_r = 100;
static unsigned int sb_trig = PCM_ENABLE_OUTPUT;
static unsigned long sb_bounce_phys;
static unsigned char sb_bounce_wraps;
static unsigned char sb_timeconst;
static jiff_t sb_full_play_ticks;

static unsigned char sb_bounce[SB_BOUNCE];

static int dsp_wait_w(unsigned base)
{
	unsigned port = base + 0x0CU;
	jiff_t deadline = (jiff_t)jiffies() + (HZ / 10) + 1;

	do {
		if ((inb(port) & 0x80) == 0)
			return 0;
		inb(0x61U);
	} while (!time_after(jiffies(), deadline));

	printk("sb: dsp write timeout\n");
	return -EIO;
}

static int dsp_cmd(unsigned char v)
{
	if (dsp_wait_w(sb_base) < 0)
		return -EIO;
	outb(v, sb_base + 0x0CU);
	return 0;
}

static void sb_rate_cache(unsigned int rate)
{
	unsigned int tc = 256U - (1000000U / rate);

	sb_rate = rate;
	if (tc > 255U)
		tc = 255U;
	sb_timeconst = (unsigned char)tc;
	sb_full_play_ticks = (((jiff_t)SB_BOUNCE * (jiff_t)HZ) +
		(jiff_t)rate - 1) / (jiff_t)rate;
	if (sb_full_play_ticks < 1)
		sb_full_play_ticks = 1;
}

static jiff_t sb_play_ticks(unsigned int len)
{
	jiff_t ticks;

	if (len == SB_BOUNCE)
		return sb_full_play_ticks;
	ticks = (((jiff_t)len * (jiff_t)HZ) + (jiff_t)sb_rate - 1) /
		(jiff_t)sb_rate;
	return (ticks < 1) ? 1 : ticks;
}

static int sb_get_arg(char *arg, void *dst, size_t len)
{
	if (!arg)
		return -EINVAL;
	return (verified_memcpy_fromfs(dst, arg, len) == 0) ? 0 : -EFAULT;
}

static int sb_put_arg(char *arg, void *src, size_t len)
{
	if (!arg)
		return -EINVAL;
	return (verified_memcpy_tofs(arg, src, len) == 0) ? 0 : -EFAULT;
}

static int sb_get_arg32(char *arg, oss_int32_t *v)
{
	return sb_get_arg(arg, v, sizeof(*v));
}

static int sb_put_arg32(char *arg, oss_int32_t v)
{
	return sb_put_arg(arg, &v, sizeof(v));
}

static int sb_reset(unsigned base)
{
	int i, j;

	outb(1, base + 6);
	for (i = 0; i < 200; i++)
		inb(0x61U);
	outb(0, base + 6);
	for (j = 0; j < 500; j++) {
		if ((inb(base + 0x0EU) & 0x80) != 0) {
			if (inb(base + 0x0AU) == 0xAA)
				return 0;
		}
	}
	return -1;
}

static void sb_dma_setpage(unsigned char dma, unsigned char page)
{
	switch (dma) {
	case 1:
		outb(page, DMA_PAGE_1);
		break;
	case 3:
		outb(page, DMA_PAGE_3);
		break;
	default:
		break;
	}
}

static void sb_halt(void)
{
	(void)dsp_cmd(0xD0); /* halt DMA (8-bit) */
	clr_irq();
	outb(sb_dma | 4, DMA1_MASK_REG);
	set_irq();
}

/*
 * SB16-style mixer at base+4 / base+5: master + voice (PCM) levels.
 * Needed on many SB Pro compatibles (e.g. OPTi 82C929 MAD16 Pro) or output
 * stays silent despite good DSP DMA.
 * PLAYVOL is 0–100 per channel; mapped linearly to voice regs 0x04 / 0x05.
 */
static void sb_mixer_voice_apply(void)
{
	unsigned a = sb_base + 4u;
	unsigned d = sb_base + 5u;
	unsigned vl = (unsigned)sb_vol_l * 255u / 100u;
	unsigned vr = (unsigned)sb_vol_r * 255u / 100u;

	outb(0x04, a);
	outb((unsigned char)vl, d);
	outb(0x05, a);
	outb((unsigned char)vr, d);
	inb(d);
}

static void sb_mixer_init_default(void)
{
	unsigned a = sb_base + 4u;
	unsigned d = sb_base + 5u;

	sb_vol_l = sb_vol_r = 100;
	outb(0x22, a);
	outb(0xFF, d);
	sb_mixer_voice_apply();
}

static void sb_interrupt(int irq, struct pt_regs *regs)
{
	(void)regs;
	inb(sb_base + 0x0EU);
	inb(sb_base + 0x0FU); /* SB16 16-bit ack; harmless on SB Pro compat */
	if (irq >= 8)
		outb(0x20, 0xA0);
	outb(0x20, 0x20);
	sb_dma_done = 1;
}

/* Returns 0, or negative errno. */
static int sb_play_chunk(unsigned int len)
{
	unsigned long phys = sb_bounce_phys;
	jiff_t deadline;
	jiff_t play_ticks;
	unsigned int c;
	unsigned char dma = sb_dma;
	unsigned char dma_mask = dma | 4;

	if (!len || len > SB_BOUNCE)
		return 0;

	if (sb_bounce_wraps &&
	    (phys & 0xFFFFUL) + (unsigned long)len > 0x10000UL) {
		printk("sb: dma 64k wrap\n");
		return -EIO;
	}

	clr_irq();
	outb(dma_mask, DMA1_MASK_REG);
	outb(0, DMA1_CLEAR_FF_REG);
	outb(DMA_MODE_WRITE | dma, DMA1_MODE_REG);
	sb_dma_setpage(dma, (unsigned char)(phys >> 16));
	outb((unsigned char)phys, (dma << 1) + IO_DMA1_BASE);
	outb((unsigned char)(phys >> 8), (dma << 1) + IO_DMA1_BASE);
	c = len - 1;
	outb(c & 0xFF, (dma << 1) + 1 + IO_DMA1_BASE);
	outb((c >> 8) & 0xFF, (dma << 1) + 1 + IO_DMA1_BASE);
	outb(dma, DMA1_MASK_REG);
	set_irq();

	if (dsp_cmd(0x40) < 0 || dsp_cmd(sb_timeconst) < 0)
		return -EIO;

	sb_dma_done = 0;
	if (dsp_cmd(0x14) < 0 ||
	    dsp_cmd((unsigned char)((len - 1) & 0xFF)) < 0 ||
	    dsp_cmd((unsigned char)((len - 1) >> 8)) < 0)
		return -EIO;

	play_ticks = sb_play_ticks(len);
	/*
	 * QEMU-backed SB16 playback can deliver the completion IRQ noticeably
	 * after the nominal sample time. Keep writes synchronous, but allow
	 * one playback interval plus at least 500ms of slack before failing.
	 */
	deadline = (jiff_t)jiffies() + 2 + play_ticks +
		((play_ticks > (jiff_t)(HZ / 2)) ? play_ticks : (jiff_t)(HZ / 2));
	while (!sb_dma_done && (jiff_t)jiffies() < deadline)
		schedule();

	clr_irq();
	outb(dma_mask, DMA1_MASK_REG);
	set_irq();

	if (!sb_dma_done) {
		printk("sb: irq timeout\n");
		sb_play_underruns++;
		return -EIO;
	}
	return 0;
}

static int sb_open(struct inode *inode, struct file *file)
{
	if (!sb_present)
		return -ENODEV;
	if (MINOR(inode->i_rdev) != 0)
		return -ENODEV;
	if (sb_opened)
		return -EBUSY;
	sb_bytes_played = 0;
	sb_play_underruns = 0;
	sb_trig = PCM_ENABLE_OUTPUT;
	sb_opened = 1;
	return 0;
}

static void sb_release(struct inode *inode, struct file *file)
{
	(void)inode;
	(void)file;
	sb_halt();
	sb_opened = 0;
}

/* No ADC / capture path; OSS apps that read get EOF (0), not EINVAL. */
static size_t sb_read(struct inode *inode, struct file *file, char *buf, size_t count)
{
	(void)inode;
	(void)file;
	(void)buf;
	(void)count;
	return 0;
}

static size_t sb_write(struct inode *inode, struct file *file, char *buf, size_t count)
{
	size_t total = 0;

	(void)inode;

	if (!(file->f_mode & FMODE_WRITE))
		return -EINVAL;

	while (total < count) {
		unsigned int chunk = (unsigned int)(count - total);
		int pr;

		if (chunk > SB_BOUNCE)
			chunk = SB_BOUNCE;
		if (verified_memcpy_fromfs(sb_bounce, buf + total, chunk) != 0)
			return -EFAULT;
		pr = 0;
		if (sb_trig & PCM_ENABLE_OUTPUT)
			pr = sb_play_chunk(chunk);
		if (pr < 0)
			return (size_t)pr;
		sb_bytes_played += (__u32)chunk;
		total += chunk;
	}
	return total;
}

static int sb_ioctl(struct inode *inode, struct file *file, int cmd, char *arg)
{
	oss_int32_t v;
	audio_buf_info abi;
	count_info ci;
	audio_errinfo ae;
	int ret;

	(void)inode;
	(void)file;

	switch (cmd) {
	case FIONREAD: {
		int z = 0;

		return sb_put_arg(arg, &z, sizeof(z));
	}
	case OSS_GETVERSION:
		v = (oss_int32_t)SOUND_VERSION;
		return sb_put_arg32(arg, v);
	/*
	 * OSS defines SNDCTL_DSP_RESET as an obsolete alias of SNDCTL_DSP_HALT;
	 * both use __SIO('P', 0). Stop DMA only.
	 */
	case SNDCTL_DSP_HALT:
	case SNDCTL_DSP_HALT_OUTPUT:
		sb_halt();
		return 0;
	case SNDCTL_DSP_SYNC:
		return 0;
	case SNDCTL_DSP_SPEED:
		ret = sb_get_arg32(arg, &v);
		if (ret < 0)
			return ret;
		/* rate==0: return current (query); does not change sb_rate */
		if (v == 0) {
			v = (oss_int32_t)sb_rate;
			return sb_put_arg32(arg, v);
		}
		if (v < 4000 || v > 48000)
			return -EINVAL;
		sb_rate_cache((unsigned int)v);
		return sb_put_arg32(arg, v);
	case SNDCTL_DSP_SETFMT:
		ret = sb_get_arg32(arg, &v);
		if (ret < 0)
			return ret;
		if (v != AFMT_QUERY && v != AFMT_U8)
			return -EINVAL;
		v = (oss_int32_t)AFMT_U8;
		return sb_put_arg32(arg, v);
	case SNDCTL_DSP_CHANNELS:
		ret = sb_get_arg32(arg, &v);
		if (ret < 0)
			return ret;
		if (v == 0) {
			v = 1;
			return sb_put_arg32(arg, v);
		}
		if (v != 1)
			return -EINVAL;
		v = 1;
		return sb_put_arg32(arg, v);
	case SNDCTL_DSP_STEREO:
		ret = sb_get_arg32(arg, &v);
		if (ret < 0)
			return ret;
		if (v != 0)
			return -EINVAL;
		return 0;
	case SNDCTL_DSP_GETBLKSIZE:
		v = (oss_int32_t)SB_BOUNCE;
		return sb_put_arg32(arg, v);
	case SNDCTL_DSP_GETFMTS:
		v = (oss_int32_t)AFMT_U8;
		return sb_put_arg32(arg, v);
	case SNDCTL_DSP_GETOSPACE:
		abi.fragments = 1;
		abi.fragstotal = 1;
		abi.fragsize = (oss_int32_t)SB_BOUNCE;
		abi.bytes = (oss_int32_t)SB_BOUNCE;
		return sb_put_arg(arg, &abi, sizeof(abi));
	case SNDCTL_DSP_GETISPACE:
		abi.fragments = 0;
		abi.fragstotal = 0;
		abi.fragsize = 0;
		abi.bytes = 0;
		return sb_put_arg(arg, &abi, sizeof(abi));
	case SNDCTL_DSP_GETCAPS:
		v = (oss_int32_t)(PCM_CAP_OUTPUT | PCM_CAP_TRIGGER | 1);
		return sb_put_arg32(arg, v);
	case SNDCTL_DSP_GETTRIGGER: /* Same number as SETTRIGGER on 16-bit int */
		if (!arg)
			return -EINVAL;
		/* Treat as SIOWR: update from user value, then return current mask. */
		if (verified_memcpy_fromfs(&v, arg, sizeof(v)) == 0)
			sb_trig = (unsigned int)v & (PCM_ENABLE_INPUT | PCM_ENABLE_OUTPUT);
		v = (oss_int32_t)sb_trig;
		return sb_put_arg32(arg, v);
	case SNDCTL_DSP_GETIPTR:
		memset(&ci, 0, sizeof(ci));
		return sb_put_arg(arg, &ci, sizeof(ci));
	case SNDCTL_DSP_GETOPTR:
		memset(&ci, 0, sizeof(ci));
		ci.bytes = sb_bytes_played;
		ci.blocks = (oss_int32_t)(sb_bytes_played / (__u32)SB_BOUNCE);
		ci.ptr = (oss_int32_t)(sb_bytes_played % (__u32)SB_BOUNCE);
		return sb_put_arg(arg, &ci, sizeof(ci));
	case SNDCTL_DSP_GETODELAY:
		v = 0; /* synchronous playback; no queued samples after write returns */
		return sb_put_arg32(arg, v);
	case SNDCTL_DSP_GETPLAYVOL: /* Same number as SETPLAYVOL on 16-bit int */
		if (!arg)
			return -EINVAL;
		/* Treat as SIOWR: optional set from user value, then return current. */
		if (verified_memcpy_fromfs(&v, arg, sizeof(v)) == 0) {
			unsigned vl = (unsigned)v & 0xFFU;
			unsigned vr = ((unsigned)v >> 8) & 0xFFU;

			if (vl > 100U)
				vl = 100U;
			if (vr > 100U)
				vr = 100U;
			sb_vol_l = (unsigned char)vl;
			sb_vol_r = (unsigned char)vr;
			if (sb_present)
				sb_mixer_voice_apply();
		}
		v = (oss_int32_t)(sb_vol_l | (sb_vol_r << 8));
		return sb_put_arg32(arg, v);
	case SNDCTL_DSP_GETERROR:
		memset(&ae, 0, sizeof(ae));
		ae.play_underruns = sb_play_underruns;
		ae.play_errorcount = sb_play_underruns;
		if (sb_play_underruns != 0)
			ae.play_lasterror = EIO;
		return sb_put_arg(arg, &ae, sizeof(ae));
	case SNDCTL_DSP_SETSYNCRO:
	case SNDCTL_DSP_SETDUPLEX:
	case SNDCTL_DSP_SKIP:
	case SNDCTL_DSP_HALT_INPUT:
		return 0;
	case SNDCTL_DSP_NONBLOCK:
		/* Playback is always synchronous; ioctl succeeds like Linux OSS no-op. */
		return 0;
	case SNDCTL_DSP_COOKEDMODE:
		return sb_get_arg32(arg, &v);
	case SNDCTL_DSP_SILENCE:
		sb_halt();
		return 0;
	case SNDCTL_DSP_GETCHANNELMASK:
		ret = sb_get_arg32(arg, &v);
		if (ret < 0)
			return ret;
		v = 1; /* mono (front-left only) */
		return sb_put_arg32(arg, v);
	case SNDCTL_DSP_BIND_CHANNEL:
		ret = sb_get_arg32(arg, &v);
		if (ret < 0)
			return ret;
		return sb_put_arg32(arg, v);
	case SNDCTL_DSP_POST:
	case SNDCTL_DSP_SUBDIVIDE:
	case SNDCTL_DSP_SETFRAGMENT:
		return 0;
	default:
		return -EINVAL;
	}
}

static struct file_operations sb_dsp_fops = {
	NULL,
	sb_read,
	sb_write,
	NULL,
	NULL,
	sb_ioctl,
	sb_open,
	sb_release
};

/*
 * /bootopts: sb=0x220,5,1  or  sb=544,5,1
 * sb=off | sb=no | sb=0  — skip SB init (kernel built with driver, no hardware).
 * Overrides CONFIG_SB_PORT, CONFIG_SB_IRQ, CONFIG_SB_DMA when present.
 */
void INITPROC sb_bootopts_parse(char *line)
{
	char *p, *q;
	unsigned long port;
	unsigned int irq, dma;

	while (*line == ' ' || *line == '\t')
		line++;

	if (!strcmp(line, "off") || !strcmp(line, "no") || !strcmp(line, "0")) {
		sb_cfg_disable = 1;
		sb_cfg_from_bootopts = 0;
		printk("sb: disabled by bootopts\n");
		return;
	}

	p = strchr(line, ',');
	if (!p)
		return;
	*p++ = '\0';
	if (line[0] == '0' && line[1] == 'x')
		port = (unsigned long)simple_strtol(line + 2, 16);
	else
		port = (unsigned long)simple_strtol(line, 10);

	q = strchr(p, ',');
	if (!q)
		return;
	*q++ = '\0';
	irq = (unsigned int)simple_strtol(p, 10);
	dma = (unsigned int)simple_strtol(q, 10);

	if (dma != 1U && dma != 3U) {
		printk("sb: bootopts dma must be 1 or 3\n");
		return;
	}
	if (irq > 15U) {
		printk("sb: bootopts bad irq\n");
		return;
	}
	if (port == 0UL || port > 0xFFFUL) {
		printk("sb: bootopts bad port\n");
		return;
	}
	sb_cfg_port = (unsigned int)port;
	sb_cfg_irq = (unsigned char)irq;
	sb_cfg_dma = (unsigned char)dma;
	sb_cfg_from_bootopts = 1;
}

void INITPROC sb_dsp_init(void)
{
	printk("sb: init enter\n");
	if (sb_cfg_disable)
	{
		printk("sb: init skipped (bootopts disable)\n");
		return;
	}

	if (sb_cfg_from_bootopts) {
		sb_base = sb_cfg_port;
		sb_dma = sb_cfg_dma;
		sb_irq_line = sb_cfg_irq;
		printk("sb: cfg bootopts port=0x%x irq=%u dma=%u\n",
		       sb_base, sb_irq_line, sb_dma);
	} else {
		sb_base = (unsigned int)CONFIG_SB_PORT;
		sb_dma = (unsigned char)CONFIG_SB_DMA;
		sb_irq_line = (unsigned char)CONFIG_SB_IRQ;
		printk("sb: cfg kconfig port=0x%x irq=%u dma=%u\n",
		       sb_base, sb_irq_line, sb_dma);
	}

	if (sb_dma != 1 && sb_dma != 3) {
		printk("sb: bad dma %u\n", sb_dma);
		return;
	}
	sb_rate_cache(sb_rate);
	sb_bounce_phys = SB_LINADDR(kernel_ds, (word_t)(unsigned long)sb_bounce);
	sb_bounce_wraps =
		((sb_bounce_phys & 0xFFFFUL) + (unsigned long)SB_BOUNCE > 0x10000UL);
	printk("sb: probing reset at 0x%x\n", sb_base);
	if (sb_reset(sb_base) < 0) {
		printk("sb: not at 0x%x\n", sb_base);
		return;
	}
	printk("sb: reset ok\n");
	printk("sb: requesting irq %u\n", sb_irq_line);
	if (request_irq((int)sb_irq_line, sb_interrupt, INT_GENERIC) != 0) {
		printk("sb: irq %u busy\n", sb_irq_line);
		return;
	}
	printk("sb: irq %u registered\n", sb_irq_line);
	if (dsp_cmd(0xD1) < 0) {
		printk("sb: speaker enable timeout\n");
		free_irq((int)sb_irq_line);
		return;
	}
	sb_mixer_init_default();
	sb_present = 1;

	printk("sb: registering char major %u\n", DSP_MAJOR);
	if (register_chrdev(DSP_MAJOR, "dsp", &sb_dsp_fops) != 0) {
		printk("sb: register_chrdev failed\n");
		free_irq((int)sb_irq_line);
		sb_present = 0;
		return;
	}
	printk("sb: register_chrdev ok\n");
	printk("sb: /dev/dsp 8-bit ISA DMA 0x%x irq %u dma %u%s\n",
	       sb_base, sb_irq_line, sb_dma,
	       sb_cfg_from_bootopts ? " bootopts" : "");
}

#endif /* CONFIG_CHAR_DEV_DSP */
