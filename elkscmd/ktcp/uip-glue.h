#ifndef UIP_GLUE_H
#define UIP_GLUE_H

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <termios.h>
#include <arpa/inet.h>
#include <stdarg.h>

#include "config.h"
#include "timer.h"
#include "ip.h"
#include "tcp.h"
#include "deveth.h"
#include "slip.h"
#include "arp.h"
#include "netconf.h"

#define KTCP_NTOHS(v) \
	((unsigned short)((((unsigned short)(v) & 0x00ffU) << 8) | \
	(((unsigned short)(v) & 0xff00U) >> 8)))
#define KTCP_HTONS(v) KTCP_NTOHS(v)
#define KTCP_NTOHL(v) \
	((unsigned long)((((unsigned long)(v) & 0x000000ffUL) << 24) | \
	(((unsigned long)(v) & 0x0000ff00UL) << 8) | \
	(((unsigned long)(v) & 0x00ff0000UL) >> 8) | \
	(((unsigned long)(v) & 0xff000000UL) >> 24)))
#define KTCP_HTONL(v) KTCP_NTOHL(v)

#undef ntohs
#undef htons
#undef ntohl
#undef htonl

#include "uip/uip.h"
#include "uip/uip_arp.h"
#if UIP_CONF_DHCPC
#include "uip/apps/dhcpc/dhcpc.h"
#endif
#if UIP_CONF_RESOLV
#include "uip/apps/resolv/resolv.h"
#endif
#include <linuxmt/netdev.h>

#define KTCP_DEFAULT_RCVBUF 2048
#define KTCP_MIN_RCVBUF 512
#define KTCP_MAX_RCVBUF 4380
#define KTCP_MIN_TXBUF 1024
#define KTCP_MAX_TXBUF 4096
#define KTCP_MAX_SOCKETS (UIP_CONF_MAX_CONNECTIONS + 6)
#define KTCP_ARP_TIMER_TICKS 20

struct ktcp_slot {
	unsigned char used;
	unsigned char listening;
	unsigned char pending_accept;
	unsigned char connected;
	unsigned char connect_pending;
	unsigned char peer_closed;
	unsigned char disconnect_notified;
	unsigned char close_requested;
	unsigned char abort_requested;
	unsigned char kernel_released;
	unsigned char netconf;
	unsigned char connect_notified;
	unsigned char reuse_addr;
	void *sock;
	void *pending_accept_sock;
	struct ktcp_slot *listener;
	struct uip_conn *uconn;
	ipaddr_t local_addr;
	ipaddr_t remote_addr;
	unsigned short local_port;
	unsigned short remote_port;
	unsigned short wanted_rcvbuf;
	unsigned short rx_cap;
	unsigned short rx_len;
	unsigned short tx_cap;
	unsigned short tx_len;
	unsigned short tx_inflight;
	unsigned char *rx_buf;
	unsigned char *tx_buf;
	timeq_t time_wait_exp;
};

extern ipaddr_t local_ip;
extern ipaddr_t gateway_ip;
extern ipaddr_t netmask_ip;
extern ipaddr_t dns_server_ip;
extern int linkprotocol;
extern unsigned int MTU;
extern int netdevfd;
extern int dflag;
extern struct ktcp_slot ktcp_slots[KTCP_MAX_SOCKETS];
extern unsigned char netdev_sbuf[NETDEV_INBUFFERSIZE];
#if UIP_CONF_DHCPC
extern char dhcpc_appstate_tag;
#endif
#if UIP_CONF_RESOLV
extern char resolv_appstate_tag;
#endif

#if UIP_CONF_UDP
#define KUDP_MAX_SOCKETS (UIP_CONF_UDP_EXTERNAL_CONNS + 4)

struct kudp_slot {
	unsigned char used;
	unsigned char bound;
	unsigned char connected;
	unsigned char tx_use_addr;
	void *sock;
	struct uip_udp_conn *uconn;
	ipaddr_t local_addr;
	ipaddr_t remote_addr;
	ipaddr_t rx_addr;
	ipaddr_t tx_addr;
	unsigned short local_port;
	unsigned short remote_port;
	unsigned short rx_port;
	unsigned short tx_port;
	unsigned short rx_len;
	unsigned short tx_len;
	unsigned char *rx_buf;
	unsigned char *tx_buf;
};

extern struct kudp_slot kudp_slots[KUDP_MAX_SOCKETS];
#endif

void uip_stack_init(void);
void uip_daemon_run(void);
void ktcp_periodic(void);
void ktcp_send_uip_output(int has_link_header);
void ktcp_process_ethernet_frame(const unsigned char *frame, int len);
void ktcp_process_slip_packet(const unsigned char *packet, int len);
int netdev_init(char *fdev);
int netdev_write_msg(const void *buf, unsigned int len);
void netdev_process(void);
void uip_tracef(const char *fmt, ...);
void uip_write_runtime_state(void);

#if UIP_CONF_UDP
int kudp_handle_command(unsigned char cmd);
void kudp_appcall(void);
int kudp_has_pending(void);
void kudp_periodic(void);
#endif

struct ktcp_slot *ktcp_find_sock(void *sock);
struct ktcp_slot *ktcp_find_uconn(struct uip_conn *conn);
struct ktcp_slot *ktcp_alloc_slot(void);
void ktcp_free_slot(struct ktcp_slot *slot);
void ktcp_cleanup_slots(void);
void ktcp_kick_slot(struct ktcp_slot *slot, int force_timer);
void ktcp_notify_disconnect(struct ktcp_slot *slot);
void ktcp_notify_data_avail(struct ktcp_slot *slot);
int ktcp_state_for_slot(struct ktcp_slot *slot);
unsigned int ktcp_retrans_memory(void);
int ktcp_queue_tx(struct ktcp_slot *slot, const unsigned char *data, unsigned int len);
int ktcp_append_rx(struct ktcp_slot *slot, const unsigned char *data, unsigned int len);
unsigned short ktcp_alloc_port(void);
int ktcp_port_in_use(unsigned short port, struct ktcp_slot *skip);
ipaddr_t uip_ipaddr_to_ip(const uip_ipaddr_t addr);
void uip_ipaddr_from_ip(uip_ipaddr_t *addr, ipaddr_t ip);

int ktcp_netconf_response(unsigned char *buf, unsigned int buflen, const struct stat_request_s *sr);
void ktcp_arp_learn(ipaddr_t ip, const eth_addr_t mac);
void ktcp_arp_observe_frame(const unsigned char *frame, int len);

#endif
