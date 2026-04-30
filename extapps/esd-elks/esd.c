/*
 * Minimal ELKS Enlightened Sound Daemon compatible endpoint.
 *
 * This is a practical ELKS subset: it listens on TCP (default 16001) and
 * treats client payload as raw U8 mono PCM stream written to /dev/dsp.
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
#define ESD_IO_CHUNK 4096

static int dsp_fd = -1;
static char io_buf[ESD_IO_CHUNK];

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

static int dsp_configure(int rate)
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
	return 0;
}

static int server_socket(int port)
{
	int s;
	struct sockaddr_in addr;

	s = socket(AF_INET, SOCK_STREAM, 0);
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
	if (listen(s, 1) < 0) {
		perror("esd: listen");
		close(s);
		return -1;
	}
	return s;
}

int main(int argc, char **argv)
{
	int port = ESD_DEF_PORT;
	long rate = ESD_DEF_RATE;
	long v;
	int s;

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
	if (rate < 4000L || rate > 48000L) {
		fprintf(stderr, "esd: bad rate (4000-48000)\n");
		return 1;
	}
	if (dsp_configure((int)rate) < 0)
		return 1;

	s = server_socket(port);
	if (s < 0)
		return 1;

	printf("esd: listening on tcp/%d\n", port);
	for (;;) {
		int c = accept(s, (struct sockaddr *)0, (unsigned *)0);
		int n;

		if (c < 0) {
			perror("esd: accept");
			continue;
		}
		printf("esd: client connected\n");
		while ((n = read(c, io_buf, sizeof(io_buf))) > 0) {
			if (write_full(dsp_fd, io_buf, n) < 0) {
				perror("esd: dsp write");
				break;
			}
		}
		ioctl(dsp_fd, SNDCTL_DSP_SYNC, (char *)0);
		close(c);
		printf("esd: client disconnected\n");
	}
}
