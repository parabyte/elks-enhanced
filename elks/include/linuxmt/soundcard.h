/*
 * OSS DSP ioctl values for kernel (must match elks-sound/elks-port/include/sys/soundcard.h).
 * /dev/dsp driver supports only 8-bit ISA DMA (8237 channels 1 or 3), not 16-bit ISA DMA.
 * OPTi 82C929 (MAD16 Pro) works in Sound Blaster Pro mode at the same DSP ports.
 */

#ifndef __LINUXMT_SOUNDCARD_H
#define __LINUXMT_SOUNDCARD_H

#include <linuxmt/types.h>

#define OSS_SIOCPARM_MASK 0x1fffU
#define OSS_SIOC_VOID     0x00000000U
#define OSS_SIOC_OUT      0x20000000U
#define OSS_SIOC_IN       0x40000000U
#define OSS_SIOC_INOUT    (OSS_SIOC_IN | OSS_SIOC_OUT)

#define OSS__SIO(x, y) \
	((int)(OSS_SIOC_VOID | (((x) & 0xff) << 8) | ((y) & 0xff)))
#define OSS__SIOR(x, y, t) \
	((int)(OSS_SIOC_OUT | (((sizeof(t)) & OSS_SIOCPARM_MASK) << 16) | (((x) & 0xff) << 8) | ((y) & 0xff)))
#define OSS__SIOW(x, y, t) \
	((int)(OSS_SIOC_IN | (((sizeof(t)) & OSS_SIOCPARM_MASK) << 16) | (((x) & 0xff) << 8) | ((y) & 0xff)))
#define OSS__SIOWR(x, y, t) \
	((int)(OSS_SIOC_INOUT | (((sizeof(t)) & OSS_SIOCPARM_MASK) << 16) | (((x) & 0xff) << 8) | ((y) & 0xff)))

#define __SIO    OSS__SIO
#define __SIOR   OSS__SIOR
#define __SIOW   OSS__SIOW
#define __SIOWR  OSS__SIOWR

typedef __s32 oss_int32_t;

#define OSS_VERSION   0x040100U
#define SOUND_VERSION OSS_VERSION

#define SNDCTL_DSP_HALT     __SIO('P', 0)
#define SNDCTL_DSP_RESET    SNDCTL_DSP_HALT
#define SNDCTL_DSP_SYNC     __SIO('P', 1)
#define SNDCTL_DSP_SPEED    __SIOWR('P', 2, oss_int32_t)
#define SNDCTL_DSP_STEREO   __SIOWR('P', 3, oss_int32_t)
#define SNDCTL_DSP_GETBLKSIZE __SIOWR('P', 4, oss_int32_t)
#define SNDCTL_DSP_SETFMT   __SIOWR('P', 5, oss_int32_t)
#define SNDCTL_DSP_SAMPLESIZE SNDCTL_DSP_SETFMT
#define SNDCTL_DSP_CHANNELS __SIOWR('P', 6, oss_int32_t)
#define SNDCTL_DSP_POST     __SIO('P', 8)
#define SNDCTL_DSP_SUBDIVIDE __SIOWR('P', 9, oss_int32_t)
#define SNDCTL_DSP_SETFRAGMENT __SIOWR('P', 10, oss_int32_t)
#define SNDCTL_DSP_GETFMTS  __SIOR('P', 11, oss_int32_t)

#define AFMT_QUERY     0x00000000U
#define AFMT_U8        0x00000008U
#define AFMT_S16_LE    0x00000010U

typedef struct audio_buf_info {
	oss_int32_t fragments;
	oss_int32_t fragstotal;
	oss_int32_t fragsize;
	oss_int32_t bytes;
} audio_buf_info;

typedef struct count_info {
	__u32 bytes;
	oss_int32_t blocks;
	oss_int32_t ptr;
} count_info;

typedef struct audio_errinfo {
	oss_int32_t play_underruns;
	oss_int32_t rec_overruns;
	__u32 play_ptradjust;
	__u32 rec_ptradjust;
	oss_int32_t play_errorcount;
	oss_int32_t rec_errorcount;
	oss_int32_t play_lasterror;
	oss_int32_t rec_lasterror;
	oss_int32_t play_errorparm;
	oss_int32_t rec_errorparm;
	oss_int32_t filler[16];
} audio_errinfo;

#define SNDCTL_DSP_GETOSPACE __SIOR('P', 12, audio_buf_info)
#define SNDCTL_DSP_GETISPACE __SIOR('P', 13, audio_buf_info)
#define SNDCTL_DSP_NONBLOCK __SIO('P', 14)
#define SNDCTL_DSP_GETCAPS   __SIOR('P', 15, oss_int32_t)
#define SNDCTL_DSP_GETTRIGGER __SIOR('P', 16, oss_int32_t)
#define SNDCTL_DSP_SETTRIGGER __SIOW('P', 16, oss_int32_t)
#define SNDCTL_DSP_GETIPTR   __SIOR('P', 17, count_info)
#define SNDCTL_DSP_GETOPTR   __SIOR('P', 18, count_info)
#define SNDCTL_DSP_GETODELAY __SIOR('P', 23, oss_int32_t)
#define SNDCTL_DSP_GETPLAYVOL __SIOR('P', 24, oss_int32_t)
#define SNDCTL_DSP_SETPLAYVOL __SIOWR('P', 24, oss_int32_t)
#define SNDCTL_DSP_GETERROR   __SIOR('P', 25, audio_errinfo)

#define SNDCTL_DSP_SETSYNCRO __SIO('P', 21)
#define SNDCTL_DSP_SETDUPLEX __SIO('P', 22)

#define SNDCTL_DSP_COOKEDMODE __SIOW('P', 30, oss_int32_t)
#define SNDCTL_DSP_SILENCE   __SIO('P', 31)
#define SNDCTL_DSP_SKIP      __SIO('P', 32)
#define SNDCTL_DSP_HALT_INPUT __SIO('P', 33)
#define SNDCTL_DSP_HALT_OUTPUT __SIO('P', 34)
#define SNDCTL_DSP_RESET_OUTPUT SNDCTL_DSP_HALT_OUTPUT

#define SNDCTL_DSP_GETCHANNELMASK __SIOWR('P', 64, oss_int32_t)
#define SNDCTL_DSP_BIND_CHANNEL   __SIOWR('P', 65, oss_int32_t)

#define OSS_GETVERSION       __SIOR('M', 118, oss_int32_t)

/* Obsolete names still used by some OSS/Linux programs */
#define SOUND_PCM_WRITE_BITS     SNDCTL_DSP_SETFMT
#define SOUND_PCM_WRITE_RATE     SNDCTL_DSP_SPEED
#define SOUND_PCM_WRITE_CHANNELS SNDCTL_DSP_CHANNELS
#define SOUND_PCM_POST           SNDCTL_DSP_POST
#define SOUND_PCM_RESET          SNDCTL_DSP_RESET
#define SOUND_PCM_SYNC           SNDCTL_DSP_SYNC
#define SOUND_PCM_SUBDIVIDE      SNDCTL_DSP_SUBDIVIDE
#define SOUND_PCM_SETFRAGMENT    SNDCTL_DSP_SETFRAGMENT
#define SOUND_PCM_GETFMTS        SNDCTL_DSP_GETFMTS
#define SOUND_PCM_SETFMT         SNDCTL_DSP_SETFMT
#define SOUND_PCM_GETOSPACE      SNDCTL_DSP_GETOSPACE
#define SOUND_PCM_GETISPACE      SNDCTL_DSP_GETISPACE
#define SOUND_PCM_NONBLOCK       SNDCTL_DSP_NONBLOCK
#define SOUND_PCM_GETCAPS        SNDCTL_DSP_GETCAPS
#define SOUND_PCM_GETTRIGGER     SNDCTL_DSP_GETTRIGGER
#define SOUND_PCM_SETTRIGGER     SNDCTL_DSP_SETTRIGGER
#define SOUND_PCM_GETIPTR        SNDCTL_DSP_GETIPTR
#define SOUND_PCM_GETOPTR        SNDCTL_DSP_GETOPTR

#define PCM_CAP_REVISION  0x000000ffU
#define PCM_CAP_TRIGGER   0x00001000U
#define PCM_CAP_OUTPUT    0x00020000U

#define PCM_ENABLE_INPUT  0x00000001U
#define PCM_ENABLE_OUTPUT 0x00000002U

#endif
