/*
 * PC speaker demo via console KIOCSOUND (not ISA /dev/dsp; no sound card).
 * For 8-bit ISA DMA PCM use ossdsp when /dev/dsp exists. PIT period 1193181/hz.
 */

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <linuxmt/kd.h>
#include <sys/ioctl.h>

#ifndef O_RDWR
#define O_RDWR 2
#endif

static unsigned
period_for_hz(unsigned hz)
{
	if (hz < 20U)
		hz = 20U;
	return 1193181U / hz;
}

static void
tone(int fd, unsigned hz, unsigned ms)
{
	unsigned p = period_for_hz(hz);

	ioctl(fd, KIOCSOUND, (char *) (unsigned long) p);
	usleep((unsigned long) ms * 1000UL);
	ioctl(fd, KIOCSOUND, (char *) 0UL);
}

int
main(void)
{
	int fd;
	static const unsigned notes[] = { 262, 294, 330, 349, 392, 440, 494, 523 };
	unsigned i;

	fd = open("/dev/tty0", O_RDWR);
	if (fd < 0) {
		perror("oss_beep: open /dev/tty0");
		return 1;
	}

	for (i = 0; i < sizeof(notes) / sizeof(notes[0]); i++)
		tone(fd, notes[i], 120U);

	close(fd);
	return 0;
}
