#ifndef UIP_CONF_H
#define UIP_CONF_H

#include <linuxmt/types.h>

typedef __u8 u8_t;
typedef __u16 u16_t;
typedef unsigned short uip_stats_t;

struct ktcp_slot;
struct kudp_slot;

void uip_appcall(void);

typedef struct ktcp_slot *uip_tcp_appstate_t;
typedef void *uip_udp_appstate_t;

#ifndef UIP_CONF_MAX_CONNECTIONS
#define UIP_CONF_MAX_CONNECTIONS 16
#endif

#ifndef UIP_CONF_MAX_LISTENPORTS
#define UIP_CONF_MAX_LISTENPORTS 10
#endif

#ifndef UIP_CONF_UDP
#define UIP_CONF_UDP 0
#endif

#ifndef UIP_CONF_BROADCAST
#define UIP_CONF_BROADCAST 0
#endif

#ifndef UIP_CONF_UDP_EXTERNAL_CONNS
#define UIP_CONF_UDP_EXTERNAL_CONNS 8
#endif

#ifndef UIP_CONF_DHCPC
#define UIP_CONF_DHCPC 0
#endif

#ifndef UIP_CONF_RESOLV
#define UIP_CONF_RESOLV 0
#endif

#ifndef UIP_CONF_RESOLV_ENTRIES
#define UIP_CONF_RESOLV_ENTRIES 4
#endif

#ifndef UIP_CONF_UDP_CONNS
#define UIP_CONF_UDP_CONNS \
	(UIP_CONF_UDP_EXTERNAL_CONNS + UIP_CONF_DHCPC + UIP_CONF_RESOLV)
#endif

#ifndef UIP_CONF_BUFFER_SIZE
#define UIP_CONF_BUFFER_SIZE 1514
#endif
#define UIP_CONF_BYTE_ORDER 3412
#define UIP_CONF_LLH_LEN 14
#define UIP_CONF_UDP_CHECKSUMS 0
#ifndef UIP_CONF_STATISTICS
#define UIP_CONF_STATISTICS 1
#endif
#define UIP_CONF_LOGGING 0

#define UIP_APPCALL uip_appcall
#define UIP_UDP_APPCALL uip_appcall

#endif
