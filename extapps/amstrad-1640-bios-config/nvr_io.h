/*
 * nvr_io.h - PC1640 port I/O backend.
 *
 * Define NVR_REAL_IO for the real ELKS/8086 target.  Native builds use a
 * small simulator so smoke tests can verify command flow without touching
 * host I/O ports.
 */

#ifndef NVR_IO_H
#define NVR_IO_H

#include "nvr_ports.h"

#ifdef NVR_REAL_IO

static unsigned char inb(unsigned short port)
{
    unsigned char val;
    __asm__ volatile ("inb %%dx, %%al" : "=a" (val) : "d" (port));
    return val;
}

static void outb(unsigned char val, unsigned short port)
{
    __asm__ volatile ("outb %%al, %%dx" : : "a" (val), "d" (port));
}

/* Short delay for I/O bus settling (~1us on 8MHz 8086) */
static void io_delay(void)
{
    __asm__ volatile ("outb %%al, $0x80" : : "a" (0));
}

#else

#include <stdio.h>
#include <stdlib.h>

static unsigned char nvr_sim_cmos[CMOS_SIZE];
static unsigned char nvr_sim_cmos_addr;
static unsigned char nvr_sim_pb;
static unsigned char nvr_sim_sysstat2;
static unsigned char nvr_sim_com1_lcr;
static unsigned char nvr_sim_com2_lcr;
static unsigned char nvr_sim_lpt1_data;
static unsigned char nvr_sim_lpt2_data;
static unsigned char nvr_sim_mouse_x;
static unsigned char nvr_sim_mouse_y;
static int nvr_sim_ready;

static void nvr_sim_checksum(void)
{
    unsigned int sum = 0;
    int i;

    for (i = 0x10; i <= 0x2D; i++)
        sum += nvr_sim_cmos[i];

    nvr_sim_cmos[CMOS_CHECKSUM_HI] = (sum >> 8) & 0xFF;
    nvr_sim_cmos[CMOS_CHECKSUM_LO] = sum & 0xFF;
}

static void nvr_sim_init(void)
{
    if (nvr_sim_ready)
        return;

    nvr_sim_ready = 1;
    nvr_sim_pb = 0x00;
    nvr_sim_sysstat2 = 0x5A;
    nvr_sim_lpt1_data = 0xAA;
    nvr_sim_lpt2_data = 0xAA;

    nvr_sim_cmos[RTC_SECONDS] = 0x00;
    nvr_sim_cmos[RTC_MINUTES] = 0x34;
    nvr_sim_cmos[RTC_HOURS] = 0x12;
    nvr_sim_cmos[RTC_DAY_OF_WEEK] = 0x06;
    nvr_sim_cmos[RTC_DAY_OF_MONTH] = 0x02;
    nvr_sim_cmos[RTC_MONTH] = 0x05;
    nvr_sim_cmos[RTC_YEAR] = 0x26;
    nvr_sim_cmos[RTC_REG_A] = 0x26;
    nvr_sim_cmos[RTC_REG_B] = RTC_B_24H;
    nvr_sim_cmos[RTC_REG_D] = RTC_D_VRT;
    nvr_sim_cmos[RTC_ALARM_SEC] = 0xC0;
    nvr_sim_cmos[RTC_ALARM_MIN] = 0xC0;
    nvr_sim_cmos[RTC_ALARM_HRS] = 0xC0;
    nvr_sim_cmos[CMOS_FLOPPY] = 0x30;
    nvr_sim_cmos[CMOS_DISK] = 0x20;
    nvr_sim_cmos[CMOS_EQUIP] = 0x01;
    nvr_sim_cmos[CMOS_BASEMEM_LO] = 0x80;
    nvr_sim_cmos[CMOS_BASEMEM_HI] = 0x02;
    nvr_sim_cmos[CMOS_CENTURY] = 0x20;
    nvr_sim_checksum();
}

static int nvr_trace_io(void)
{
    static int checked;
    static int enabled;

    if (!checked) {
        enabled = getenv("NVR_TRACE_IO") != 0;
        checked = 1;
    }
    return enabled;
}

static unsigned char nvr_sim_read(unsigned short port)
{
    nvr_sim_init();

    switch (port) {
    case CMOS_DATA_PORT:
        return nvr_sim_cmos[nvr_sim_cmos_addr & 0x3F];
    case PORT_PB:
        return nvr_sim_pb;
    case PORT_STATUS2:
        if (nvr_sim_pb & PB_NIBBLE_SEL)
            return nvr_sim_sysstat2 & 0x0F;
        return (nvr_sim_sysstat2 >> 4) & 0x0F;
    case PORT_KBD_DATA:
        return 0x0D;
    case PORT_LPT1_DATA:
        return nvr_sim_lpt1_data | 0x07;
    case PORT_LPT1_STATUS:
        return 0x27;      /* English, DIP latch set, EGA display */
    case PORT_LPT1_CTRL:
        return 0x0C;
    case PORT_LPT2_DATA:
        return nvr_sim_lpt2_data;
    case PORT_IDA_STATUS:
        return 0x00;
    case PORT_MOUSE_X:
        return nvr_sim_mouse_x;
    case PORT_MOUSE_Y:
        return nvr_sim_mouse_y;
    case PORT_COM1_BASE + 3:
        return nvr_sim_com1_lcr;
    case PORT_COM2_BASE + 3:
        return nvr_sim_com2_lcr;
    case PORT_PIC_CMD:
        return 0x00;
    case PORT_PIC_DATA:
        return 0x00;
    case PORT_DMA_STAT:
        return 0x00;
    case PORT_PIT_CH2:
        return 0x34;
    case PORT_GAME:
        return 0xF0;
    case PORT_DEAD:
        return 0x00;
    default:
        return 0xFF;
    }
}

static void nvr_sim_write(unsigned char val, unsigned short port)
{
    nvr_sim_init();

    switch (port) {
    case CMOS_ADDR_PORT:
        nvr_sim_cmos_addr = val & 0x3F;
        break;
    case CMOS_DATA_PORT:
        if (nvr_sim_cmos_addr != RTC_REG_C && nvr_sim_cmos_addr != RTC_REG_D)
            nvr_sim_cmos[nvr_sim_cmos_addr & 0x3F] = val;
        break;
    case PORT_PB:
        nvr_sim_pb = val;
        break;
    case PORT_SYSSTAT2_WR:
        nvr_sim_sysstat2 = val;
        break;
    case PORT_MOUSE_X:
        nvr_sim_mouse_x = 0;
        break;
    case PORT_MOUSE_Y:
        nvr_sim_mouse_y = 0;
        break;
    case PORT_COM1_BASE + 3:
        nvr_sim_com1_lcr = val;
        break;
    case PORT_COM2_BASE + 3:
        nvr_sim_com2_lcr = val;
        break;
    case PORT_LPT1_DATA:
        nvr_sim_lpt1_data = val;
        break;
    case PORT_LPT2_DATA:
        nvr_sim_lpt2_data = val;
        break;
    default:
        break;
    }
}

static unsigned char inb(unsigned short port)
{
    unsigned char val = nvr_sim_read(port);

    if (nvr_trace_io())
        fprintf(stderr, "IN  0x%04X -> 0x%02X\n", port, val);

    return val;
}

static void outb(unsigned char val, unsigned short port)
{
    if (nvr_trace_io())
        fprintf(stderr, "OUT 0x%04X <- 0x%02X\n", port, val);

    nvr_sim_write(val, port);
}

static void io_delay(void)
{
}

#endif

#endif
