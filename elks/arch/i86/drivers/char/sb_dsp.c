/*
 * 8-bit ISA DMA only: primary 8237, channels 1 or 3, memory-to-I/O playback.
 * OSS /dev/dsp ioctls; mono U8 by default. No 16-bit ISA DMA (channels 5–7),
 * no SB16 high-DMA mode. Uses the classic SB 2.0-style DSP 0x14 /
 * time-constant path.
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
 * returns channel count without changing configuration.
 */

#include <linuxmt/config.h>

#ifdef CONFIG_CHAR_DEV_DSP

#include <linuxmt/types.h>
#include <linuxmt/major.h>
#include <linuxmt/fs.h>
#include <linuxmt/kernel.h>
#include <linuxmt/sched.h>
#include <linuxmt/mm.h>
#include <linuxmt/memory.h>
#include <linuxmt/errno.h>
#include <linuxmt/string.h>
#include <linuxmt/soundcard.h>
#include <linuxmt/termios.h>
#include <linuxmt/init.h>

#include <arch/io.h>
#include <arch/irq.h>
#include <arch/dma.h>
#include <arch/segment.h>

#ifndef CONFIG_SB_BOUNCE
#define CONFIG_SB_BOUNCE 4096
#endif

/*
 * Single 8237 DMA window. This affects transfer chunking only; sample format,
 * rate programming, and MAD16/WSS bring-up stay identical.
 */
#define SB_BOUNCE       CONFIG_SB_BOUNCE
#define SB_DEFAULT_PLAYVOL 50

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
#ifdef CONFIG_SB_STEREO
static unsigned char sb_channels = 1;
#define SB_BYTE_RATE(r) ((r) * (unsigned int)sb_channels)
#else
#define SB_BYTE_RATE(r) (r)
#endif
static unsigned char sb_present;
static unsigned char sb_opened;
static __u32 sb_bytes_played;
static oss_int32_t sb_play_underruns;
static unsigned char sb_vol_l = 100;
static unsigned char sb_vol_r = 100;
static unsigned int sb_trig = PCM_ENABLE_OUTPUT;
static unsigned char sb_timeconst;
static volatile unsigned char sb_dma_done;
static unsigned char sb_dsp_ver_major;
static unsigned char sb_dsp_ver_minor;
static unsigned int sb_dma_addr_port;
static unsigned int sb_dma_count_port;
static unsigned int sb_dma_page_reg;
static unsigned char sb_dma_mask;
static unsigned char sb_dma_tc_bit;
static unsigned long sb_bounce_phys;
static unsigned char sb_active;
static unsigned int sb_active_len;
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
#ifdef CONFIG_SB_STEREO
	unsigned int byte_rate = SB_BYTE_RATE(rate);
	unsigned int tc = 256U - (1000000U / byte_rate);
#else
	unsigned int tc = 256U - (1000000U / rate);
#endif

	sb_rate = rate;
	if (tc > 255U)
		tc = 255U;
	sb_timeconst = (unsigned char)tc;
#ifdef CONFIG_SB_STEREO
	sb_full_play_ticks = (((jiff_t)SB_BOUNCE * (jiff_t)HZ) +
		(jiff_t)byte_rate - 1) / (jiff_t)byte_rate;
#else
	sb_full_play_ticks = (((jiff_t)SB_BOUNCE * (jiff_t)HZ) +
		(jiff_t)rate - 1) / (jiff_t)rate;
#endif
	if (sb_full_play_ticks < 1)
		sb_full_play_ticks = 1;
}

static jiff_t sb_play_ticks(unsigned int len)
{
	jiff_t ticks;
#ifdef CONFIG_SB_STEREO
	unsigned int byte_rate = SB_BYTE_RATE(sb_rate);
#endif

	if (len == SB_BOUNCE)
		return sb_full_play_ticks;
#ifdef CONFIG_SB_STEREO
	ticks = (((jiff_t)len * (jiff_t)HZ) + (jiff_t)byte_rate - 1) /
		(jiff_t)byte_rate;
#else
	ticks = (((jiff_t)len * (jiff_t)HZ) + (jiff_t)sb_rate - 1) /
		(jiff_t)sb_rate;
#endif
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

#ifdef CONFIG_SB_OPTI82C929
/*
 * OPTi 82C929 MC registers (Linux opti9xx map): password 0xE3 at 0xF8F,
 * MC1..MC6 at 0xF8D..0xF92.
 *
 * MC1,2,4,5,6 follow the Linux mad16.c C929 bring-up profile, with WSS/CD
 * decode kept off for the ELKS SB-only driver. Linux writes MC4=0xa2,
 * MC5=0xa5|cs4231_mode, MC6=0x03; ELKS uses the base 0xa5 MC5 value because
 * it does not probe the WSS codec here. Only MC3 is rebuilt for the active
 * sb= / SB_* port, IRQ, and DMA instead of Linux's WSS-only MC3=0xf0.
 */
#define OPTI929_PWD_PORT 0xF8FU
#define OPTI929_PASSWORD 0xE3U
#define OPTI929_MC1 0xF8DU
#define OPTI929_MC2 0xF8EU
#define OPTI929_MC3 0xF8FU
#define OPTI929_MC4 0xF90U
#define OPTI929_MC5 0xF91U
#define OPTI929_MC6 0xF92U
#define OPTI929_WSS_BASE 0x530U
#define OPTI929_CODEC_BASE (OPTI929_WSS_BASE + 4U)

#define OPTI929_MC1_DEFAULT 0x00U
#define OPTI929_MC2_DEFAULT 0x03U
#define OPTI929_MC4_DEFAULT 0xA2U
#define OPTI929_MC6_DEFAULT 0x03U

#define OPTI929_MC1_MASK (0x40U | 0x01U)
#define OPTI929_MC2_OPL4 0x20U
#define OPTI929_MC2_CDSEL_DISABLED 0x03U
#define OPTI929_MC3_IRQ_MASK 0xC0U
#define OPTI929_MC3_DMA_MASK 0x30U
#define OPTI929_MC3_SB_240 0x04U
#define OPTI929_MC3_GP_TIMER 0x02U /* write GPMODE; read overlaps REV bit 1 */
#define OPTI929_MC5_MUST1_7 0x80U
#define OPTI929_MC5_MUST0_6 0x40U
#define OPTI929_MC5_SHPASS 0x20U
#define OPTI929_MC5_SPACCESS 0x10U
#define OPTI929_MC5_CFIFO 0x08U
#define OPTI929_MC5_MUST1_2 0x04U
#define OPTI929_MC5_CFIX 0x02U
#define OPTI929_MC5_MUST1_0 0x01U
#define OPTI929_MC6_MPU_ENABLE 0x80U
#define OPTI929_MC6_MPU_IRQ 0x18U
#define OPTI929_MC6_MPU_OFF_MASK 0x07U

#define OPTI929_MC5_DEFAULT (OPTI929_MC5_MUST1_7 | OPTI929_MC5_SHPASS | \
			     OPTI929_MC5_MUST1_2 | OPTI929_MC5_MUST1_0)

static unsigned char opti929_rd(unsigned port)
{
	unsigned int flags;
	unsigned char val;

	save_flags(flags);
	clr_irq();
	outb(OPTI929_PASSWORD, OPTI929_PWD_PORT);
	val = inb(port);
	restore_flags(flags);
	return val;
}

static void opti929_wr(unsigned port, unsigned char val)
{
	unsigned int flags;

	save_flags(flags);
	clr_irq();
	outb(OPTI929_PASSWORD, OPTI929_PWD_PORT);
	outb(val, port);
	restore_flags(flags);
}

static int opti929_detect(void)
{
	unsigned char mc1, ungated, toggled;

	mc1 = opti929_rd(OPTI929_MC1);
	if (mc1 == 0xFFU)
		return 0;

	ungated = inb(OPTI929_MC1);
	if (ungated == mc1)
		return 0;

	opti929_wr(OPTI929_MC1, (unsigned char)(mc1 ^ 0x80U));
	toggled = opti929_rd(OPTI929_MC1);
	opti929_wr(OPTI929_MC1, mc1);
	return toggled == (unsigned char)(mc1 ^ 0x80U);
}

/* Mirrors mad16cfg encode_mc3() + xt_sanitize_regs() MC3 IRQ/DMA rules. */
static unsigned char opti929_mc3_for_sb(unsigned int sb_port, unsigned char irq,
					unsigned char dma)
{
	unsigned char mc3 = OPTI929_MC3_GP_TIMER;

	if (irq == 5)
		mc3 |= 0x80U;
	if (dma == 3)
		mc3 |= 0x20U;
	if (sb_port == 0x240U)
		mc3 |= OPTI929_MC3_SB_240;

	if ((mc3 & OPTI929_MC3_IRQ_MASK) == 0x40U)
		mc3 = (unsigned char)((mc3 & ~OPTI929_MC3_IRQ_MASK) |
				      OPTI929_MC3_IRQ_MASK);
	if ((mc3 & OPTI929_MC3_DMA_MASK) == 0x10U)
		mc3 = (unsigned char)((mc3 & ~OPTI929_MC3_DMA_MASK) |
				      OPTI929_MC3_DMA_MASK);
	return mc3;
}

static int opti929_codec_wait(unsigned int base)
{
	unsigned int timeout = 60000U;

	while (timeout-- != 0U) {
		if ((inb(base) & 0x80U) == 0)
			return 0;
		inb(0x61U);
	}
	return -EIO;
}

static unsigned char opti929_codec_read(unsigned int base, unsigned char reg,
					unsigned char mce)
{
	unsigned int flags;
	unsigned char value;

	(void)opti929_codec_wait(base);
	save_flags(flags);
	clr_irq();
	outb((unsigned char)((reg & 0x1FU) | mce), base);
	value = inb(base + 1U);
	restore_flags(flags);
	return value;
}

static void opti929_codec_write(unsigned int base, unsigned char reg,
				unsigned char value, unsigned char mce)
{
	unsigned int flags;

	(void)opti929_codec_wait(base);
	save_flags(flags);
	clr_irq();
	outb((unsigned char)((reg & 0x1FU) | mce), base);
	outb(value, base + 1U);
	restore_flags(flags);
}

static void opti929_codec_leave_mce(unsigned int base)
{
	unsigned long timeout;

	(void)opti929_codec_wait(base);
	outb(0x00, base);
	outb(0x00, base);

	timeout = 80000U;
	while (timeout-- != 0U &&
	       (opti929_codec_read(base, 11, 0x00) & 0x20U) != 0U)
		inb(0x61U);
}

static int opti929_codec_init(unsigned int base, unsigned char *mc5_extra)
{
	static unsigned char init_values[32] = {
		0xa8, 0xa8, 0x08, 0x08, 0x08, 0x08, 0x00, 0x00,
		0x00, 0x0c, 0x02, 0x00, 0x8a, 0x01, 0x00, 0x00,
		0x80, 0x00, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};
	unsigned char tmp, cs_compat = 0;
	unsigned int i;

	*mc5_extra = 0;
	if (opti929_codec_wait(base) < 0 || (inb(base) & 0x80U) != 0U)
		return 0;

	/*
	 * Same writable-register sanity check Linux ad1848_detect() uses,
	 * trimmed to the registers we will immediately reinitialize.
	 */
	opti929_codec_write(base, 0, 0xaa, 0x40);
	opti929_codec_write(base, 1, 0x45, 0x40);
	if (opti929_codec_read(base, 0, 0x40) != 0xaa ||
	    opti929_codec_read(base, 1, 0x40) != 0x45)
		return 0;
	opti929_codec_write(base, 0, 0x45, 0x40);
	opti929_codec_write(base, 1, 0xaa, 0x40);
	if (opti929_codec_read(base, 0, 0x40) != 0x45 ||
	    opti929_codec_read(base, 1, 0x40) != 0xaa)
		return 0;

	opti929_codec_write(base, 12, 0x40, 0x40);
	tmp = opti929_codec_read(base, 12, 0x40);
	if ((tmp & 0x80U) != 0U)
		cs_compat = 1;

	for (i = 0; i < 16U; i++)
		opti929_codec_write(base, (unsigned char)i, init_values[i], 0x40);
	opti929_codec_write(base, 9,
			    (unsigned char)(opti929_codec_read(base, 9, 0x40) | 0x04U),
			    0x40);
	if ((tmp & 0xC0U) == 0xC0U) {
		opti929_codec_write(base, 12,
				    (unsigned char)(opti929_codec_read(base, 12, 0x40) | 0x40U),
				    0x40);
		for (i = 16U; i < 32U; i++)
			opti929_codec_write(base, (unsigned char)i,
					    init_values[i], 0x40);
	}
	outb(0x00, base + 2U);
	opti929_codec_leave_mce(base);

	if (cs_compat)
		*mc5_extra = OPTI929_MC5_CFIX;
	return 1;
}

static unsigned char opti929_wss_dma_cfg(unsigned char dma)
{
	return (dma == 3U) ? 0x03U : 0x02U;
}

static void opti82c929_early_init(unsigned int sb_port, unsigned char irq,
				  unsigned char dma)
{
	unsigned char mc1, mc2, mc3, mc4, mc5, mc6, mc5_extra = 0;

	if (sb_port != 0x220U && sb_port != 0x240U)
		return;
	if (irq != 5U && irq != 7U)
		return;
	if (dma != 1U && dma != 3U)
		return;
	if (!opti929_detect())
		return;

	mc1 = OPTI929_MC1_DEFAULT;
	mc2 = OPTI929_MC2_DEFAULT;
	mc3 = opti929_mc3_for_sb(sb_port, irq, dma);
	mc4 = OPTI929_MC4_DEFAULT;
	mc5 = OPTI929_MC5_DEFAULT;
	mc6 = OPTI929_MC6_DEFAULT;

	mc1 &= OPTI929_MC1_MASK;
	mc2 = (unsigned char)((mc2 & OPTI929_MC2_OPL4) | OPTI929_MC2_CDSEL_DISABLED);
	mc5 &= (unsigned char)~OPTI929_MC5_MUST0_6;
	mc5 |= OPTI929_MC5_MUST1_7 | OPTI929_MC5_MUST1_2 |
	       OPTI929_MC5_MUST1_0;
	if ((mc6 & OPTI929_MC6_MPU_ENABLE) == 0)
		mc6 &= OPTI929_MC6_MPU_OFF_MASK;
	else if ((mc6 & OPTI929_MC6_MPU_IRQ) == 0x00U ||
		 (mc6 & OPTI929_MC6_MPU_IRQ) == 0x08U)
		mc6 = (unsigned char)((mc6 & ~OPTI929_MC6_MPU_IRQ) | 0x18U);

	/*
	 * Linux mad16.c initializes the attached WSS codec before using the
	 * C929 profile. Without this, some boards accept SB DSP commands but
	 * never drain enough of the internal FIFO to keep DRQ asserted.
	 */
	opti929_wr(OPTI929_MC1, 0x80U);  /* WSS mode, WSBase 0x530 */
	opti929_wr(OPTI929_MC2, mc2);
	opti929_wr(OPTI929_MC3, 0xF0U);  /* disable SB while WSS is active */
	opti929_wr(OPTI929_MC4, mc4);
	opti929_wr(OPTI929_MC5, mc5);
	opti929_wr(OPTI929_MC6, mc6);
	outb(opti929_wss_dma_cfg(dma), OPTI929_WSS_BASE);
	(void)opti929_codec_init(OPTI929_CODEC_BASE, &mc5_extra);

	mc5 |= mc5_extra;
	opti929_wr(OPTI929_MC1, mc1);
	opti929_wr(OPTI929_MC2, mc2);
	opti929_wr(OPTI929_MC3, mc3);
	opti929_wr(OPTI929_MC4, mc4);
	opti929_wr(OPTI929_MC5, mc5);
	opti929_wr(OPTI929_MC6, mc6);
}
#endif /* CONFIG_SB_OPTI82C929 */

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

static int dsp_read_byte(unsigned char *value)
{
	jiff_t deadline = (jiff_t)jiffies() + (HZ / 10) + 1;

	do {
		if ((inb(sb_base + 0x0EU) & 0x80U) != 0U) {
			*value = inb(sb_base + 0x0AU);
			return 0;
		}
		inb(0x61U);
	} while (!time_after(jiffies(), deadline));

	return -EIO;
}

static int sb_read_dsp_version(void)
{
	if (dsp_cmd(0xE1) < 0)
		return -EIO;
	if (dsp_read_byte(&sb_dsp_ver_major) < 0 ||
	    dsp_read_byte(&sb_dsp_ver_minor) < 0) {
		sb_dsp_ver_major = 0;
		sb_dsp_ver_minor = 0;
		return -EIO;
	}
	return 0;
}

static void sb_dma_cache(void)
{
	sb_dma_addr_port = (unsigned int)((sb_dma << 1) + IO_DMA1_BASE);
	sb_dma_count_port = sb_dma_addr_port + 1U;
	sb_dma_page_reg = (sb_dma == 1U) ? DMA_PAGE_1 : DMA_PAGE_3;
	sb_dma_mask = sb_dma | 4U;
	sb_dma_tc_bit = (unsigned char)(1U << sb_dma);
}

static unsigned int sb_dma_count_locked(void)
{
	unsigned int count;

	outb(0, DMA1_CLEAR_FF_REG);
	count = (unsigned int)inb(sb_dma_count_port);
	count |= (unsigned int)inb(sb_dma_count_port) << 8;
	return count + 1U;
}

static void sb_dma_program_locked(unsigned int len)
{
	unsigned int c = len - 1U;

	outb(sb_dma_mask, DMA1_MASK_REG);
	(void)inb(DMA1_STAT_REG); /* clear prior terminal-count bits */
	outb(0, DMA1_CLEAR_FF_REG);
	outb(DMA_MODE_WRITE | sb_dma, DMA1_MODE_REG);
	outb((unsigned char)(sb_bounce_phys >> 16), sb_dma_page_reg);
	outb((unsigned char)sb_bounce_phys, sb_dma_addr_port);
	outb((unsigned char)(sb_bounce_phys >> 8), sb_dma_addr_port);
	outb(c & 0xFF, sb_dma_count_port);
	outb((c >> 8) & 0xFF, sb_dma_count_port);
	outb(sb_dma, DMA1_MASK_REG);
}

static int sb_dsp_start(unsigned int len)
{
	if (dsp_cmd(0x40) < 0 || dsp_cmd(sb_timeconst) < 0)
		return -EIO;
	if (dsp_cmd(0x14) < 0 ||
	    dsp_cmd((unsigned char)((len - 1U) & 0xFFU)) < 0 ||
	    dsp_cmd((unsigned char)((len - 1U) >> 8)) < 0)
		return -EIO;
	return 0;
}

static int sb_start(unsigned int len)
{
	int ret;

	clr_irq();
	sb_dma_done = 0;
	sb_dma_program_locked(len);
	sb_active = 1;
	sb_active_len = len;
	set_irq();

	ret = sb_dsp_start(len);
	if (ret < 0) {
		clr_irq();
		outb(sb_dma_mask, DMA1_MASK_REG);
		sb_active = 0;
		sb_dma_done = 0;
		sb_play_underruns++;
		set_irq();
	}
	return ret;
}

static void sb_halt(void)
{
	(void)dsp_cmd(0xD0); /* halt DMA (8-bit) */
	clr_irq();
	outb(sb_dma_mask, DMA1_MASK_REG);
	sb_active = 0;
	sb_dma_done = 0;
	set_irq();
}

/*
 * CT1345/CT1745-compatible mixer access at base+4 / base+5. MAD16 exposes
 * the SB Pro packed left/right register map; maxing the wrong SB16-style
 * per-channel registers can leave output unexpectedly quiet on real cards.
 */
static void sb_mixer_write(unsigned char reg, unsigned char value)
{
	unsigned a = sb_base + 4u;
	unsigned d = sb_base + 5u;

	outb(reg, a);
	outb(value, d);
	inb(d);
}

#ifdef CONFIG_SB_STEREO
static unsigned char sb_mixer_read(unsigned char reg)
{
	unsigned a = sb_base + 4u;
	unsigned d = sb_base + 5u;

	outb(reg, a);
	return inb(d);
}

static void sb_mixer_stereo_apply(void)
{
	unsigned char v;

	if (sb_dsp_ver_major != 0 && sb_dsp_ver_major < 3)
		return;
	v = sb_mixer_read(0x0e);
	if (sb_channels == 2)
		v |= 0x02;
	else
		v &= ~0x02;
	sb_mixer_write(0x0e, v);
}
#endif

static unsigned char sb_mixer_lr_byte(unsigned left, unsigned right)
{
	if (sb_dsp_ver_major >= 4) {
		unsigned vl = (left * 15U + 50U) / 100U;
		unsigned vr = (right * 15U + 50U) / 100U;

		return (unsigned char)((vl << 4) | vr);
	} else {
		unsigned vl = (left * 7U + 50U) / 100U;
		unsigned vr = (right * 7U + 50U) / 100U;

		return (unsigned char)((vl << 5) | (vr << 1));
	}
}

static void sb_mixer_voice_apply(void)
{
	if (sb_dsp_ver_major != 0 && sb_dsp_ver_major < 3)
		return;
	sb_mixer_write(0x04, sb_mixer_lr_byte(sb_vol_l, sb_vol_r));
}

static void sb_mixer_init_default(void)
{
	sb_vol_l = sb_vol_r = SB_DEFAULT_PLAYVOL;
	if (sb_dsp_ver_major != 0 && sb_dsp_ver_major < 3)
		return;
	sb_mixer_write(0x22, sb_mixer_lr_byte(100, 100));
	sb_mixer_voice_apply();
}

/*
 * SB Pro / OPTi MAD16 and many compatibles latch IRQ until the DSP read path
 * is serviced: if bit 7 of read-status (base+0x0E) is set, read data from
 * base+0x0A. Call after playback even when completion was detected via 8237
 * TC polling (IRQ never reached the CPU).
 */
static void sb_dsp_irq_ack(void)
{
	unsigned n = 0;

	while (n < 8U && (inb(sb_base + 0x0EU) & 0x80U) != 0U) {
		inb(sb_base + 0x0AU);
		n++;
	}
	inb(sb_base + 0x0FU); /* SB16 16-bit ack; harmless on SB Pro compat */
}

static void sb_interrupt(int irq, struct pt_regs *regs)
{
	(void)irq;
	(void)regs;
	sb_dsp_irq_ack();
	sb_dma_done = 1;
}

static int sb_transfer_done(void)
{
	if (sb_dma_done)
		return 1;
	if (sb_active && (inb(DMA1_STAT_REG) & sb_dma_tc_bit) != 0U) {
		sb_dma_done = 1;
		return 1;
	}
	return 0;
}

static int sb_wait_complete(unsigned int len)
{
	jiff_t deadline = (jiff_t)jiffies() + sb_play_ticks(len) + (HZ / 2) + 1;

	for (;;) {
		if (sb_transfer_done())
			break;
		if (current->signal) {
			sb_halt();
			return -EINTR;
		}
		if (time_after(jiffies(), deadline)) {
			sb_play_underruns++;
			sb_halt();
			return -EIO;
		}
		inb(0x61U);
	}

	clr_irq();
	outb(sb_dma_mask, DMA1_MASK_REG);
	sb_active = 0;
	sb_dma_done = 0;
	set_irq();
	sb_dsp_irq_ack();
	sb_bytes_played += (__u32)len;
	return 0;
}

static unsigned int sb_output_delay(void)
{
	unsigned int rem;

	clr_irq();
	if (sb_transfer_done()) {
		rem = 0;
	} else if (sb_active) {
		rem = sb_dma_count_locked();
		if (rem > sb_active_len)
			rem = sb_active_len;
	} else {
		rem = 0;
	}
	set_irq();
	return rem;
}

/* Returns 0, or negative errno. */
static int sb_play_chunk(char *buf, unsigned int len)
{
	int ret;

	if (!len || len > SB_BOUNCE)
		return 0;

	if (((sb_bounce_phys & 0xFFFFUL) + (unsigned long)len) > 0x10000UL)
		return -EIO;
	if (verified_memcpy_fromfs(sb_bounce, buf, len) != 0)
		return -EFAULT;

	ret = sb_start(len);
	if (ret < 0)
		return ret;
	return sb_wait_complete(len);
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
	sb_active = 0;
	sb_dma_done = 0;
#ifdef CONFIG_SB_STEREO
	sb_channels = 1;
	sb_rate_cache(sb_rate);
	sb_mixer_stereo_apply();
#endif
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
	int ret;

	(void)inode;

	if (!(file->f_mode & FMODE_WRITE))
		return -EINVAL;

	while (total < count) {
		unsigned int chunk = (unsigned int)(count - total);

		if (chunk > SB_BOUNCE)
			chunk = SB_BOUNCE;
		ret = 0;
		if (sb_trig & PCM_ENABLE_OUTPUT)
			ret = sb_play_chunk(buf + total, chunk);
		if (ret < 0)
			return (size_t)ret;
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
#ifdef CONFIG_SB_STEREO
		if (v == 0) {
			v = sb_channels;
			return sb_put_arg32(arg, v);
		}
		if (v != 1 && v != 2)
			return -EINVAL;
		if (v == 2 && sb_dsp_ver_major != 0 && sb_dsp_ver_major < 3)
			return -EINVAL;
		if ((unsigned char)v != sb_channels) {
			sb_channels = (unsigned char)v;
			sb_rate_cache(sb_rate);
			sb_mixer_stereo_apply();
		}
		return sb_put_arg32(arg, v);
#else
		if (v == 0) {
			v = 1;
			return sb_put_arg32(arg, v);
		}
		if (v != 1)
			return -EINVAL;
		v = 1;
		return sb_put_arg32(arg, v);
#endif
	case SNDCTL_DSP_STEREO:
		ret = sb_get_arg32(arg, &v);
		if (ret < 0)
			return ret;
#ifdef CONFIG_SB_STEREO
		if (v != 0 && v != 1)
			return -EINVAL;
		if (v != 0 && sb_dsp_ver_major != 0 && sb_dsp_ver_major < 3)
			return -EINVAL;
		sb_channels = (v != 0) ? 2 : 1;
		sb_rate_cache(sb_rate);
		sb_mixer_stereo_apply();
		v = (sb_channels == 2) ? 1 : 0;
		return sb_put_arg32(arg, v);
#else
		if (v != 0)
			return -EINVAL;
		return 0;
#endif
	case SNDCTL_DSP_GETBLKSIZE:
		v = (oss_int32_t)SB_BOUNCE;
		return sb_put_arg32(arg, v);
	case SNDCTL_DSP_GETFMTS:
		v = (oss_int32_t)AFMT_U8;
		return sb_put_arg32(arg, v);
	case SNDCTL_DSP_GETOSPACE:
		v = (oss_int32_t)(SB_BOUNCE - sb_output_delay());
		abi.fragments = (oss_int32_t)(v / SB_BOUNCE);
		abi.fragstotal = 1;
		abi.fragsize = (oss_int32_t)SB_BOUNCE;
		abi.bytes = v;
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
		v = (oss_int32_t)sb_output_delay();
		return sb_put_arg32(arg, v);
	case SNDCTL_DSP_GETPLAYVOL: /* Same number as SETPLAYVOL on 16-bit int */
		if (!arg)
			return -EINVAL;
		/*
		 * 16-bit ELKS collapses GETPLAYVOL and SETPLAYVOL to the same
		 * command number. Use -1 as a local query-without-changing sentinel.
		 */
		if (verified_memcpy_fromfs(&v, arg, sizeof(v)) == 0) {
			if (v != (oss_int32_t)-1) {
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
#ifdef CONFIG_SB_STEREO
		v = (sb_channels == 2) ? 3 : 1;
#else
		v = 1; /* mono (front-left only) */
#endif
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
	if (sb_cfg_disable)
		return;

	if (sb_cfg_from_bootopts) {
		sb_base = sb_cfg_port;
		sb_dma = sb_cfg_dma;
		sb_irq_line = sb_cfg_irq;
	} else {
		sb_base = (unsigned int)CONFIG_SB_PORT;
		sb_dma = (unsigned char)CONFIG_SB_DMA;
		sb_irq_line = (unsigned char)CONFIG_SB_IRQ;
	}

	if (sb_dma != 1 && sb_dma != 3) {
		printk("sb: bad dma %u\n", sb_dma);
		return;
	}
	sb_dma_cache();
	sb_rate_cache(sb_rate);
	sb_bounce_phys = SB_LINADDR(kernel_ds, (word_t)_FP_OFF(sb_bounce));
	if ((sb_bounce_phys & 0xFFFFUL) + (unsigned long)SB_BOUNCE > 0x10000UL) {
		printk("sb: dma buffer crosses 64k\n");
		return;
	}
#ifdef CONFIG_SB_OPTI82C929
	opti82c929_early_init(sb_base, sb_irq_line, sb_dma);
#endif
	if (sb_reset(sb_base) < 0) {
		printk("sb: not at 0x%x\n", sb_base);
		return;
	}
	(void)sb_read_dsp_version();
	if (request_irq((int)sb_irq_line, sb_interrupt, INT_GENERIC) != 0) {
		printk("sb: irq %u busy\n", sb_irq_line);
		return;
	}
	if (dsp_cmd(0xD1) < 0) {
		printk("sb: speaker enable timeout\n");
		free_irq((int)sb_irq_line);
		return;
	}
	sb_mixer_init_default();
#ifdef CONFIG_SB_STEREO
	sb_mixer_stereo_apply();
#endif
	sb_present = 1;

	if (register_chrdev(DSP_MAJOR, "dsp", &sb_dsp_fops) != 0) {
		printk("sb: register_chrdev failed\n");
		free_irq((int)sb_irq_line);
		sb_present = 0;
		return;
	}
}

#endif /* CONFIG_CHAR_DEV_DSP */
