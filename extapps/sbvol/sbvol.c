#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <termios.h>
#include <unistd.h>

static struct termios saved_tio;
static int have_tio;
static int dsp_fd = -1;

static void restore_tty(void)
{
	if (have_tio)
		tcsetattr(0, TCSANOW, &saved_tio);
	printf("\033[0m\033[?25h\n");
}

static void on_signal(int sig)
{
	(void)sig;
	restore_tty();
	exit(1);
}

static int raw_tty(void)
{
	struct termios tio;

	if (tcgetattr(0, &saved_tio) < 0)
		return -1;
	have_tio = 1;
	tio = saved_tio;
	tio.c_lflag &= ~(ICANON | ECHO);
	tio.c_lflag |= ISIG;
	tio.c_cc[VMIN] = 1;
	tio.c_cc[VTIME] = 0;
	return tcsetattr(0, TCSANOW, &tio);
}

static int clamp(int v)
{
	if (v < 0)
		return 0;
	if (v > 100)
		return 100;
	return v;
}

static int get_vol(int *left, int *right)
{
	oss_int32_t v = (oss_int32_t)-1;

	if (ioctl(dsp_fd, SNDCTL_DSP_GETPLAYVOL, &v) < 0)
		return -1;
	*left = (int)(v & 0xff);
	*right = (int)((v >> 8) & 0xff);
	return 0;
}

static int set_vol(int left, int right)
{
	oss_int32_t v;

	left = clamp(left);
	right = clamp(right);
	v = (oss_int32_t)(left | (right << 8));
	return ioctl(dsp_fd, SNDCTL_DSP_SETPLAYVOL, &v);
}

static void bar(const char *name, int value)
{
	int i;
	int n = (value + 5) / 10;

	printf("%s %3d [", name, value);
	for (i = 0; i < 10; i++)
		putchar(i < n ? '#' : '-');
	printf("]\n");
}

static void draw(int left, int right)
{
	printf("\033[2J\033[H\033[?25l");
	printf("sbvol\n\n");
	bar("L", left);
	bar("R", right);
	printf("\n+/- both  a/z left  k/m right  b balance  q quit\n");
	fflush(stdout);
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/dsp";
	int left, right;
	char c;

	if (argc > 1)
		dev = argv[1];
	dsp_fd = open(dev, O_WRONLY);
	if (dsp_fd < 0) {
		perror(dev);
		return 1;
	}
	if (get_vol(&left, &right) < 0) {
		perror("SNDCTL_DSP_GETPLAYVOL");
		close(dsp_fd);
		return 1;
	}
	if (raw_tty() < 0) {
		perror("termios");
		close(dsp_fd);
		return 1;
	}
	atexit(restore_tty);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	draw(left, right);
	while (read(0, &c, 1) == 1) {
		switch (c) {
		case '+':
		case '=':
			left += 5;
			right += 5;
			break;
		case '-':
		case '_':
			left -= 5;
			right -= 5;
			break;
		case 'a':
			left += 5;
			break;
		case 'z':
			left -= 5;
			break;
		case 'k':
			right += 5;
			break;
		case 'm':
			right -= 5;
			break;
		case 'b':
			left = right = (left + right) / 2;
			break;
		case 'q':
		case 'Q':
			close(dsp_fd);
			return 0;
		default:
			draw(left, right);
			continue;
		}
		left = clamp(left);
		right = clamp(right);
		if (set_vol(left, right) < 0) {
			perror("SNDCTL_DSP_SETPLAYVOL");
			close(dsp_fd);
			return 1;
		}
		draw(left, right);
	}

	close(dsp_fd);
	return 0;
}
