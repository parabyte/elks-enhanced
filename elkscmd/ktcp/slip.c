#include "uip-glue.h"
#include "vjhc.h"

#define SERIAL_BUFFER_SIZE 256
#define SLIP_HEADROOM 128
#define END 0300
#define ESC 0333
#define ESC_END 0334
#define ESC_ESC 0335

static unsigned char sbuf[SERIAL_BUFFER_SIZE];
static unsigned char packet[SLIP_MTU + SLIP_HEADROOM];
static unsigned char lastchar;
static unsigned int packpos;
static int devfd;

static speed_t convert_baudrate(speed_t baudrate)
{
	switch (baudrate) {
	case 50: return B50;
	case 75: return B75;
	case 110: return B110;
	case 134: return B134;
	case 150: return B150;
	case 200: return B200;
	case 300: return B300;
	case 600: return B600;
	case 1200: return B1200;
	case 1800: return B1800;
	case 2400: return B2400;
	case 4800: return B4800;
	case 9600: return B9600;
	case 19200: return B19200;
	case 38400: return B38400;
	case 57600: return B57600;
	case 115200: return B115200;
#ifdef B230400
	case 230400: return B230400;
#endif
#ifdef B460800
	case 460800: return B460800;
#endif
#ifdef B500000
	case 500000: return B500000;
#endif
#ifdef B576000
	case 576000: return B576000;
#endif
#ifdef B921600
	case 921600: return B921600;
#endif
#ifdef B1000000
	case 1000000: return B1000000;
#endif
	default:
		printf("uip: unknown baud rate %lu\n", (unsigned long)baudrate);
		return (speed_t)-1;
	}
}

#if CSLIP
static void cslip_decompress(unsigned char **pkt, size_t *len)
{
	pkt_ut p;
	unsigned char *base;
	__u8 type;

	base = *pkt;
	p.p_data = base;
	p.p_size = *len;
	p.p_offset = SLIP_HEADROOM;
	p.p_maxsize = sizeof(packet);

	type = *(base + SLIP_HEADROOM) & 0xf0;
	if (type != TYPE_IP) {
		if (type & 0x80) {
			ip_vjhc_arr_compr(&p);
			debug_cslip("cslip: Compressed TCP packet offset %d size %d (%d)\n",
				p.p_offset, p.p_size, *len);
		} else if (type == TYPE_UNCOMPRESSED_TCP) {
			*(base + SLIP_HEADROOM) &= 0x4f;
			ip_vjhc_arr_uncompr(&p);
			debug_cslip("cslip: Uncompressed TCP packet offset %d size %d (%d)\n",
				p.p_offset, p.p_size, *len);
		}
		if (p.p_size > 0)
			*pkt = base + p.p_offset;
	} else {
		debug_cslip("cslip: IP packet\n");
		*pkt = base + SLIP_HEADROOM;
	}

	*len = p.p_size;
}

static void cslip_compress(unsigned char **pkt, int *len)
{
	pkt_ut p;
	__u8 type;

	p.p_data = *pkt;
	p.p_size = *len;
	p.p_offset = 0;
	p.p_maxsize = MTU;

	type = ip_vjhc_compress(&p);
	if (type != TYPE_IP) {
		*pkt += p.p_offset;
		*len = p.p_size;
		(*pkt)[0] |= type;
	}
}
#endif

int slip_init(char *fdev, speed_t baudrate)
{
	speed_t baud;
	struct termios tios;

#if CSLIP
	if (linkprotocol == LINK_CSLIP && ip_vjhc_init() < 0)
		return -1;
#endif

	baud = 0;
	if (baudrate) {
		baud = convert_baudrate(baudrate);
		if (baud == (speed_t)-1)
			return -1;
	}

	devfd = open(fdev, O_RDWR | O_NONBLOCK | O_EXCL | O_NOCTTY);
	if (devfd < 0) {
		printf("uip: failed to open serial device %s\n", fdev);
		return -1;
	}

	if (tcgetattr(devfd, &tios) < 0) {
		close(devfd);
		printf("uip: failed to read serial termios on %s\n", fdev);
		return -1;
	}

	if (baud) {
		cfsetispeed(&tios, baud);
		cfsetospeed(&tios, baud);
	}

	tios.c_iflag = 0;
	tios.c_oflag = 0;
	tios.c_lflag = 0;
	tios.c_cflag &= ~(CSIZE | PARENB | CSTOPB);
#ifdef CRTSCTS
	tios.c_cflag &= ~CRTSCTS;
#endif
	tios.c_cflag |= CS8 | CREAD | CLOCAL;
	tios.c_cc[VMIN] = 1;
	tios.c_cc[VTIME] = 0;
	if (tcsetattr(devfd, TCSAFLUSH, &tios) < 0) {
		close(devfd);
		printf("uip: failed to set serial termios on %s\n", fdev);
		return -1;
	}

	packpos = SLIP_HEADROOM;
	lastchar = 0;
	return devfd;
}

void slip_process(void)
{
	unsigned char *payload;
	size_t psize;
	int i;
	int len;

	while ((len = read(devfd, sbuf, sizeof(sbuf))) > 0) {
		for (i = 0; i < len; i++) {
			if (lastchar == ESC) {
				switch (sbuf[i]) {
				case ESC_END:
					packet[packpos++] = END;
					break;
				case ESC_ESC:
					packet[packpos++] = ESC;
					break;
				default:
					packet[packpos++] = sbuf[i];
					break;
				}
			} else {
				switch (sbuf[i]) {
				case ESC:
					break;
				case END:
					if (packpos == SLIP_HEADROOM)
						break;

					payload = packet;
					psize = packpos - SLIP_HEADROOM;
#if CSLIP
					if (linkprotocol == LINK_CSLIP)
						cslip_decompress(&payload, &psize);
					else
#endif
						payload += SLIP_HEADROOM;

					if (psize > 0) {
						netstats.sliprcvcnt++;
						ktcp_process_slip_packet(payload, psize);
					}

					packpos = SLIP_HEADROOM;
					lastchar = 0;
					continue;
				default:
					if (packpos < sizeof(packet))
						packet[packpos++] = sbuf[i];
					break;
				}
			}
			lastchar = sbuf[i];
		}
	}
}

void slip_send(unsigned char *packet, int len)
{
	unsigned char buf[SLIP_MTU + 2];
	unsigned char *p;
	unsigned char *q;

	p = packet;
#if CSLIP
	if (linkprotocol == LINK_CSLIP)
		cslip_compress(&p, &len);
#endif
	debug_cslip("slip: send %d\n", len);

	q = buf;
	*q++ = END;
	while (len--) {
		switch (*p) {
		case END:
			*q++ = ESC;
			*q++ = ESC_END;
			break;
		case ESC:
			*q++ = ESC;
			*q++ = ESC_ESC;
			break;
		default:
			*q++ = *p;
			break;
		}
		p++;
		if (q - buf >= (int)sizeof(buf) - 2) {
			write(devfd, buf, q - buf);
			q = buf;
		}
	}
	*q++ = END;
	write(devfd, buf, q - buf);
	netstats.slipsndcnt++;
}
