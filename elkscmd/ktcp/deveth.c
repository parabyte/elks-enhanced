#include "uip-glue.h"

eth_addr_t eth_local_addr;

static unsigned char sbuf[ETH_MTU + sizeof(struct uip_eth_hdr)];
static int devfd;

int deveth_init(char *fdev)
{
	devfd = open(fdev, O_NONBLOCK | O_RDWR);
	if (devfd < 0) {
		printf("uip: failed to open eth device %s\n", fdev);
		return -1;
	}

	if (ioctl(devfd, IOCTL_ETH_ADDR_GET, eth_local_addr) < 0) {
		printf("uip: IOCTL_ETH_ADDR_GET fail\n");
		close(devfd);
		devfd = -1;
		return -2;
	}

	return devfd;
}

void eth_process(void)
{
	int len;

	len = read(devfd, sbuf, sizeof(sbuf));
	uip_tracef("eth read=%d errno=%d\n", len, len < 0 ? errno : 0);
	if (len <= (int)sizeof(struct uip_eth_hdr))
		return;

	netstats.ethrcvcnt++;
	ktcp_process_ethernet_frame(sbuf, len);
}

void eth_write(unsigned char *packet, int len)
{
	struct uip_eth_hdr *eth;
	unsigned short op;

	if (devfd < 0 || len <= 0)
		return;

	write(devfd, packet, len);
	netstats.ethsndcnt++;

	if (len < (int)sizeof(struct uip_eth_hdr))
		return;

	eth = (struct uip_eth_hdr *)packet;
	if (eth->type != KTCP_HTONS(UIP_ETHTYPE_ARP))
		return;

	if (len < (int)(sizeof(struct uip_eth_hdr) + 8))
		return;

	op = ((unsigned short)packet[20] << 8) | packet[21];
	if (op == ARP_REQUEST)
		netstats.arpsndreqcnt++;
	else if (op == ARP_REPLY)
		netstats.arpsndreplycnt++;
}

void eth_printhex(unsigned char *packet, int len)
{
	unsigned char *p;
	int i;

	p = packet;
	i = 0;
	if (len > 128)
		len = 128;
	while (len--) {
		printf("%02X ", *p++);
		if ((i++ & 15) == 15)
			printf("\n");
	}
	printf("\n");
}

char *mac_ntoa(eth_addr_t eth_addr)
{
	unsigned char *p;
	static char b[18];

	p = (unsigned char *)eth_addr;
	sprintf(b, "%02x.%02x.%02x.%02x.%02x.%02x",
		p[0], p[1], p[2], p[3], p[4], p[5]);
	return b;
}
