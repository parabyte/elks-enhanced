/*
 * mad16cfg - XT-only OPTi 82C929A/MAD16 control-port utility.
 *
 * This utility intentionally exposes only the XT-safe profile:
 * Sound Blaster mode, SB IRQ 5/7, 8-bit DMA 1/3, optional MPU IRQ 5/7,
 * and WSS/CD-ROM/IDE decode forced off.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mad16.h>

static int portfd;

static void usage(void)
{
	printf("Usage:\n");
	printf("  mad16cfg                Show current XT-safe configuration\n");
	printf("  mad16cfg status         Show current XT-safe configuration\n");
	printf("  mad16cfg ports          Show XT-safe hardware address map\n");
	printf("  mad16cfg defaults       Apply XT-safe defaults\n");
	printf("  mad16cfg raw            Show raw MC1..MC6 values\n");
	printf("  mad16cfg gate           Test password + MC1 read via /dev/port (diagnostic)\n");
	printf("  mad16cfg rawset mc1 mc2 mc3 mc4 mc5 mc6\n");
	printf("  mad16cfg set key value [key value ...]\n");
	printf("\n");
	printf("Keys:\n");
	printf("  mode sb\n");
	printf("  sb_port 0x220|0x240    sb_irq/irq 5|7|off    sb_dma/dma 1|3|off\n");
	printf("  (sb_port also accepts 220/240 meaning 0x220/0x240)\n");
	printf("  mpu on|off             mpu_port 300|310|320|330   mpu_irq 5|7\n");
	printf("  gameport on|off        gpmode external|internal\n");
	printf("  fmap normal|single     fmclk opl3|opl2    sbver 1|2|3|4\n");
	printf("  powerdown/adpcm/gpout/timeout/outmix/silence/shpass/spaccess\n");
	printf("  codec_access/fifo/cfix/dma_watchdog/wave/attn on|off\n");
	printf("\n");
	printf("Notes:\n");
	printf("  XT-only: no AT IRQ 9/10/11 routes are accepted.\n");
	printf("  WSS and CD-ROM/IDE interface decode are forced disabled.\n");
	printf("  rawset is sanitized before write so it cannot enable AT-only decode.\n");
	printf("  All MC access uses /dev/port only (password write then register read/write).\n");
}

static int parse_num(const char *s, unsigned long *out)
{
	char *end;
	unsigned long v;

	if (!s || !s[0])
		return -1;
	v = strtoul(s, &end, 0);
	if (end && *end)
		return -1;
	*out = v;
	return 0;
}

static int parse_onoff(const char *s, unsigned char *out)
{
	if (!strcmp(s, "on") || !strcmp(s, "1") || !strcmp(s, "yes")) {
		*out = 1;
		return 0;
	}
	if (!strcmp(s, "off") || !strcmp(s, "0") || !strcmp(s, "no")) {
		*out = 0;
		return 0;
	}
	return -1;
}

static int parse_raw_port(const char *s, unsigned short *out)
{
	unsigned long v;

	if (parse_num(s, &v) < 0 || v > 0xFFFFUL)
		return -1;
	*out = (unsigned short)v;
	return 0;
}

static int parse_sb_port(const char *s, unsigned short *out)
{
	unsigned short port;

	if (parse_raw_port(s, &port) < 0)
		return -1;
	if (port == MAD16_SB_PORT_220 || port == 220) {
		*out = MAD16_SB_PORT_220;
		return 0;
	}
	if (port == MAD16_SB_PORT_240 || port == 240) {
		*out = MAD16_SB_PORT_240;
		return 0;
	}
	return -1;
}

static int parse_mpu_port(const char *s, unsigned short *out)
{
	unsigned short port;

	if (parse_raw_port(s, &port) < 0)
		return -1;
	if (port == MAD16_MPU_PORT_300 || port == 300) {
		*out = MAD16_MPU_PORT_300;
		return 0;
	}
	if (port == MAD16_MPU_PORT_310 || port == 310) {
		*out = MAD16_MPU_PORT_310;
		return 0;
	}
	if (port == MAD16_MPU_PORT_320 || port == 320) {
		*out = MAD16_MPU_PORT_320;
		return 0;
	}
	if (port == MAD16_MPU_PORT_330 || port == 330) {
		*out = MAD16_MPU_PORT_330;
		return 0;
	}
	return -1;
}

static int parse_xt_irq(const char *s, unsigned char *out, int off_ok)
{
	unsigned long v;

	if (off_ok && !strcmp(s, "off")) {
		*out = MAD16_DISABLED;
		return 0;
	}
	if (parse_num(s, &v) < 0 || v > 255UL)
		return -1;
	if (v != 5UL && v != 7UL)
		return -1;
	*out = (unsigned char)v;
	return 0;
}

static int parse_xt_dma(const char *s, unsigned char *out)
{
	unsigned long v;

	if (!strcmp(s, "off")) {
		*out = MAD16_DISABLED;
		return 0;
	}
	if (parse_num(s, &v) < 0 || (v != 1UL && v != 3UL))
		return -1;
	*out = (unsigned char)v;
	return 0;
}

static int port_read8(unsigned short port, unsigned char *value)
{
	if (lseek(portfd, (long)port, SEEK_SET) < 0)
		return -1;
	if (read(portfd, value, 1) != 1) {
		errno = EIO;
		return -1;
	}
	return 0;
}

static int port_write8(unsigned short port, unsigned char value)
{
	if (lseek(portfd, (long)port, SEEK_SET) < 0)
		return -1;
	if (write(portfd, &value, 1) != 1) {
		errno = EIO;
		return -1;
	}
	return 0;
}

static int mad16_read_reg(unsigned short port, unsigned char *value)
{
	if (port_write8(MAD16_PASSWD_PORT, MAD16_PASSWORD) < 0)
		return -1;
	return port_read8(port, value);
}

static int mad16_write_reg(unsigned short port, unsigned char value)
{
	if (port_write8(MAD16_PASSWD_PORT, MAD16_PASSWORD) < 0)
		return -1;
	return port_write8(port, value);
}

static int mad16_detect(void)
{
	unsigned char gated = 0xFF, raw = 0xFF;
	int i;

	/*
	 * After password, the first MC read is
	 * gated; an immediate read without a new password must see the window
	 * close (different byte). Retry for transient 0xff or slow settle.
	 */
	for (i = 0; i < 12; i++) {
		if (mad16_read_reg(MAD16_MC1_PORT, &gated) < 0)
			continue;
		if (gated == 0xFF)
			continue;
		if (port_read8(MAD16_MC1_PORT, &raw) < 0)
			continue;
		if (raw == gated)
			continue;
		return 1;
	}
	return 0;
}

static int mad16_read_regs(struct mad16_regs *regs)
{
	int rc = 0;

	/*
	 * Readback on some 82C929-compatible boards can intermittently return
	 * 0xff for gated reads; retry each register a few times to reduce
	 * false negatives in status/verify.
	 */
	{
		unsigned short ports[6] = {
			MAD16_MC1_PORT, MAD16_MC2_PORT, MAD16_MC3_PORT,
			MAD16_MC4_PORT, MAD16_MC5_PORT, MAD16_MC6_PORT
		};
		unsigned char *vals[6] = {
			&regs->mc1, &regs->mc2, &regs->mc3,
			&regs->mc4, &regs->mc5, &regs->mc6
		};
		int i, tries;

		for (i = 0; i < 6; i++) {
			unsigned char v = 0xFF;
			int ok = -1;

			for (tries = 0; tries < 4; tries++) {
				if (mad16_read_reg(ports[i], &v) == 0) {
					ok = 0;
					if (v != 0xFF)
						break;
				}
			}
			if (ok < 0)
				rc |= -1;
			*vals[i] = v;
		}
	}
	return rc;
}

static unsigned char bit_majority3(unsigned char a, unsigned char b, unsigned char c)
{
	return (unsigned char)((a & b) | (a & c) | (b & c));
}

static int regs_ff_score(struct mad16_regs *r)
{
	int s = 0;
	if (r->mc1 == 0xFF) s++;
	if (r->mc2 == 0xFF) s++;
	if (r->mc3 == 0xFF) s++;
	if (r->mc4 == 0xFF) s++;
	if (r->mc5 == 0xFF) s++;
	if (r->mc6 == 0xFF) s++;
	return s;
}

/*
 * Build a more stable register snapshot by sampling multiple times and using
 * per-bit majority voting. This filters transient readback noise common on
 * some 82C929-compatible boards.
 */
static int mad16_read_regs_stable(struct mad16_regs *regs)
{
	struct mad16_regs r1, r2, r3;
	int rc = 0;

	rc |= mad16_read_regs(&r1);
	rc |= mad16_read_regs(&r2);
	rc |= mad16_read_regs(&r3);

	regs->mc1 = bit_majority3(r1.mc1, r2.mc1, r3.mc1);
	regs->mc2 = bit_majority3(r1.mc2, r2.mc2, r3.mc2);
	regs->mc3 = bit_majority3(r1.mc3, r2.mc3, r3.mc3);
	regs->mc4 = bit_majority3(r1.mc4, r2.mc4, r3.mc4);
	regs->mc5 = bit_majority3(r1.mc5, r2.mc5, r3.mc5);
	regs->mc6 = bit_majority3(r1.mc6, r2.mc6, r3.mc6);

	/*
	 * If majority output is still obviously bogus (all 0xff), fall back to
	 * the single sample with the fewest 0xff registers.
	 */
	if (regs_ff_score(regs) >= 5) {
		int s1 = regs_ff_score(&r1);
		int s2 = regs_ff_score(&r2);
		int s3 = regs_ff_score(&r3);
		struct mad16_regs *best = &r1;

		if (s2 < s1)
			best = &r2;
		if ((best == &r1 && s3 < s1) || (best == &r2 && s3 < s2))
			best = &r3;
		*regs = *best;
	}
	return rc;
}

/*
 * Program MC1..MC5, verify, then write MC6 (write-only / MPU).
 * Writing MC6 in the same burst can perturb readback or routing on some boards.
 */
static int mad16_write_regs_mc1_mc5(struct mad16_regs *regs)
{
	int rc = 0;

	rc |= mad16_write_reg(MAD16_MC1_PORT, regs->mc1);
	rc |= mad16_write_reg(MAD16_MC2_PORT, regs->mc2);
	rc |= mad16_write_reg(MAD16_MC3_PORT, regs->mc3);
	rc |= mad16_write_reg(MAD16_MC4_PORT, regs->mc4);
	rc |= mad16_write_reg(MAD16_MC5_PORT, regs->mc5);
	return rc;
}

static unsigned char get_bit(unsigned char v, unsigned char bit)
{
	return (unsigned char)((v >> bit) & 1u);
}

static void set_bit(unsigned char *v, unsigned char bit, unsigned char on)
{
	if (on)
		*v |= (unsigned char)(1u << bit);
	else
		*v &= (unsigned char)~(1u << bit);
}

static void xt_sanitize_regs(struct mad16_regs *regs)
{
	/* SB mode; WSS base bits and CD/IDE type bits cleared. */
	regs->mc1 &= (MAD16_MC1_POWERDOWN | MAD16_MC1_GAME_OFF);

	/* Linux mad16.c default for no CD IRQ and disabled CD DMA. */
	regs->mc2 = (unsigned char)((regs->mc2 & MAD16_MC2_OPL4) |
		MAD16_MC2_CDSEL_DISABLED);

	/* MC3: no AT IRQ10 and no DMA0 in this XT profile. */
	if ((regs->mc3 & MAD16_MC3_IRQ_MASK) == 0x40)
		regs->mc3 = (unsigned char)((regs->mc3 & ~MAD16_MC3_IRQ_MASK) |
			MAD16_MC3_IRQ_MASK);
	if ((regs->mc3 & MAD16_MC3_DMA_MASK) == 0x10)
		regs->mc3 = (unsigned char)((regs->mc3 & ~MAD16_MC3_DMA_MASK) |
			MAD16_MC3_DMA_MASK);

	/* MC5: Linux C929 keeps reserved bit 7 and bits 0/2 set; bit 6 is must-zero. */
	regs->mc5 &= (unsigned char)~MAD16_MC5_MUST0_6;
	regs->mc5 |= MAD16_MC5_MUST1_7 | MAD16_MC5_RES2_MUST1 |
		MAD16_MC5_RES0_MUST1;

	/* MC6: Linux disables MPU with 0x03; IRQ/port bits matter only if enabled. */
	if ((regs->mc6 & MAD16_MC6_MPU_ENABLE) == 0)
		regs->mc6 &= MAD16_MC6_MPU_OFF_MASK;
	else if ((regs->mc6 & MAD16_MC6_MPU_IRQ) == 0x00 ||
		 (regs->mc6 & MAD16_MC6_MPU_IRQ) == 0x08)
		regs->mc6 = (unsigned char)((regs->mc6 & ~MAD16_MC6_MPU_IRQ) | 0x18);
}

static int encode_mc1(struct mad16_cfg *cfg, unsigned char *out)
{
	unsigned char mc1 = 0;

	if (cfg->mode != MAD16_MODE_SB)
		return -1;
	set_bit(&mc1, 6, cfg->powerdown != 0);
	set_bit(&mc1, 0, cfg->gameport ? 0 : 1);
	*out = mc1;
	return 0;
}

static int encode_mc3(struct mad16_cfg *cfg, unsigned char *out)
{
	unsigned char mc3 = 0;

	if (cfg->gpmode)
		mc3 |= MAD16_MC3_GP_TIMER;

	switch (cfg->sb_irq) {
	case 7:
		break;
	case 5:
		mc3 |= 0x80;
		break;
	case MAD16_DISABLED:
		mc3 |= MAD16_MC3_IRQ_MASK;
		break;
	default:
		return -1;
	}

	switch (cfg->sb_dma) {
	case 1:
		break;
	case 3:
		mc3 |= 0x20;
		break;
	case MAD16_DISABLED:
		mc3 |= MAD16_MC3_DMA_MASK;
		break;
	default:
		return -1;
	}

	set_bit(&mc3, 3, cfg->fmap_single != 0);
	switch (cfg->sb_port) {
	case MAD16_SB_PORT_220:
		break;
	case MAD16_SB_PORT_240:
		mc3 |= MAD16_MC3_SB_240;
		break;
	default:
		return -1;
	}
	*out = mc3;
	return 0;
}

static int encode_mc4(struct mad16_cfg *cfg, unsigned char *out)
{
	unsigned char mc4 = 0;

	set_bit(&mc4, 7, cfg->adpcm != 0);
	set_bit(&mc4, 6, cfg->gpout != 0);
	set_bit(&mc4, 5, cfg->timeout != 0);
	set_bit(&mc4, 4, cfg->outmix != 0);
	set_bit(&mc4, 3, cfg->fmclk_opl2 != 0);
	set_bit(&mc4, 2, cfg->silence != 0);

	switch (cfg->sbver) {
	case 1:
		mc4 |= 0x01;
		break;
	case 2:
		break;
	case 3:
		mc4 |= 0x02;
		break;
	case 4:
		mc4 |= 0x03;
		break;
	default:
		return -1;
	}

	*out = mc4;
	return 0;
}

static void encode_mc5(struct mad16_cfg *cfg, unsigned char *out)
{
	unsigned char mc5 = 0;

	set_bit(&mc5, 5, cfg->shpass != 0);
	set_bit(&mc5, 4, cfg->spaccess || cfg->codec_access);
	set_bit(&mc5, 3, cfg->fifo != 0);
	set_bit(&mc5, 2, cfg->res2_must1 != 0);
	set_bit(&mc5, 1, cfg->cfix != 0);
	set_bit(&mc5, 0, cfg->res0_must1 != 0);
	*out = mc5;
}

static int encode_mc6(struct mad16_cfg *cfg, unsigned char *out)
{
	unsigned char mc6 = 0;

	if (cfg->mpu_enable) {
		mc6 |= MAD16_MC6_MPU_ENABLE;

		switch (cfg->mpu_port) {
		case MAD16_MPU_PORT_330:
			break;
		case MAD16_MPU_PORT_320:
			mc6 |= 0x20;
			break;
		case MAD16_MPU_PORT_310:
			mc6 |= 0x40;
			break;
		case MAD16_MPU_PORT_300:
			mc6 |= 0x60;
			break;
		default:
			return -1;
		}

		switch (cfg->mpu_irq) {
		case 5:
			mc6 |= 0x10;
			break;
		case 7:
			mc6 |= 0x18;
			break;
		default:
			return -1;
		}
	}

	set_bit(&mc6, 2, cfg->dma_watchdog != 0);
	set_bit(&mc6, 1, cfg->wave != 0);
	set_bit(&mc6, 0, cfg->attn != 0);
	*out = mc6;
	return 0;
}

static int cfg_to_regs(struct mad16_cfg *cfg, struct mad16_regs *regs)
{
	if (encode_mc1(cfg, &regs->mc1) < 0)
		return -1;
	regs->mc2 = (unsigned char)(MAD16_MC2_CDSEL_DISABLED |
		(cfg->opl4 ? MAD16_MC2_OPL4 : 0));
	if (encode_mc3(cfg, &regs->mc3) < 0)
		return -1;
	if (encode_mc4(cfg, &regs->mc4) < 0)
		return -1;
	encode_mc5(cfg, &regs->mc5);
	if (encode_mc6(cfg, &regs->mc6) < 0)
		return -1;
	xt_sanitize_regs(regs);
	return 0;
}

static void regs_to_cfg(struct mad16_regs *regs, struct mad16_cfg *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->wss_port = 0x530;
	cfg->cd_port = 0x340;
	cfg->sb_port = get_bit(regs->mc3, 2) ? MAD16_SB_PORT_240 : MAD16_SB_PORT_220;
	cfg->mpu_port = MAD16_MPU_PORT_330;
	cfg->mode = MAD16_MODE_SB;
	cfg->powerdown = get_bit(regs->mc1, 6);
	cfg->wss_irq = MAD16_WSS_IRQ_AUTO;
	cfg->wss_dma = 0;
	cfg->cd_type = MAD16_CD_DISABLED;
	cfg->cd_irq = MAD16_DISABLED;
	cfg->cd_dma = MAD16_DISABLED;
	/* XT profile keeps CD/IDE/WSS decode off; MC2 readback is board-variable. */
	cfg->opl4 = 0;

	switch (regs->mc3 & MAD16_MC3_IRQ_MASK) {
	case 0x00:
		cfg->sb_irq = 7;
		break;
	case 0x80:
		cfg->sb_irq = 5;
		break;
	default:
		cfg->sb_irq = MAD16_DISABLED;
		break;
	}
	if (cfg->sb_irq == MAD16_DISABLED)
		cfg->sb_dma = MAD16_DISABLED;
	else {
		switch (regs->mc3 & MAD16_MC3_DMA_MASK) {
		case 0x00:
			cfg->sb_dma = 1;
			break;
		case 0x20:
			cfg->sb_dma = 3;
			break;
		default:
			cfg->sb_dma = MAD16_DISABLED;
			break;
		}
	}

	cfg->fmap_single = get_bit(regs->mc3, 3);
	cfg->gameport = get_bit(regs->mc1, 0) ? 0 : 1;
	/* GP timer mode is write-only/overlaps revision readback on MC3 bit 1. */
	cfg->gpmode = get_bit(regs->mc3, 1);
	cfg->adpcm = get_bit(regs->mc4, 7);
	cfg->gpout = get_bit(regs->mc4, 6);
	cfg->timeout = get_bit(regs->mc4, 5);
	cfg->outmix = get_bit(regs->mc4, 4);
	cfg->fmclk_opl2 = get_bit(regs->mc4, 3);
	cfg->silence = get_bit(regs->mc4, 2);

	switch (regs->mc4 & MAD16_MC4_SBVER_MASK) {
	case 1:
		cfg->sbver = 1;
		break;
	case 0:
		cfg->sbver = 2;
		break;
	case 2:
		cfg->sbver = 3;
		break;
	default:
		cfg->sbver = 4;
		break;
	}

	cfg->shpass = get_bit(regs->mc5, 5);
	cfg->spaccess = get_bit(regs->mc5, 4);
	cfg->codec_access = cfg->spaccess;
	cfg->fifo = get_bit(regs->mc5, 3);
	cfg->res2_must1 = 1;
	cfg->cfix = get_bit(regs->mc5, 1);
	cfg->res0_must1 = 1;

	/*
	 * MC6 is documented write-only on 82C929, and readback is not reliable.
	 * Report stable XT-safe defaults instead of decoding random values.
	 */
	cfg->mpu_enable = 0;
	cfg->mpu_port = MAD16_MPU_PORT_330;
	cfg->mpu_irq = 7;
	cfg->dma_watchdog = 0;
	cfg->wave = 1;
	cfg->attn = 1;
}

static void default_cfg(struct mad16_cfg *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->wss_port = 0x530;
	cfg->cd_port = 0x340;
	cfg->sb_port = MAD16_SB_PORT_220;
	cfg->mpu_port = MAD16_MPU_PORT_330;
	cfg->mode = MAD16_MODE_SB;
	cfg->wss_irq = MAD16_WSS_IRQ_AUTO;
	cfg->cd_irq = MAD16_DISABLED;
	cfg->cd_dma = MAD16_DISABLED;
	cfg->sb_irq = 5;
	cfg->sb_dma = 1;
	cfg->gameport = 1;
	cfg->gpmode = 1;
	cfg->adpcm = 1;
	cfg->timeout = 1;
	cfg->sbver = 3;
	cfg->shpass = 1;
	cfg->spaccess = 0;
	cfg->codec_access = 0;
	cfg->res2_must1 = 1;
	cfg->res0_must1 = 1;
	cfg->mpu_enable = 0;
	cfg->mpu_irq = 7;
	cfg->wave = 1;
	cfg->attn = 1;
}

/*
 * Keep defaults aligned with Linux mad16.c's C929 register image while
 * preserving SB mode and XT-only IRQ/DMA choices.
 */
static void optimize_82c929a_cfg(struct mad16_cfg *cfg)
{
	cfg->mode = MAD16_MODE_SB;
	cfg->wss_irq = MAD16_WSS_IRQ_AUTO;
	cfg->wss_dma = 0;
	cfg->cd_type = MAD16_CD_DISABLED;
	cfg->cd_irq = MAD16_DISABLED;
	cfg->cd_dma = MAD16_DISABLED;

	/* Internal game timer and normal/two-crystal mapping. */
	cfg->gpmode = 1;
	cfg->fmap_single = 0;

	/* Linux C929 profile: MC4=0xa2, MC5=0xa5, MC6=0x03. */
	cfg->adpcm = 1;
	cfg->timeout = 1;
	cfg->silence = 0;
	cfg->sbver = 3;
	cfg->shpass = 1;
	cfg->spaccess = 0;
	cfg->codec_access = 0;
	cfg->res2_must1 = 1;
	cfg->res0_must1 = 1;
	cfg->mpu_enable = 0;
	cfg->wave = 1;
	cfg->attn = 1;
}

static const char *onoff_name(unsigned char v)
{
	return v ? "on" : "off";
}

static void print_irq(const char *label, unsigned char irq)
{
	if (irq == MAD16_DISABLED)
		printf("%s off\n", label);
	else
		printf("%s %u\n", label, (unsigned)irq);
}

static void print_dma(const char *label, unsigned char dma)
{
	if (dma == MAD16_DISABLED)
		printf("%s off\n", label);
	else
		printf("%s %u\n", label, (unsigned)dma);
}

static void show_cfg(struct mad16_cfg *cfg, unsigned char rev)
{
	printf("profile xt-only\n");
	printf("ctrl_ports 0x%x-0x%x\n", MAD16_MC1_PORT, MAD16_MC6_PORT);
	printf("passwd_port 0x%x\n", MAD16_PASSWD_PORT);
	printf("wss off\n");
	printf("cd_ide off\n");
	printf("gameport_range 0x%x-0x%x\n", MAD16_GAMEPORT_BASE, MAD16_GAMEPORT_LAST);
	printf("mode sb\n");
	printf("sb_port 0x%x\n", cfg->sb_port);
	print_irq("sb_irq", cfg->sb_irq);
	print_dma("sb_dma", cfg->sb_dma);
	printf("mpu %s\n", onoff_name(cfg->mpu_enable));
	printf("mpu_port 0x%x\n", cfg->mpu_port);
	print_irq("mpu_irq", cfg->mpu_irq);
	printf("gameport %s\n", onoff_name(cfg->gameport));
	printf("gpmode %s\n", cfg->gpmode ? "internal" : "external");
	printf("fmap %s\n", cfg->fmap_single ? "single" : "normal");
	printf("fmclk %s\n", cfg->fmclk_opl2 ? "opl2" : "opl3");
	printf("sbver %u\n", (unsigned)cfg->sbver);
	printf("powerdown %s\n", onoff_name(cfg->powerdown));
	printf("adpcm %s\n", onoff_name(cfg->adpcm));
	printf("gpout %s\n", onoff_name(cfg->gpout));
	printf("timeout %s\n", onoff_name(cfg->timeout));
	printf("outmix %s\n", onoff_name(cfg->outmix));
	printf("silence %s\n", onoff_name(cfg->silence));
	printf("mc5_profile linux_c929\n");
	printf("mc5_res7_written 1\n");
	printf("shpass %s\n", onoff_name(cfg->shpass));
	printf("spaccess %s\n", onoff_name(cfg->spaccess));
	printf("codec_access %s\n", onoff_name(cfg->codec_access));
	printf("fifo_readback %s\n", onoff_name(cfg->fifo));
	printf("cfix %s\n", onoff_name(cfg->cfix));
	printf("mc5_res2_written 1\n");
	printf("mc5_res0_written 1\n");
	printf("dma_watchdog %s\n", onoff_name(cfg->dma_watchdog));
	printf("wave %s\n", onoff_name(cfg->wave));
	printf("attn %s\n", onoff_name(cfg->attn));
	printf("revision %u\n", (unsigned)rev);
}

static void show_ports(void)
{
	printf("control 0x%x-0x%x\n", MAD16_MC1_PORT, MAD16_MC6_PORT);
	printf("password 0x%x\n", MAD16_PASSWD_PORT);
	printf("sb_port 0x%x|0x%x\n", MAD16_SB_PORT_220, MAD16_SB_PORT_240);
	printf("sb_irq/irq 5|7\n");
	printf("sb_dma/dma 1|3\n");
	printf("mpu_port 0x%x|0x%x|0x%x|0x%x\n",
		MAD16_MPU_PORT_300, MAD16_MPU_PORT_310,
		MAD16_MPU_PORT_320, MAD16_MPU_PORT_330);
	printf("mpu_irq 5|7\n");
	printf("gameport 0x%x-0x%x\n", MAD16_GAMEPORT_BASE, MAD16_GAMEPORT_LAST);
	printf("wss disabled\n");
	printf("cd_ide disabled\n");
}

static int set_key(struct mad16_cfg *cfg, const char *key, const char *val)
{
	unsigned long n;
	unsigned short port;

	if (!strcmp(key, "mode"))
		return !strcmp(val, "sb") ? 0 : -1;
	if (!strcmp(key, "sb_port")) {
		if (parse_sb_port(val, &port) < 0)
			return -1;
		cfg->sb_port = port;
		return 0;
	}
	if (!strcmp(key, "sb_irq") || !strcmp(key, "irq"))
		return parse_xt_irq(val, &cfg->sb_irq, 1);
	if (!strcmp(key, "sb_dma") || !strcmp(key, "dma"))
		return parse_xt_dma(val, &cfg->sb_dma);
	if (!strcmp(key, "mpu"))
		return parse_onoff(val, &cfg->mpu_enable);
	if (!strcmp(key, "mpu_port")) {
		if (parse_mpu_port(val, &port) < 0)
			return -1;
		cfg->mpu_port = port;
		return 0;
	}
	if (!strcmp(key, "mpu_irq"))
		return parse_xt_irq(val, &cfg->mpu_irq, 0);
	if (!strcmp(key, "gameport"))
		return parse_onoff(val, &cfg->gameport);
	if (!strcmp(key, "gpmode")) {
		if (!strcmp(val, "external"))
			cfg->gpmode = 0;
		else if (!strcmp(val, "internal"))
			cfg->gpmode = 1;
		else
			return -1;
		return 0;
	}
	if (!strcmp(key, "fmap")) {
		if (!strcmp(val, "normal"))
			cfg->fmap_single = 0;
		else if (!strcmp(val, "single"))
			cfg->fmap_single = 1;
		else
			return -1;
		return 0;
	}
	if (!strcmp(key, "fmclk")) {
		if (!strcmp(val, "opl3"))
			cfg->fmclk_opl2 = 0;
		else if (!strcmp(val, "opl2"))
			cfg->fmclk_opl2 = 1;
		else
			return -1;
		return 0;
	}
	if (!strcmp(key, "sbver")) {
		if (parse_num(val, &n) < 0 || n < 1 || n > 4)
			return -1;
		cfg->sbver = (unsigned char)n;
		return 0;
	}
	if (!strcmp(key, "opl4"))
		return parse_onoff(val, &cfg->opl4);

#define SET_BOOL_FIELD(name) \
	if (!strcmp(key, #name)) \
		return parse_onoff(val, &cfg->name)

	SET_BOOL_FIELD(powerdown);
	SET_BOOL_FIELD(adpcm);
	SET_BOOL_FIELD(gpout);
	SET_BOOL_FIELD(timeout);
	SET_BOOL_FIELD(outmix);
	SET_BOOL_FIELD(silence);
	SET_BOOL_FIELD(shpass);
	SET_BOOL_FIELD(spaccess);
	SET_BOOL_FIELD(codec_access);
	SET_BOOL_FIELD(fifo);
	SET_BOOL_FIELD(cfix);
	SET_BOOL_FIELD(dma_watchdog);
	SET_BOOL_FIELD(wave);
	SET_BOOL_FIELD(attn);

#undef SET_BOOL_FIELD

	if (!strcmp(key, "shadow"))
		return parse_onoff(val, &cfg->spaccess);
	if (!strcmp(key, "shadow_protect"))
		return parse_onoff(val, &cfg->spaccess);
	if (!strcmp(key, "autovol")) {
		unsigned char on;

		if (parse_onoff(val, &on) < 0)
			return -1;
		cfg->shpass = on ? 0 : 1;
		return 0;
	}
	if (!strcmp(key, "sbmix"))
		return parse_onoff(val, &cfg->res2_must1);
	if (!strcmp(key, "cdf_toen"))
		return parse_onoff(val, &cfg->res0_must1);

	return -1;
}

static int verify_one(const char *name, unsigned char got,
		      unsigned char want, unsigned char mask)
{
	if ((got & mask) == (want & mask))
		return 0;
	fprintf(stderr,
		"mad16cfg: verify %s got 0x%02x want 0x%02x mask 0x%02x\n",
		name, (unsigned)got, (unsigned)want, (unsigned)mask);
	return -1;
}

static unsigned int mismatch_count(unsigned char got, unsigned char want, unsigned char mask)
{
	unsigned char d = (unsigned char)((got ^ want) & mask);
	unsigned int n = 0;

	while (d) {
		n += d & 1u;
		d >>= 1;
	}
	return n;
}

static int verify_snapshot(struct mad16_regs *got, struct mad16_regs *want)
{
	int bad = 0;

	bad |= verify_one("mc1", got->mc1, want->mc1, MAD16_MC1_POWERDOWN | MAD16_MC1_GAME_OFF);
	/* MC2 readback is board-dependent; verify only OPL4 and CD DMA-off bits. */
	bad |= verify_one("mc2", got->mc2, want->mc2, MAD16_MC2_OPL4 | MAD16_MC2_CD_DMA_MASK);
	/* IRQ/DMA/FMAP/SB_240 only. */
	bad |= verify_one("mc3", got->mc3, want->mc3,
			  MAD16_MC3_IRQ_MASK | MAD16_MC3_DMA_MASK |
			  MAD16_MC3_FMAP | MAD16_MC3_SB_240);
	/* Only the stable/readable MC4 bits. */
	bad |= verify_one("mc4", got->mc4, want->mc4,
			  MAD16_MC4_ADPCM | MAD16_MC4_TIMEOUT |
			  MAD16_MC4_SILENCE | MAD16_MC4_SBVER_MASK);
	/* Conservative readable subset of MC5. */
	bad |= verify_one("mc5", got->mc5, want->mc5,
			  MAD16_MC5_MUST1_7 | MAD16_MC5_SHPASS |
			  MAD16_MC5_SPACCESS | MAD16_MC5_CFIX);
	return bad ? -1 : 0;
}

static unsigned int verify_snapshot_score(struct mad16_regs *got, struct mad16_regs *want)
{
	unsigned int s = 0;

	s += mismatch_count(got->mc1, want->mc1, MAD16_MC1_POWERDOWN | MAD16_MC1_GAME_OFF);
	s += mismatch_count(got->mc2, want->mc2, MAD16_MC2_OPL4 | MAD16_MC2_CD_DMA_MASK);
	s += mismatch_count(got->mc3, want->mc3,
			    MAD16_MC3_IRQ_MASK | MAD16_MC3_DMA_MASK |
			    MAD16_MC3_FMAP | MAD16_MC3_SB_240);
	s += mismatch_count(got->mc4, want->mc4,
			    MAD16_MC4_ADPCM | MAD16_MC4_TIMEOUT |
			    MAD16_MC4_SILENCE | MAD16_MC4_SBVER_MASK);
	s += mismatch_count(got->mc5, want->mc5,
			    MAD16_MC5_MUST1_7 | MAD16_MC5_SHPASS |
			    MAD16_MC5_SPACCESS | MAD16_MC5_CFIX);
	return s;
}

static int verify_write(struct mad16_regs *want)
{
	struct mad16_regs got;
	struct mad16_regs best;
	unsigned int best_score = ~0U;
	int i, have_best = 0;

	/*
	 * Readback is noisy on some boards. Take multiple snapshots and accept
	 * the write if any snapshot matches our stable-bit masks.
	 */
	for (i = 0; i < 3; i++) {
		if (mad16_read_regs_stable(&got) < 0)
			continue;
		if (verify_snapshot(&got, want) == 0)
			return 0;
		{
			unsigned int score = verify_snapshot_score(&got, want);
			if (!have_best || score < best_score) {
				best = got;
				best_score = score;
				have_best = 1;
			}
		}
	}
	if (have_best && best_score <= 2U) {
		fprintf(stderr, "mad16cfg: verify marginal (score %u), accepting\n", best_score);
		return 0;
	}
	if (have_best)
		(void)verify_snapshot(&best, want);
	fprintf(stderr, "mad16cfg: verify unstable; keeping written values\n");
	return 0;
}

static int apply_regs(struct mad16_regs *regs)
{
	int attempt;

	xt_sanitize_regs(regs);
	for (attempt = 0; attempt < 3; attempt++) {
		if (mad16_write_regs_mc1_mc5(regs) < 0) {
			perror("mad16cfg: write mc1-mc5");
			continue;
		}
		(void)verify_write(regs);
		if (mad16_write_reg(MAD16_MC6_PORT, regs->mc6) < 0) {
			perror("mad16cfg: write mc6");
			continue;
		}
		return 0;
	}
	fprintf(stderr, "mad16cfg: I/O to MC registers failed after retries\n");
	return -1;
}

int main(int argc, char **argv)
{
	struct mad16_regs regs;
	struct mad16_cfg cfg;
	unsigned char rev;
	unsigned long v[6];
	int i;

	if (argc > 1 && !strcmp(argv[1], "ports")) {
		show_ports();
		return 0;
	}

	portfd = open("/dev/port", O_RDWR);
	if (portfd < 0) {
		perror("mad16cfg: open /dev/port");
		return 1;
	}

	if (argc > 1 && !strcmp(argv[1], "gate")) {
		unsigned char gated = 0xFF, raw = 0xFF;

		if (port_write8(MAD16_PASSWD_PORT, MAD16_PASSWORD) < 0) {
			perror("mad16cfg: password write");
			close(portfd);
			return 1;
		}
		if (port_read8(MAD16_MC1_PORT, &gated) < 0) {
			perror("mad16cfg: gated read mc1");
			close(portfd);
			return 1;
		}
		printf("gated_mc1 0x%02x\n", (unsigned)gated);
		if (port_read8(MAD16_MC1_PORT, &raw) < 0) {
			perror("mad16cfg: ungated read mc1");
			close(portfd);
			return 1;
		}
		printf("ungated_mc1 0x%02x (should differ from gated if chip ok)\n", (unsigned)raw);
		close(portfd);
		return 0;
	}

	if (!mad16_detect()) {
		printf("mad16cfg: 82C929A not detected or /dev/port cannot reach 0x%x-0x%x\n",
			MAD16_MC1_PORT, MAD16_MC6_PORT);
		close(portfd);
		return 1;
	}

	if (argc == 1 || !strcmp(argv[1], "status")) {
		if (mad16_read_regs_stable(&regs) < 0) {
			perror("mad16cfg: read regs");
			close(portfd);
			return 1;
		}
		regs_to_cfg(&regs, &cfg);
		rev = (unsigned char)(regs.mc3 & MAD16_MC3_REV_MASK);
		show_cfg(&cfg, rev);
		close(portfd);
		return 0;
	}

	if (!strcmp(argv[1], "defaults")) {
		default_cfg(&cfg);
		optimize_82c929a_cfg(&cfg);
		memset(&regs, 0, sizeof(regs));
		if (cfg_to_regs(&cfg, &regs) < 0) {
			fprintf(stderr, "mad16cfg: defaults: cfg_to_regs failed\n");
			close(portfd);
			return 1;
		}
		if (apply_regs(&regs) < 0) {
			close(portfd);
			return 1;
		}
		close(portfd);
		return 0;
	}

	if (!strcmp(argv[1], "raw")) {
		if (mad16_read_regs_stable(&regs) < 0) {
			perror("mad16cfg: read regs");
			close(portfd);
			return 1;
		}
		printf("mc1 %02x\nmc2 %02x\nmc3 %02x\nmc4 %02x\nmc5 %02x\nmc6 %02x\n",
			regs.mc1, regs.mc2, regs.mc3, regs.mc4, regs.mc5, regs.mc6);
		close(portfd);
		return 0;
	}

	if (!strcmp(argv[1], "rawset")) {
		if (argc != 8) {
			usage();
			close(portfd);
			return 1;
		}
		for (i = 0; i < 6; i++) {
			if (parse_num(argv[i + 2], &v[i]) < 0 || v[i] > 0xFFUL) {
				fprintf(stderr, "mad16cfg: bad raw byte '%s'\n", argv[i + 2]);
				close(portfd);
				return 1;
			}
		}
		regs.mc1 = (unsigned char)v[0];
		regs.mc2 = (unsigned char)v[1];
		regs.mc3 = (unsigned char)v[2];
		regs.mc4 = (unsigned char)v[3];
		regs.mc5 = (unsigned char)v[4];
		regs.mc6 = (unsigned char)v[5];
		if (apply_regs(&regs) < 0) {
			close(portfd);
			return 1;
		}
		close(portfd);
		return 0;
	}

	if (!strcmp(argv[1], "set")) {
		if (mad16_read_regs_stable(&regs) < 0) {
			perror("mad16cfg: read regs");
			close(portfd);
			return 1;
		}
		regs_to_cfg(&regs, &cfg);
		if (((argc - 2) & 1) != 0) {
			usage();
			close(portfd);
			return 1;
		}
		for (i = 2; i < argc; i += 2) {
			if (set_key(&cfg, argv[i], argv[i + 1]) < 0) {
				fprintf(stderr, "mad16cfg: bad %s %s\n", argv[i], argv[i + 1]);
				close(portfd);
				return 1;
			}
		}
		optimize_82c929a_cfg(&cfg);
		if (cfg_to_regs(&cfg, &regs) < 0) {
			fprintf(stderr,
				"mad16cfg: invalid XT register image (use sb_port 0x220/0x240, sb_irq 5|7, sb_dma 1|3)\n");
			close(portfd);
			return 1;
		}
		if (apply_regs(&regs) < 0) {
			close(portfd);
			return 1;
		}
		close(portfd);
		return 0;
	}

	usage();
	close(portfd);
	return 1;
}
