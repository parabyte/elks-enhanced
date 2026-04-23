/*
 * Report OSS API level and whether /dev/dsp is available.
 * ELKS /dev/dsp is 8-bit ISA DMA audio only (SB-compatible path), not PCI/USB.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>

#ifndef O_RDWR
#define O_RDWR 2
#endif

int
main(void)
{
	int fd;

	printf("OSS userland API (ELKS subset, 8-bit ISA DMA only)\n");
	printf("SOUND_VERSION (headers) 0x%lx\n", (unsigned long)SOUND_VERSION);

	fd = open("/dev/dsp", O_RDWR);
	if (fd < 0)
		fd = open("/dev/audio", O_RDWR);
	if (fd < 0) {
		printf("/dev/dsp: not present (no 8-bit ISA /dev/dsp driver in kernel)\n");
		return 0;
	}

		{
			int32_t kver = 0;
			int32_t caps = 0;
			int32_t blksz = 0;
			int32_t odelay = 0;
			int32_t pvol = (100 | (100 << 8));
			int32_t rate = 0;
			int32_t trig = PCM_ENABLE_OUTPUT;
			int32_t chq = 0;
			audio_errinfo aerr;

		if (ioctl(fd, OSS_GETVERSION, &kver) == 0)
			printf("OSS_GETVERSION (kernel) 0x%lx\n", (unsigned long)kver);
		else
			perror("ossinfo: OSS_GETVERSION");
		if (ioctl(fd, SNDCTL_DSP_GETCAPS, &caps) == 0)
			printf("SNDCTL_DSP_GETCAPS 0x%lx (OUTPUT=0x%lx TRIGGER=0x%lx)\n",
			       (unsigned long)caps, (unsigned long)PCM_CAP_OUTPUT,
			       (unsigned long)PCM_CAP_TRIGGER);
		else
			perror("ossinfo: SNDCTL_DSP_GETCAPS");
		if (ioctl(fd, SNDCTL_DSP_GETBLKSIZE, &blksz) == 0)
			printf("SNDCTL_DSP_GETBLKSIZE %ld\n", (long)blksz);
		else
			perror("ossinfo: SNDCTL_DSP_GETBLKSIZE");
			if (ioctl(fd, SNDCTL_DSP_SPEED, &rate) == 0)
				printf("SNDCTL_DSP_SPEED (query 0) -> %ld Hz\n", (long)rate);
			else
				perror("ossinfo: SNDCTL_DSP_SPEED query");
			if (ioctl(fd, SNDCTL_DSP_CHANNELS, &chq) == 0)
				printf("SNDCTL_DSP_CHANNELS (query 0) -> %ld\n", (long)chq);
		else
			perror("ossinfo: SNDCTL_DSP_CHANNELS query");
		if (ioctl(fd, SNDCTL_DSP_GETTRIGGER, &trig) == 0)
			printf("SNDCTL_DSP_GETTRIGGER 0x%lx (OUTPUT=%d INPUT=%d)\n",
			       (unsigned long)trig,
			       (trig & PCM_ENABLE_OUTPUT) != 0,
			       (trig & PCM_ENABLE_INPUT) != 0);
		else
			perror("ossinfo: SNDCTL_DSP_GETTRIGGER");
		if (ioctl(fd, SNDCTL_DSP_GETODELAY, &odelay) == 0)
			printf("SNDCTL_DSP_GETODELAY %ld\n", (long)odelay);
		else
			perror("ossinfo: SNDCTL_DSP_GETODELAY");
		if (ioctl(fd, SNDCTL_DSP_GETPLAYVOL, &pvol) == 0)
			printf("SNDCTL_DSP_GETPLAYVOL L=%ld R=%ld (0-100 each)\n",
			       (long)(pvol & 0xFF), (long)((pvol >> 8) & 0xFF));
		else
			perror("ossinfo: SNDCTL_DSP_GETPLAYVOL");
		memset(&aerr, 0, sizeof(aerr));
		if (ioctl(fd, SNDCTL_DSP_GETERROR, &aerr) == 0)
			printf("SNDCTL_DSP_GETERROR underruns=%ld errorcount=%ld lasterr=%ld\n",
			       (long)aerr.play_underruns, (long)aerr.play_errorcount,
			       (long)aerr.play_lasterror);
		else
			perror("ossinfo: SNDCTL_DSP_GETERROR");
	}
	close(fd);
	printf("/dev/dsp or /dev/audio: open ok\n");
	return 0;
}
