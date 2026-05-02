/*
 * Minimal ELKS Enlightened Sound Daemon compatible endpoint.
 *
 * This is a practical ELKS subset: it listens on UDP (default 16001) and
 * treats datagram payloads as raw U8 mono PCM written to /dev/dsp.
 * It does not implement the full historical ESD command protocol.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/soundcard.h>

#ifndef O_RDWR
#define O_RDWR 2
#endif

#define ESD_DEF_PORT 16001
#define ESD_DEF_RATE 8000
#define ESD_IO_CHUNK 256
#define ESD_MIN_RATE 3000L
#define ESD_DSP_MIN_RATE 4000U
/* Keep user-space aligned with the SB driver's exact-divisor ceiling. */
#define ESD_MAX_RATE 20000L

static int dsp_fd = -1;
static char io_buf[ESD_IO_CHUNK];
static char dsp_buf[ESD_IO_CHUNK * 2];

static int write_full(int fd, const char *buf, int len)
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

static long dsp_configure(long rate)
{
	oss_int32_t fmt, ch, r;

	dsp_fd = open("/dev/dsp", O_RDWR);
	if (dsp_fd < 0)
		dsp_fd = open("/dev/audio", O_RDWR);
	if (dsp_fd < 0) {
		perror("esd: open /dev/dsp or /dev/audio");
		return -1;
	}

	fmt = AFMT_U8;
	if (ioctl(dsp_fd, SNDCTL_DSP_SETFMT, &fmt) < 0) {
		perror("esd: SNDCTL_DSP_SETFMT");
		close(dsp_fd);
		dsp_fd = -1;
		return -1;
	}
	ch = 1;
	if (ioctl(dsp_fd, SNDCTL_DSP_CHANNELS, &ch) < 0) {
		perror("esd: SNDCTL_DSP_CHANNELS");
		close(dsp_fd);
		dsp_fd = -1;
		return -1;
	}
	r = (oss_int32_t)rate;
	if (ioctl(dsp_fd, SNDCTL_DSP_SPEED, &r) < 0) {
		perror("esd: SNDCTL_DSP_SPEED");
		close(dsp_fd);
		dsp_fd = -1;
		return -1;
	}
	printf("esd: dsp configured U8 mono %ld Hz\n", (long)r);
	return (long)r;
}

static int resample_u8(const char *in, int in_len, char *out, int out_size,
		unsigned int in_rate, unsigned int out_rate,
		unsigned long *phase)
{
	int i, out_len = 0;

	if (in_rate == out_rate) {
		if (in_len > out_size)
			in_len = out_size;
		memcpy(out, in, (unsigned)in_len);
		return in_len;
	}

	if (in_rate == 3000 && out_rate == 4000) {
		int state = (int)*phase;

		for (i = 0; i < in_len; i++) {
			if (out_len >= out_size)
				break;
			out[out_len++] = in[i];
			if (state == 2) {
				if (out_len >= out_size)
					break;
				out[out_len++] = in[i];
				state = 0;
			} else {
				state++;
			}
		}
		*phase = (unsigned long)state;
		return out_len;
	}

	/* Integer remainder accumulator: fixed-point rate conversion, no FP. */
	for (i = 0; i < in_len; i++) {
		*phase += (unsigned long)out_rate;
		while (*phase >= in_rate) {
			if (out_len >= out_size)
				return out_len;
			out[out_len++] = in[i];
			*phase -= in_rate;
		}
	}
	return out_len;
}

static int udp_socket(int port)
{
	int s;
	struct sockaddr_in addr;

	s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0) {
		perror("esd: socket");
		return -1;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((unsigned short)port);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("esd: bind");
		close(s);
		return -1;
	}
	return s;
}

int main(int argc, char **argv)
{
	int port = ESD_DEF_PORT;
	long rate = ESD_DEF_RATE;
	unsigned int input_rate;
	unsigned int dsp_rate;
	long actual_rate;
	long v;
	int s;
	unsigned long phase = 0;

	if (argc > 1) {
		v = atol(argv[1]);
		if (v > 0 && v <= 65535L)
			port = (int)v;
	}
	if (argc > 2) {
		v = atol(argv[2]);
		if (v > 0 && v <= 65535L)
			rate = v;
	}
	if (port <= 0) {
		fprintf(stderr, "esd: bad port\n");
		return 1;
	}
	if (rate < ESD_MIN_RATE || rate > ESD_MAX_RATE) {
		fprintf(stderr, "esd: bad rate (3000-20000)\n");
		return 1;
	}
	input_rate = (unsigned int)rate;
	dsp_rate = input_rate < ESD_DSP_MIN_RATE ? ESD_DSP_MIN_RATE : input_rate;
	actual_rate = dsp_configure((long)dsp_rate);
	if (actual_rate < 0)
		return 1;
	dsp_rate = (unsigned int)actual_rate;
	if (dsp_rate != input_rate)
		printf("esd: UDP stream U8 mono %u Hz, DSP output %u Hz\n",
			input_rate, dsp_rate);

	s = udp_socket(port);
	if (s < 0)
		return 1;

	printf("esd: listening on udp/%d\n", port);
	for (;;) {
		int n = read(s, io_buf, sizeof(io_buf));
		int out;

		if (n <= 0) {
			if (n < 0)
				perror("esd: udp read");
			continue;
		}
		out = resample_u8(io_buf, n, dsp_buf, sizeof(dsp_buf),
			input_rate, dsp_rate, &phase);
		if (write_full(dsp_fd, dsp_buf, out) < 0) {
			perror("esd: dsp write");
			break;
		}
	}
	close(s);
	close(dsp_fd);
	return 1;
}
