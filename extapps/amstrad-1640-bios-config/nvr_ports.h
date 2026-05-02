/*
 * nvr_ports.h - Amstrad PC1640 RTC/CMOS and system I/O definitions
 */

#ifndef NVR_PORTS_H
#define NVR_PORTS_H

/* MC146818 RTC/CMOS ports */
#define CMOS_ADDR_PORT  0x70
#define CMOS_DATA_PORT  0x71
#define CMOS_SIZE       64      /* PC1640: 64-byte CMOS only (mask 0x3F) */

/* RTC time/date registers */
#define RTC_SECONDS      0x00
#define RTC_ALARM_SEC    0x01
#define RTC_MINUTES      0x02
#define RTC_ALARM_MIN    0x03
#define RTC_HOURS        0x04
#define RTC_ALARM_HRS    0x05
#define RTC_DAY_OF_WEEK  0x06
#define RTC_DAY_OF_MONTH 0x07
#define RTC_MONTH        0x08
#define RTC_YEAR         0x09

/* RTC status registers */
#define RTC_REG_A        0x0A
#define RTC_REG_B        0x0B
#define RTC_REG_C        0x0C   /* Read-only, clears IRQ flags on read */
#define RTC_REG_D        0x0D   /* Read-only, VRT (battery) flag */

/* Status Register A bits */
#define RTC_A_UIP        0x80   /* Update In Progress */
#define RTC_A_DV_MASK    0x70   /* Divider select (oscillator) */
#define RTC_A_DV_SHIFT   4
#define RTC_A_RS_MASK    0x0F   /* Rate select (periodic interrupt) */

/* Status Register B bits */
#define RTC_B_SET        0x80   /* SET - stop updates for safe read/write */
#define RTC_B_PIE        0x40   /* Periodic Interrupt Enable */
#define RTC_B_AIE        0x20   /* Alarm Interrupt Enable */
#define RTC_B_UIE        0x10   /* Update-ended Interrupt Enable */
#define RTC_B_SQWE       0x08   /* Square Wave Enable (SQW pin) */
#define RTC_B_DM         0x04   /* Data Mode: 1=binary, 0=BCD */
#define RTC_B_24H        0x02   /* 24-hour mode */
#define RTC_B_DSE        0x01   /* Daylight Savings Enable */

/* Status Register C bits */
#define RTC_C_IRQF       0x80   /* IRQ flag (composite) */
#define RTC_C_PF         0x40   /* Periodic interrupt flag */
#define RTC_C_AF         0x20   /* Alarm flag */
#define RTC_C_UF         0x10   /* Update-ended flag */

/* Status Register D bits */
#define RTC_D_VRT        0x80   /* Valid RAM and Time (battery OK) */

/* CMOS configuration addresses */
#define CMOS_DIAG        0x0E   /* Diagnostic status byte (POST results) */
#define CMOS_SHUTDOWN    0x0F   /* Shutdown status byte */
#define CMOS_FLOPPY      0x10   /* Floppy drive types (hi=A, lo=B) */
#define CMOS_RSVD_11     0x11   /* Reserved */
#define CMOS_DISK        0x12   /* Hard disk types (hi=drv0, lo=drv1) */
#define CMOS_RSVD_13     0x13   /* Reserved */
#define CMOS_EQUIP       0x14   /* Equipment byte */
#define CMOS_BASEMEM_LO  0x15   /* Base memory low byte (KB) */
#define CMOS_BASEMEM_HI  0x16   /* Base memory high byte */
#define CMOS_EXTMEM_LO   0x17   /* Extended memory low byte (KB) */
#define CMOS_EXTMEM_HI   0x18   /* Extended memory high byte */
#define CMOS_DISK0_EXT   0x19   /* Hard disk 0 extended type */
#define CMOS_DISK1_EXT   0x1A   /* Hard disk 1 extended type */
#define CMOS_CHECKSUM_HI 0x2E   /* CMOS checksum high byte */
#define CMOS_CHECKSUM_LO 0x2F   /* CMOS checksum low byte */
#define CMOS_CENTURY     0x32   /* Century (BCD, e.g. 0x20) */

/* Amstrad system ports */
#define PORT_KBD_DATA       0x60   /* Keyboard data / system status 1 */
#define PORT_PB             0x61   /* PB register */
#define PORT_STATUS2        0x62   /* System status 2 (nibble-selected read) */
#define PORT_SYSSTAT1_WR    0x64   /* System status 1 latch (write) */
#define PORT_SYSSTAT2_WR    0x65   /* System status 2 / NVR latch (write) */
#define PORT_SOFT_RESET     0x66   /* Soft reset trigger (write) */
#define PORT_MOUSE_X        0x78   /* Amstrad mouse X counter */
#define PORT_MOUSE_Y        0x7A   /* Amstrad mouse Y counter */

/* PB register bits (port 0x61) */
#define PB_SPEAKER_GATE     0x01   /* PIT channel 2 gate */
#define PB_SPEAKER_ENABLE   0x02   /* Speaker amplifier enable */
#define PB_NIBBLE_SEL       0x04   /* 0=high nibble, 1=low nibble */
#define PB_KBD_RESET        0x40   /* Keyboard reset (rising edge) */
#define PB_STATUS_MODE      0x80   /* 0=keyboard data, 1=system status */

/* Standard PC ports */
#define PORT_PIC_CMD        0x20   /* 8259A PIC command */
#define PORT_PIC_DATA       0x21   /* 8259A PIC data (IMR) */
#define PORT_NMI_MASK       0xA0   /* NMI mask register */
#define PORT_PIT_CH2        0x42   /* 8253 PIT channel 2 count */
#define PORT_PIT_MODE       0x43   /* 8253 PIT mode control */
#define PORT_DMA_STAT       0x08   /* 8237A DMA status */

/* Serial/parallel port addresses */
#define PORT_COM1_BASE      0x3F8
#define PORT_COM2_BASE      0x2F8
#define PORT_LPT1_DATA      0x378
#define PORT_LPT1_STATUS    0x379
#define PORT_LPT1_CTRL      0x37A
#define PORT_LPT2_DATA      0x3BC

/* Video ports */
#define PORT_CRTC_ADDR_CGA  0x3D4
#define PORT_CRTC_DATA_CGA  0x3D5
#define PORT_CGA_MODE       0x3D8
#define PORT_CGA_STATUS     0x3DA
#define PORT_VID_SWITCH     0x3DB   /* PC1640: bit 6 = CGA/EGA toggle */
#define PORT_VID_EXT        0x3DD   /* Amstrad extended video */
#define PORT_IDA_STATUS     0x3DE   /* IDA disabled flag (0x20 = off) */

/* Game port */
#define PORT_GAME           0x201

/* Dead-man diagnostic port */
#define PORT_DEAD           0xDEAD

/* LPT1 status register bit fields (PC1640-specific) */
#define LPT1_LANG_MASK      0x07   /* Bits 0-2: language code */
#define LPT1_DIP_LATCH      0x20   /* Bit 5: DIP switch latch */
#define LPT1_DISP_MASK      0xC0   /* Bits 6-7: display type */
#define LPT1_DISP_SHIFT     6

#endif
