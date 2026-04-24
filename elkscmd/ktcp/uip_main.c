#include "uip-glue.h"

ipaddr_t local_ip;
ipaddr_t gateway_ip;
ipaddr_t netmask_ip;
ipaddr_t dns_server_ip;

int linkprotocol = LINK_ETHER;
unsigned int MTU;
int dflag;
struct ktcp_slot ktcp_slots[KTCP_MAX_SOCKETS];
#if UIP_CONF_UDP
struct kudp_slot kudp_slots[KUDP_MAX_SOCKETS];
#endif

#if UIP_CONF_DHCPC
char dhcpc_appstate_tag;
#endif
#if UIP_CONF_RESOLV
char resolv_appstate_tag;
#endif

#define DEFAULT_IP "10.0.2.15"
#define DEFAULT_GATEWAY "10.0.2.2"
#define DEFAULT_NETMASK "255.255.255.0"
#define DEFAULT_DHCP_IP "0.0.0.0"
#define UIP_STATE_FILE "/tmp/uip-network.cfg"
#define UIP_RESOLV_FILE "/etc/resolv.cfg"

static char ethdev[10] = "/dev/ne0";
static char *serdev = "/dev/ttyS0";
static speed_t baudrate = 57600;
static int intfd;
static unsigned char arp_timer_ticks;
static int tracefd = -1;
static ipaddr_t static_dns_ip;
static int dhcp_enabled;
static int network_configured;
static int resolv_enabled;
static int resolv_query_pending;
static int resolv_query_done;
static int resolv_query_success;
static ipaddr_t resolv_query_ip;
static char resolv_query_name[64];

ipaddr_t uip_ipaddr_to_ip(const uip_ipaddr_t addr)
{
	ipaddr_t ip;

	memcpy(&ip, addr, sizeof(ip));
	return ip;
}

void uip_ipaddr_from_ip(uip_ipaddr_t *addr, ipaddr_t ip)
{
	memcpy(addr, &ip, sizeof(ip));
}

u16_t uip_link_mss(void)
{
	unsigned int mss;

	mss = UIP_TCP_MSS;
	if (linkprotocol != LINK_ETHER) {
		if (MTU > UIP_TCPIP_HLEN)
			mss = MTU - UIP_TCPIP_HLEN;
		else
			mss = 64;
		if (mss > 192)
			mss = 192;
		if (mss < 64)
			mss = 64;
	}
	return mss;
}

u16_t uip_link_window(void)
{
	if (linkprotocol != LINK_ETHER)
		return uip_link_mss() * 2;
	return UIP_RECEIVE_WINDOW;
}

static void copy_ip_string(char *dst, size_t dstlen, ipaddr_t ip)
{
	const char *src;

	src = in_ntoa(ip);
	if (src == NULL)
		src = DEFAULT_DHCP_IP;
	strncpy(dst, src, dstlen - 1);
	dst[dstlen - 1] = '\0';
}

static void write_text_file(const char *path, const char *text)
{
	int fd;

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return;
	write(fd, text, strlen(text));
	close(fd);
	sync();
}

static const char *resolv_status_string(void)
{
	if (!resolv_enabled)
		return "disabled";
	if (resolv_query_name[0] == '\0')
		return dns_server_ip ? "ready" : "idle";
	if (resolv_query_done)
		return resolv_query_success ? "found" : "notfound";
	if (dns_server_ip == 0)
		return "queued";
	return resolv_query_pending ? "pending" : "idle";
}

static void write_resolv_cfg(void)
{
	char dnsbuf[20];
	char buf[96];

	if (dns_server_ip != 0) {
		copy_ip_string(dnsbuf, sizeof(dnsbuf), dns_server_ip);
		snprintf(buf, sizeof(buf),
			"# Managed by uip\nnameserver %s\n", dnsbuf);
	} else {
		snprintf(buf, sizeof(buf),
			"# Managed by uip\n");
	}
	write_text_file(UIP_RESOLV_FILE, buf);
}

void uip_write_runtime_state(void)
{
	char ipbuf[20];
	char gwbuf[20];
	char maskbuf[20];
	char dnsbuf[20];
	char querybuf[20];
	char buf[320];

	copy_ip_string(ipbuf, sizeof(ipbuf), local_ip);
	copy_ip_string(gwbuf, sizeof(gwbuf), gateway_ip);
	copy_ip_string(maskbuf, sizeof(maskbuf), netmask_ip);
	copy_ip_string(dnsbuf, sizeof(dnsbuf), dns_server_ip);
	copy_ip_string(querybuf, sizeof(querybuf), resolv_query_ip);

	snprintf(buf, sizeof(buf),
		"# Managed by uip\n"
		"mode=%s\n"
		"configured=%d\n"
		"ip=%s\n"
		"gateway=%s\n"
		"netmask=%s\n"
		"dns=%s\n"
		"resolv_enabled=%d\n"
		"resolv_name=%s\n"
		"resolv_status=%s\n"
		"resolv_result=%s\n",
		dhcp_enabled ? "dhcp" : "static",
		network_configured ? 1 : 0,
		ipbuf, gwbuf, maskbuf, dnsbuf,
		resolv_enabled ? 1 : 0,
		resolv_query_name,
		resolv_status_string(),
		querybuf);
	write_text_file(UIP_STATE_FILE, buf);
}

static void start_resolver_query(void)
{
#if UIP_CONF_RESOLV
	if (!resolv_enabled || dns_server_ip == 0 || resolv_query_name[0] == '\0')
		return;
	if (resolv_query_pending || resolv_query_done)
		return;

	resolv_query_pending = 1;
	resolv_query_success = 0;
	resolv_query_ip = 0;
	uip_tracef("resolv query %s\n", resolv_query_name);
	resolv_query(resolv_query_name);
	uip_write_runtime_state();
#endif
}

static void apply_network_config(ipaddr_t ip, ipaddr_t gateway, ipaddr_t mask)
{
	uip_ipaddr_t addr;

	local_ip = ip;
	gateway_ip = gateway;
	netmask_ip = mask;

	uip_ipaddr_from_ip(&addr, local_ip);
	uip_sethostaddr(&addr);
	uip_ipaddr_from_ip(&addr, gateway_ip);
	uip_setdraddr(&addr);
	uip_ipaddr_from_ip(&addr, netmask_ip);
	uip_setnetmask(&addr);
}

static void apply_dns_server(ipaddr_t ip)
{
	dns_server_ip = ip;
#if UIP_CONF_RESOLV
	if (resolv_enabled && dns_server_ip != 0) {
		uip_ipaddr_t addr;

		uip_ipaddr_from_ip(&addr, dns_server_ip);
		resolv_conf(&addr);
	}
#endif
	write_resolv_cfg();
	uip_write_runtime_state();
	start_resolver_query();
}

void uip_tracef(const char *fmt, ...)
{
	char buf[128];
	va_list ap;
	int len;

	if (tracefd < 0)
		return;

	va_start(ap, fmt);
	len = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (len <= 0)
		return;
	if (len > (int)sizeof(buf))
		len = sizeof(buf);
	write(tracefd, buf, len);
	sync();
}

void uip_log(char *msg)
{
	if (msg != NULL)
		uip_tracef("uip log: %s\n", msg);
}

#if UIP_CONF_RESOLV
void resolv_found(char *name, u16_t *ipaddr)
{
	resolv_query_pending = 0;
	resolv_query_done = 1;
	resolv_query_success = 0;
	resolv_query_ip = 0;

	if (ipaddr != NULL) {
		memcpy(&resolv_query_ip, ipaddr, sizeof(resolv_query_ip));
		resolv_query_success = 1;
	}

	uip_tracef("resolv result %s %s\n",
		name ? name : "",
		resolv_query_success ? "ok" : "fail");
	uip_write_runtime_state();
}
#endif

#if UIP_CONF_DHCPC
void dhcpc_configured(const struct dhcpc_state *s)
{
	ipaddr_t ip;
	ipaddr_t gateway;
	ipaddr_t mask;
	ipaddr_t dns;

	memcpy(&ip, s->ipaddr, sizeof(ip));
	memcpy(&gateway, s->default_router, sizeof(gateway));
	memcpy(&mask, s->netmask, sizeof(mask));
	memcpy(&dns, s->dnsaddr, sizeof(dns));

	apply_network_config(ip, gateway, mask);
	network_configured = 1;
	if (dns == 0)
		dns = static_dns_ip;
	{
		char ipbuf[20];
		char dnsbuf[20];

		copy_ip_string(ipbuf, sizeof(ipbuf), ip);
		copy_ip_string(dnsbuf, sizeof(dnsbuf), dns);
		uip_tracef("dhcp configured ip=%s dns=%s\n", ipbuf, dnsbuf);
	}
	apply_dns_server(dns);
	uip_write_runtime_state();
}
#endif

void ktcp_send_uip_output(int has_link_header)
{
	if (uip_len == 0)
		return;

	if (linkprotocol == LINK_ETHER) {
		if (!has_link_header) {
			uip_arp_out();
			if (uip_len == 0)
				return;
		}
		uip_tracef("tx len=%u type=%04x\n",
			uip_len,
			uip_len >= sizeof(struct uip_eth_hdr) ?
				((struct uip_eth_hdr *)&uip_buf[0])->type : 0);
		eth_write(uip_buf, uip_len);
	} else {
		uip_tracef("slip tx len=%u\n", uip_len);
		slip_send(&uip_buf[UIP_LLH_LEN], uip_len);
	}
}

void ktcp_process_ethernet_frame(const unsigned char *frame, int len)
{
	struct uip_eth_hdr *eth;

	if (len <= 0 || len > (int)(sizeof(uip_buf)))
		return;

	ktcp_arp_observe_frame(frame, len);
	memcpy(uip_buf, frame, len);
	uip_len = len;

	eth = (struct uip_eth_hdr *)&uip_buf[0];
	uip_tracef("rx len=%d type=%04x\n", len, eth->type);
	switch (eth->type) {
	case KTCP_HTONS(UIP_ETHTYPE_IP):
		uip_arp_ipin();
		uip_input();
		if (uip_len > 0)
			ktcp_send_uip_output(0);
		break;
	case KTCP_HTONS(UIP_ETHTYPE_ARP):
		uip_arp_arpin();
		if (uip_len > 0)
			ktcp_send_uip_output(1);
		break;
	default:
		uip_len = 0;
		break;
	}
}

void ktcp_process_slip_packet(const unsigned char *packet, int len)
{
	if (len <= 0 || len > (int)(sizeof(uip_buf) - UIP_LLH_LEN))
		return;

	memcpy(&uip_buf[UIP_LLH_LEN], packet, len);
	uip_len = len;
	uip_tracef("slip rx len=%d\n", len);
	if (len >= 8) {
		uip_tracef("slip tail %02x %02x %02x %02x %02x %02x %02x %02x\n",
			packet[len - 8], packet[len - 7], packet[len - 6], packet[len - 5],
			packet[len - 4], packet[len - 3], packet[len - 2], packet[len - 1]);
	}
	uip_input();
	if (uip_len > 0)
		ktcp_send_uip_output(0);
}

void ktcp_periodic(void)
{
	int i;

	for (i = 0; i < UIP_CONNS; i++) {
		uip_periodic(i);
		if (uip_len > 0)
			ktcp_send_uip_output(0);
	}

#if UIP_CONF_UDP
	for (i = 0; i < UIP_UDP_CONNS; i++) {
		uip_udp_periodic(i);
		if (uip_len > 0)
			ktcp_send_uip_output(0);
	}
	kudp_periodic();
#endif

	if (linkprotocol == LINK_ETHER) {
		if (++arp_timer_ticks >= KTCP_ARP_TIMER_TICKS) {
			arp_timer_ticks = 0;
			uip_arp_timer();
		}
	}

	ktcp_cleanup_slots();
}

void uip_stack_init(void)
{
	uip_ipaddr_t addr;
	struct uip_eth_addr eaddr;

	memset(ktcp_slots, 0, sizeof(ktcp_slots));
#if UIP_CONF_UDP
	memset(kudp_slots, 0, sizeof(kudp_slots));
#endif
	arp_timer_ticks = 0;
	dns_server_ip = 0;
	resolv_query_pending = 0;
	resolv_query_done = 0;
	resolv_query_success = 0;
	resolv_query_ip = 0;

	uip_init();
	memset(uip_conns, 0, sizeof(uip_conns));
#if UIP_CONF_UDP
	memset(uip_udp_conns, 0, sizeof(uip_udp_conns));
#endif

	apply_network_config(local_ip, gateway_ip, netmask_ip);

	if (linkprotocol == LINK_ETHER) {
		memcpy(eaddr.addr, eth_local_addr, sizeof(eaddr.addr));
		uip_setethaddr(eaddr);
		uip_arp_init();
	}

	netconf_init();

#if UIP_CONF_RESOLV
	if (resolv_enabled)
		resolv_init();
#endif

	if (dhcp_enabled) {
		network_configured = 0;
		write_resolv_cfg();
		uip_write_runtime_state();
#if UIP_CONF_DHCPC
		uip_ipaddr(addr, 0, 0, 0, 0);
		uip_sethostaddr(&addr);
		uip_setdraddr(&addr);
		uip_setnetmask(&addr);
		dhcpc_init(eth_local_addr, sizeof(eth_local_addr));
		dhcpc_request();
#endif
	} else {
		network_configured = 1;
		apply_dns_server(static_dns_ip);
		uip_write_runtime_state();
	}
}

void uip_daemon_run(void)
{
	fd_set fdset;
	fd_set wfds;
	struct timeval tv;
	int maxfd;
	int count;
	int loopagain;
	int had_activity;
	int udp_pending;

	loopagain = 0;

	while (1) {
		udp_pending = 0;
#if UIP_CONF_UDP
		udp_pending = kudp_has_pending();
#endif
		FD_ZERO(&fdset);
		FD_ZERO(&wfds);
		FD_SET(intfd, &fdset);
		FD_SET(netdevfd, &fdset);
		if (udp_pending)
			FD_SET(netdevfd, &wfds);
		maxfd = intfd > netdevfd ? intfd : netdevfd;

		tv.tv_sec = 0;
		tv.tv_usec = loopagain ? 0 : 500000L;
		count = select(maxfd + 1, &fdset, udp_pending ? &wfds : NULL, NULL, &tv);
		if (count < 0) {
			if (errno == EINTR)
				continue;
			printf("uip: select failed errno %d\n", errno);
			return;
		}

		Now = get_time();

		if (count == 0) {
			if (loopagain) {
				loopagain = 0;
				continue;
			}
			ktcp_periodic();
			continue;
		}

		had_activity = 0;
		if (FD_ISSET(intfd, &fdset)) {
			if (linkprotocol == LINK_ETHER)
				eth_process();
			else
				slip_process();
			had_activity = 1;
		}

		if (FD_ISSET(netdevfd, &fdset)) {
			netdev_process();
			had_activity = 1;
		}

#if UIP_CONF_UDP
		if (udp_pending && FD_ISSET(netdevfd, &wfds)) {
			kudp_periodic();
			had_activity = 1;
		}
#endif
		ktcp_cleanup_slots();
		loopagain = had_activity;
	}
}

#if USE_DEBUG_EVENT
int dprintf_on = DEBUG_STARTDEF;

void debug_toggle(int sig)
{
	dprintf_on = !dprintf_on;
	printf("\nuip: debug %s\n", dprintf_on ? "on" : "off");
	signal(SIGURG, debug_toggle);
}

void dprintf(const char *fmt, ...)
{
	va_list ptr;

	if (!dprintf_on)
		return;
	va_start(ptr, fmt);
	vfprintf(stdout, fmt, ptr);
	va_end(ptr);
}
#endif

static void catch(int sig)
{
	printf("uip: exiting on signal %d\n", sig);
	exit(1);
}

static void usage(void)
{
	printf("Usage: uip [-b] [-d] [-D] [-m MTU] [-p ee0|ne0|wd0|3c0|slip|cslip] [-s baud] [-l device] [-n dns_ip] [-q host] [local_ip] [gateway] [netmask]\n");
	exit(1);
}

int main(int argc, char **argv)
{
	int ch;
	int bflag;
	int mtu;
	int fd;
	int ret;
	char *p;
	char *default_ip;
	char *default_gateway;
	char *default_netmask;
	char ipbuf[20];
	char gwbuf[20];
	char maskbuf[20];
	char dnsbuf[20];
	static char *linknames[3] = { "", "slip", "cslip" };

	bflag = 0;
	mtu = 0;
	resolv_enabled = UIP_CONF_RESOLV ? 1 : 0;

	while ((ch = getopt(argc, argv, "bdDm:p:s:l:n:q:")) != -1) {
		switch (ch) {
		case 'b':
			bflag = 1;
			break;
		case 'd':
			dflag++;
			break;
		case 'D':
			dhcp_enabled = 1;
			break;
		case 'm':
			mtu = (int)atol(optarg);
			break;
		case 'p':
			linkprotocol = !strcmp(optarg, "ne0") ? LINK_ETHER :
				!strcmp(optarg, "wd0") ? LINK_ETHER :
				!strcmp(optarg, "3c0") ? LINK_ETHER :
				!strcmp(optarg, "ee0") ? LINK_ETHER :
				!strcmp(optarg, "le0") ? LINK_ETHER :
				!strcmp(optarg, "slip") ? LINK_SLIP :
				!strcmp(optarg, "cslip") ? LINK_CSLIP :
				-1;
			if (linkprotocol < 0)
				usage();
			if (linkprotocol == LINK_ETHER)
				strcpy(&ethdev[5], optarg);
			break;
		case 's':
			baudrate = atol(optarg);
			break;
		case 'l':
			serdev = optarg;
			break;
		case 'n':
			static_dns_ip = in_gethostbyname(optarg);
			if (static_dns_ip == 0) {
				printf("uip: invalid dns server %s\n", optarg);
				exit(1);
			}
			break;
		case 'q':
			strncpy(resolv_query_name, optarg,
				sizeof(resolv_query_name) - 1);
			resolv_query_name[sizeof(resolv_query_name) - 1] = '\0';
			break;
		default:
			usage();
		}
	}

	if (timer_init() < 0)
		exit(1);

	if (dhcp_enabled && linkprotocol != LINK_ETHER) {
		printf("uip: DHCP requires an ethernet link\n");
		exit(1);
	}
#if !UIP_CONF_DHCPC
	if (dhcp_enabled) {
		printf("uip: DHCP support not built\n");
		exit(1);
	}
#endif
#if !UIP_CONF_RESOLV
	if (resolv_query_name[0] != '\0') {
		printf("uip: resolver support not built\n");
		exit(1);
	}
#endif

	default_ip = (p = getenv("HOSTNAME")) ? p :
		(dhcp_enabled ? DEFAULT_DHCP_IP : DEFAULT_IP);
	default_gateway = (p = getenv("GATEWAY")) ? p :
		(dhcp_enabled ? DEFAULT_DHCP_IP : DEFAULT_GATEWAY);
	default_netmask = (p = getenv("NETMASK")) ? p :
		(dhcp_enabled ? DEFAULT_DHCP_IP : DEFAULT_NETMASK);
	p = getenv("UIP_TRACE");
	if (!p)
		p = getenv("KTCP_TRACE");
	if (p)
		tracefd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (resolv_query_name[0] == '\0') {
		p = getenv("UIP_RESOLV_QUERY");
		if (p != NULL) {
			strncpy(resolv_query_name, p,
				sizeof(resolv_query_name) - 1);
			resolv_query_name[sizeof(resolv_query_name) - 1] = '\0';
		}
	}

	local_ip = in_gethostbyname(optind < argc ? argv[optind++] : default_ip);
	gateway_ip = in_gethostbyname(optind < argc ? argv[optind++] : default_gateway);
	netmask_ip = in_gethostbyname(optind < argc ? argv[optind++] : default_netmask);
	MTU = mtu ? mtu : (linkprotocol == LINK_ETHER ? ETH_MTU : SLIP_MTU);

	netdevfd = netdev_init("/dev/netdev");
	if (netdevfd < 0)
		exit(1);

	if (linkprotocol == LINK_ETHER)
		intfd = deveth_init(ethdev);
	else
		intfd = slip_init(serdev, baudrate);
	if (intfd < 0)
		exit(2);

	copy_ip_string(ipbuf, sizeof(ipbuf), local_ip);
	copy_ip_string(gwbuf, sizeof(gwbuf), gateway_ip);
	copy_ip_string(maskbuf, sizeof(maskbuf), netmask_ip);
	copy_ip_string(dnsbuf, sizeof(dnsbuf), static_dns_ip);
	printf("uip: ip %s, gateway %s, netmask %s",
		ipbuf, gwbuf, maskbuf);
	if (static_dns_ip != 0)
		printf(", dns %s", dnsbuf);
	if (dhcp_enabled)
		printf(" (dhcp)");
	printf("\n");
	printf("uip: ");
	if (linkprotocol == LINK_ETHER)
		printf("%s mac %s", ethdev, mac_ntoa(eth_local_addr));
	else
		printf("%s %s baud %lu", linknames[linkprotocol], serdev,
			(unsigned long)baudrate);
	printf(" mtu %u\n", MTU);

	signal(SIGHUP, catch);
	signal(SIGINT, catch);
	signal(SIGTERM, catch);
#if USE_DEBUG_EVENT
	signal(SIGURG, debug_toggle);
#endif

	if (bflag) {
		ret = fork();
		if (ret == -1) {
			printf("uip: Can't fork to become daemon\n");
			exit(1);
		}
		if (ret)
			exit(0);
		close(0);
		fd = open("/dev/console", O_WRONLY);
		dup2(fd, 1);
		dup2(fd, 2);
		setsid();
	}

	uip_stack_init();
	uip_daemon_run();
	return 0;
}
