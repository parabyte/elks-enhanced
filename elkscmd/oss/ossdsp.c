/*
 * Minimal PCM player using OSS ioctls on /dev/dsp.
 * Accepts raw unsigned 8-bit mono PCM or PCM WAV
 * (8-bit, mono, unsigned), including WAV files with extra RIFF chunks.
 *
 * Usage: ossdsp [path] [sample_rate_hz]
 * Raw PCM defaults to stdin at 8000 Hz. WAV uses its header rate.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>

#ifndef O_RDONLY
#define O_RDONLY 0
#endif
#ifndef O_WRONLY
#define O_WRONLY 1
#endif
#ifndef O_RDWR
#define O_RDWR 2
#endif

#define OSSDSP_MIN_RATE 4000UL
#define OSSDSP_MAX_RATE 20000UL

/*
 * Match the default /dev/dsp DMA block.  With the SB driver queuing one block
 * asynchronously, this lets the next MFM read overlap the current playback.
 */
#define DSP_BUFSIZE 4096

static char buf[DSP_BUFSIZE];
static unsigned char hdr[12];
static unsigned char skipbuf[64];

static unsigned short
rd16(unsigned char *p)
{
	return (unsigned short)p[0] | ((unsigned short)p[1] << 8);
}

static unsigned long
rd32(unsigned char *p)
{
	return (unsigned long)p[0]
		| ((unsigned long)p[1] << 8)
		| ((unsigned long)p[2] << 16)
		| ((unsigned long)p[3] << 24);
}

static int
match4(unsigned char *p, const char *s)
{
	return p[0] == (unsigned char)s[0]
		&& p[1] == (unsigned char)s[1]
		&& p[2] == (unsigned char)s[2]
		&& p[3] == (unsigned char)s[3];
}

static int
read_head(int fd, unsigned char *buf, int len)
{
	int got = 0;

	while (got < len) {
		int r = read(fd, buf + got, (unsigned)(len - got));

		if (r <= 0)
			break;
		got += r;
	}
	return got;
}

static int
skip_bytes(int fd, unsigned long len)
{
	while (len) {
		unsigned want = sizeof(skipbuf);
		int r;

		if (len < want)
			want = (unsigned)len;
		r = read(fd, skipbuf, want);
		if (r <= 0)
			return -1;
		len -= (unsigned long)r;
	}
	return 0;
}

static int
find_wav_data(int fd, unsigned long *rate)
{
	unsigned char chdr[8];
	unsigned char fmt[16];
	unsigned long size;
	int got_fmt = 0;

	if (!match4(hdr, "RIFF") || !match4(hdr + 8, "WAVE"))
		return 0;

	for (;;) {
		if (read_head(fd, chdr, sizeof(chdr)) != sizeof(chdr))
			return -1;
		size = rd32(chdr + 4);

		if (match4(chdr, "fmt ")) {
			if (size < 16)
				return -1;
			if (read_head(fd, fmt, sizeof(fmt)) != sizeof(fmt))
				return -1;
			if (rd16(fmt + 0) != 1 || rd16(fmt + 2) != 1 || rd16(fmt + 14) != 8)
				return -1;
			*rate = rd32(fmt + 4);
			got_fmt = 1;
			size -= 16;
		} else if (match4(chdr, "data")) {
			if (!got_fmt)
				return -1;
			return 1;
		}

		if (size && skip_bytes(fd, size) < 0)
			return -1;
		if ((rd32(chdr + 4) & 1UL) && skip_bytes(fd, 1) < 0)
			return -1;
	}
}

static int
setup_dsp(int dsp, unsigned long hz)
{
	oss_int32_t fmt, rate, ch;
	int r;
	int err = 0;

	fmt = (oss_int32_t)AFMT_U8;
	r = ioctl(dsp, SNDCTL_DSP_SETFMT, &fmt);
	if (r < 0) {
		perror("ossdsp: SNDCTL_DSP_SETFMT");
		err = 1;
	}

	rate = (oss_int32_t)hz;
	r = ioctl(dsp, SNDCTL_DSP_SPEED, &rate);
	if (r < 0) {
		perror("ossdsp: SNDCTL_DSP_SPEED");
		err = 1;
	}

	ch = 1;
	r = ioctl(dsp, SNDCTL_DSP_CHANNELS, &ch);
	if (r < 0) {
		perror("ossdsp: SNDCTL_DSP_CHANNELS");
		err = 1;
	}

	return err;
}

static int
write_full(int fd, char *buf, int len)
{
	int off = 0;

	while (off < len) {
		int w = write(fd, buf + off, (unsigned)(len - off));

		if (w <= 0)
			return -1;
		off += w;
	}
	return 0;
}

int
main(int argc, char **argv)
{
	int dsp, in;
	int n;
	int err = 0;
	int wav;
	unsigned long rate = 8000;

	if (argc > 1) {
		in = open(argv[1], O_RDONLY);
		if (in < 0) {
			perror("ossdsp: open input");
			return 1;
		}
	} else
		in = 0; /* stdin */

	dsp = open("/dev/dsp", O_RDWR);
	if (dsp < 0)
		dsp = open("/dev/audio", O_RDWR);
	if (dsp < 0) {
		perror("ossdsp: open /dev/dsp or /dev/audio");
		if (in != 0)
			close(in);
		return 1;
	}

	if (argc > 2) {
		long rr = atol(argv[2]);

		if (rr >= (long)OSSDSP_MIN_RATE && rr <= (long)OSSDSP_MAX_RATE)
			rate = (unsigned long)rr;
	}

	wav = 0;
	n = read_head(in, hdr, sizeof(hdr));
	if (n == sizeof(hdr))
		wav = find_wav_data(in, &rate);
	if (wav < 0) {
		fprintf(stderr, "ossdsp: unsupported WAV (need PCM U8 mono)\n");
		close(dsp);
		if (in != 0)
			close(in);
		return 1;
	}
	if (rate < OSSDSP_MIN_RATE || rate > OSSDSP_MAX_RATE) {
		fprintf(stderr, "ossdsp: unsupported rate (4000-20000)\n");
		close(dsp);
		if (in != 0)
			close(in);
		return 1;
	}

	err = setup_dsp(dsp, rate);

	if (!wav && n > 0) {
		if (write_full(dsp, (char *)hdr, n) < 0) {
			perror("ossdsp: write");
			close(dsp);
			if (in != 0)
				close(in);
			return 1;
		}
	}

	while ((n = read(in, buf, sizeof buf)) > 0) {
		if (write_full(dsp, buf, n) < 0) {
			perror("ossdsp: write");
			break;
		}
	}

	ioctl(dsp, SNDCTL_DSP_SYNC, (char *) 0);
	close(dsp);
	if (in != 0)
		close(in);
	return err;
}
