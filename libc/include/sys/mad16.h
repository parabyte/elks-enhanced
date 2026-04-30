/*
 * OPTi 82C928/82C929 (MAD16 family) — MC register I/O for mad16cfg(1).
 *
 * Port map matches Linux sound/isa/opti9xx (82C929: mc_base 0xf8c, pwd_reg 3,
 * password 0xe3; MCn accessed at mc_base + n).  Some old notes list 0xf2e/
 * 0xf44; real ISA 929 boards use the 0xf8cx window — wrong bases yield all
 * 0xff reads and failed detection.  No ioctl on /dev/port is required.
 */
#ifndef __SYS_MAD16_H
#define __SYS_MAD16_H

#define MAD16_MC_BASE     0xF8CU
#define MAD16_PASSWORD    0xE3U
/* Unlock: outb(MAD16_PASSWORD, MAD16_PASSWD_PORT); same I/O as MC3 */
#define MAD16_PASSWD_PORT (MAD16_MC_BASE + 3U)
#define MAD16_MC1_PORT    (MAD16_MC_BASE + 1U)
#define MAD16_MC2_PORT    (MAD16_MC_BASE + 2U)
#define MAD16_MC3_PORT    (MAD16_MC_BASE + 3U)
#define MAD16_MC4_PORT    (MAD16_MC_BASE + 4U)
#define MAD16_MC5_PORT    (MAD16_MC_BASE + 5U)
#define MAD16_MC6_PORT    (MAD16_MC_BASE + 6U)

#define MAD16_GAMEPORT_BASE 0x200U
#define MAD16_GAMEPORT_LAST 0x207U

#define MAD16_SB_PORT_220 0x220U
#define MAD16_SB_PORT_240 0x240U

#define MAD16_MPU_PORT_300 0x300U
#define MAD16_MPU_PORT_310 0x310U
#define MAD16_MPU_PORT_320 0x320U
#define MAD16_MPU_PORT_330 0x330U

/* Sentinel for sb_irq/sb_dma/mpu_irq "off" in XT profile */
#define MAD16_DISABLED 0xFFU

#define MAD16_MODE_SB 1

#define MAD16_WSS_IRQ_AUTO 0U
#define MAD16_CD_DISABLED  0U

struct mad16_regs {
    unsigned char mc1, mc2, mc3, mc4, mc5, mc6;
};

struct mad16_cfg {
    unsigned short wss_port;
    unsigned short cd_port;
    unsigned short sb_port;
    unsigned short mpu_port;
    unsigned char mode;
    unsigned char powerdown;
    unsigned char gameport;
    unsigned char gpmode;
    unsigned char fmap_single;
    unsigned char fmclk_opl2;
    unsigned char sbver;
    unsigned char sb_irq;
    unsigned char sb_dma;
    unsigned char mpu_enable;
    unsigned char mpu_irq;
    unsigned char wss_irq;
    unsigned char wss_dma;
    unsigned char cd_type;
    unsigned char cd_irq;
    unsigned char cd_dma;
    unsigned char opl4;
    unsigned char adpcm;
    unsigned char gpout;
    unsigned char timeout;
    unsigned char outmix;
    unsigned char silence;
    unsigned char shpass;
    unsigned char spaccess;
    unsigned char codec_access;
    unsigned char fifo;
    unsigned char res2_must1;
    unsigned char cfix;
    unsigned char res0_must1;
    unsigned char dma_watchdog;
    unsigned char wave;
    unsigned char attn;
};

/* MC1 */
#define MAD16_MC1_POWERDOWN 0x40U
#define MAD16_MC1_GAME_OFF  0x01U

/* MC2 */
#define MAD16_MC2_OPL4             0x20U
#define MAD16_MC2_CDSEL_DISABLED   0x03U  /* Linux mad16.c default: no CD IRQ, DMA off */
#define MAD16_MC2_CD_DMA_MASK      0x03U

/* MC3 */
#define MAD16_MC3_IRQ_MASK  0xC0U
#define MAD16_MC3_DMA_MASK  0x30U
#define MAD16_MC3_SB_240    0x04U
#define MAD16_MC3_FMAP      0x08U
#define MAD16_MC3_GP_TIMER  0x02U  /* write GPMODE; read overlaps REV bit 1 */
#define MAD16_MC3_REV_MASK  0x03U

/* MC4 */
#define MAD16_MC4_ADPCM       0x80U
#define MAD16_MC4_TIMEOUT     0x20U
#define MAD16_MC4_SILENCE     0x04U
#define MAD16_MC4_SBVER_MASK  0x03U

/* MC5 */
#define MAD16_MC5_MUST1_7      0x80U
#define MAD16_MC5_MUST0_6      0x40U
#define MAD16_MC5_SHPASS       0x20U
#define MAD16_MC5_SPACCESS     0x10U
#define MAD16_MC5_CODEC_ACCESS MAD16_MC5_SPACCESS
#define MAD16_MC5_CFIFO        0x08U
#define MAD16_MC5_RES2_MUST1   0x04U
#define MAD16_MC5_CFIX         0x02U
#define MAD16_MC5_RES0_MUST1   0x01U
#define MAD16_MC5_LINUX_C929   (MAD16_MC5_MUST1_7 | MAD16_MC5_SHPASS | \
                                MAD16_MC5_RES2_MUST1 | MAD16_MC5_RES0_MUST1)

/* MC6 */
#define MAD16_MC6_MPU_ENABLE   0x80U
#define MAD16_MC6_MPU_IRQ      0x18U
#define MAD16_MC6_MPU_OFF_MASK 0x07U

#endif
