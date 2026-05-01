#include "uip-glue.h"

struct packet_stats_s netstats;
struct arp_cache arp_cache[ARP_CACHE_MAX];

static unsigned char arp_replace;

struct ktcp_arp_pkt {
	struct uip_eth_hdr ethhdr;
	u16_t hwtype;
	u16_t protocol;
	u8_t hwlen;
	u8_t protolen;
	u16_t opcode;
	struct uip_eth_addr shwaddr;
	u16_t sipaddr[2];
	struct uip_eth_addr dhwaddr;
	u16_t dipaddr[2];
};

struct ktcp_ip_pkt {
	struct uip_eth_hdr ethhdr;
	u8_t vhl;
	u8_t tos;
	u8_t len[2];
	u8_t ipid[2];
	u8_t ipoffset[2];
	u8_t ttl;
	u8_t proto;
	u16_t ipchksum;
	u16_t srcipaddr[2];
	u16_t destipaddr[2];
};

static ipaddr_t pkt_ip_to_ipaddr(const u16_t *addr)
{
	ipaddr_t ip;

	memcpy(&ip, addr, sizeof(ip));
	return ip;
}

static void fill_cb_stats(struct cb_stats_s *cbstats, struct ktcp_slot *slot)
{
	unsigned char flags;

	memset(cbstats, 0, sizeof(*cbstats));
	if (!slot) {
		cbstats->valid = 0;
		return;
	}

	cbstats->valid = 1;
	cbstats->state = ktcp_state_for_slot(slot);
	cbstats->remaddr = slot->remote_addr;
	cbstats->remport = slot->remote_port;
	cbstats->localport = slot->local_port;
	cbstats->time_wait_exp = slot->time_wait_exp;
	if (slot->uconn) {
		flags = slot->uconn->tcpstateflags & UIP_TS_MASK;
		if (flags == UIP_TIME_WAIT)
			cbstats->time_wait_exp = Now +
				(timeq_t)(UIP_TIME_WAIT_TIMEOUT - slot->uconn->timer) * 8;
		cbstats->rtt = slot->uconn->rto * 500U;
	}
}

static struct ktcp_slot *slot_by_visible_index(int index)
{
	int i;
	int n;

	n = 0;
	for (i = 0; i < KTCP_MAX_SOCKETS; i++) {
		if (!ktcp_slots[i].used)
			continue;
		if (!ktcp_slots[i].listening &&
		    !ktcp_slots[i].connected &&
		    !ktcp_slots[i].connect_pending &&
		    !ktcp_slots[i].peer_closed &&
		    !ktcp_slots[i].netconf)
			continue;
		if (n == index)
			return &ktcp_slots[i];
		n++;
	}
	return NULL;
}

void netconf_init(void)
{
	memset(&netstats, 0, sizeof(netstats));
	memset(arp_cache, 0, sizeof(arp_cache));
	arp_replace = 0;
}

void ktcp_arp_learn(ipaddr_t ip, const eth_addr_t mac)
{
	int i;
	int free_slot;
	uip_ipaddr_t ipaddr;
	struct uip_eth_addr eaddr;

	if (ip == 0 || ip == local_ip)
		return;

	uip_ipaddr_from_ip(&ipaddr, ip);
	memcpy(eaddr.addr, mac, sizeof(eaddr.addr));
	uip_arp_register(ipaddr, &eaddr);
	uip_tracef("arp learn %u.%u.%u.%u\n",
		((unsigned char *)&ip)[0], ((unsigned char *)&ip)[1],
		((unsigned char *)&ip)[2], ((unsigned char *)&ip)[3]);

	free_slot = -1;
	for (i = 0; i < ARP_CACHE_MAX; i++) {
		if (arp_cache[i].ip_addr == ip) {
			memcpy(arp_cache[i].eth_addr, mac, sizeof(eth_addr_t));
			arp_cache[i].valid = 1;
			arp_cache[i].qpacket = NULL;
			arp_cache[i].len = 0;
			return;
		}
		if (free_slot < 0 && arp_cache[i].ip_addr == 0)
			free_slot = i;
	}

	if (free_slot < 0)
		free_slot = arp_replace++ % ARP_CACHE_MAX;

	arp_cache[free_slot].ip_addr = ip;
	memcpy(arp_cache[free_slot].eth_addr, mac, sizeof(eth_addr_t));
	arp_cache[free_slot].valid = 1;
	arp_cache[free_slot].qpacket = NULL;
	arp_cache[free_slot].len = 0;
	netstats.arpcacheadds++;
}

void ktcp_arp_observe_frame(const unsigned char *frame, int len)
{
	const struct uip_eth_hdr *eth;
	const struct ktcp_ip_pkt *ippkt;
	const struct ktcp_arp_pkt *arppkt;
	ipaddr_t ip;
	unsigned short type;
	unsigned short op;

	if (len < (int)sizeof(struct uip_eth_hdr))
		return;

	eth = (const struct uip_eth_hdr *)frame;
	type = ((unsigned short)((const unsigned char *)&eth->type)[0] << 8) |
		((const unsigned char *)&eth->type)[1];

	if (type == UIP_ETHTYPE_IP && len >= (int)sizeof(struct ktcp_ip_pkt)) {
		ippkt = (const struct ktcp_ip_pkt *)frame;
		ip = pkt_ip_to_ipaddr(ippkt->srcipaddr);
		ktcp_arp_learn(ip, ippkt->ethhdr.src.addr);
		return;
	}

	if (type != UIP_ETHTYPE_ARP || len < (int)sizeof(struct ktcp_arp_pkt))
		return;

	arppkt = (const struct ktcp_arp_pkt *)frame;
	op = ((unsigned short)((const unsigned char *)&arppkt->opcode)[0] << 8) |
		((const unsigned char *)&arppkt->opcode)[1];
	ip = pkt_ip_to_ipaddr(arppkt->sipaddr);
	ktcp_arp_learn(ip, arppkt->shwaddr.addr);
	if (op == ARP_REQUEST)
		netstats.arprcvreqcnt++;
	else if (op == ARP_REPLY)
		netstats.arprcvreplycnt++;
}

int ktcp_netconf_response(unsigned char *buf, unsigned int buflen,
	const struct stat_request_s *sr)
{
	struct general_stats_s gstats;
	struct cb_stats_s cbstats;

	switch (sr->type) {
	case NS_GENERAL:
		if (buflen < sizeof(gstats))
			return -1;
		memset(&gstats, 0, sizeof(gstats));
		gstats.cb_num = 0;
		while (slot_by_visible_index(gstats.cb_num))
			gstats.cb_num++;
		gstats.retrans_memory = ktcp_retrans_memory();
		memcpy(buf, &gstats, sizeof(gstats));
		return sizeof(gstats);
	case NS_CB:
		if (buflen < sizeof(cbstats))
			return -1;
		fill_cb_stats(&cbstats, slot_by_visible_index(sr->extra));
		memcpy(buf, &cbstats, sizeof(cbstats));
		return sizeof(cbstats);
	case NS_NETSTATS:
		if (buflen < sizeof(netstats))
			return -1;
#if UIP_STATISTICS == 1
		netstats.ipbadhdr = uip_stat.ip.vhlerr + uip_stat.ip.hblenerr +
			uip_stat.ip.lblenerr + uip_stat.ip.fragerr +
			uip_stat.ip.protoerr;
		netstats.ipbadchksum = uip_stat.ip.chkerr;
		netstats.iprcvcnt = uip_stat.ip.recv;
		netstats.ipsndcnt = uip_stat.ip.sent;
		netstats.icmprcvcnt = uip_stat.icmp.recv;
		netstats.icmpsndcnt = uip_stat.icmp.sent;
		netstats.tcpbadchksum = uip_stat.tcp.chkerr;
		netstats.tcprcvcnt = uip_stat.tcp.recv;
		netstats.tcpsndcnt = uip_stat.tcp.sent;
		netstats.tcpdropcnt = uip_stat.tcp.drop + uip_stat.tcp.syndrop;
		netstats.tcpretranscnt = uip_stat.tcp.rexmit;
#endif
		memcpy(buf, &netstats, sizeof(netstats));
		return sizeof(netstats);
	case NS_ARP:
		if (buflen < sizeof(arp_cache))
			return -1;
		memcpy(buf, arp_cache, sizeof(arp_cache));
		return sizeof(arp_cache);
	default:
		break;
	}

	return -1;
}
