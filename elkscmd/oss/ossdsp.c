/*
 * Minimal raw 8-bit mono PCM player using OSS ioctls on /dev/dsp.
 * Expects 8-bit ISA DMA playback (SB-compatible kernel driver), not PCI/USB.
 *
 * Usage: ossdsp [path] [sample_rate_hz]
 * Default: stdin, 8000 Hz. Sample rate must be 4000–48000.
 * Opens /dev/dsp O_RDWR; exits 1 if SETFMT, SPEED, or CHANNELS ioctl fails.
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

static char buf[256];

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
	oss_int32_t fmt, rate, ch;
	int n;
	int r;
	int err = 0;

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

	fmt = (oss_int32_t)AFMT_U8;
	r = ioctl(dsp, SNDCTL_DSP_SETFMT, &fmt);
	if (r < 0) {
		perror("ossdsp: SNDCTL_DSP_SETFMT");
		err = 1;
	}

	rate = 8000;
	if (argc > 2) {
		long rr = atol(argv[2]);

		if (rr >= 4000L && rr <= 48000L)
			rate = (oss_int32_t)rr;
	}
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
