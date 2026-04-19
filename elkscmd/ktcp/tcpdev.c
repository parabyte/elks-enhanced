#include "uip-glue.h"

int netdevfd;
unsigned char netdev_sbuf[NETDEV_INBUFFERSIZE];

static unsigned short next_port = 1023;
#define sbuf netdev_sbuf

static void zero_slot(struct ktcp_slot *slot)
{
	memset(slot, 0, sizeof(*slot));
	slot->local_addr = local_ip;
	slot->wanted_rcvbuf = KTCP_DEFAULT_RCVBUF;
}

static int ensure_rx_capacity(struct ktcp_slot *slot, unsigned int need)
{
	unsigned int newcap;
	unsigned char *p;

	if (slot->rx_cap >= need)
		return 0;

	newcap = slot->rx_cap ? slot->rx_cap : slot->wanted_rcvbuf;
	if (newcap < KTCP_MIN_RCVBUF)
		newcap = KTCP_MIN_RCVBUF;
	while (newcap < need && newcap < KTCP_MAX_RCVBUF)
		newcap <<= 1;
	if (newcap < need)
		newcap = need;
	if (newcap > KTCP_MAX_RCVBUF)
		return -1;

	p = realloc(slot->rx_buf, newcap);
	if (!p)
		return -1;
	slot->rx_buf = p;
	slot->rx_cap = newcap;
	return 0;
}

static int ensure_tx_capacity(struct ktcp_slot *slot, unsigned int need)
{
	unsigned int newcap;
	unsigned char *p;

	if (slot->tx_cap >= need)
		return 0;

	newcap = slot->tx_cap ? slot->tx_cap : KTCP_MIN_TXBUF;
	while (newcap < need && newcap < KTCP_MAX_TXBUF)
		newcap <<= 1;
	if (newcap < need)
		newcap = need;
	if (newcap > KTCP_MAX_TXBUF)
		return -1;

	p = realloc(slot->tx_buf, newcap);
	if (!p)
		return -1;
	slot->tx_buf = p;
	slot->tx_cap = newcap;
	return 0;
}

static void tx_acked(struct ktcp_slot *slot)
{
	if (slot->tx_inflight == 0)
		return;

	if (slot->tx_inflight >= slot->tx_len) {
		slot->tx_len = 0;
		slot->tx_inflight = 0;
		return;
	}

	memmove(slot->tx_buf, slot->tx_buf + slot->tx_inflight,
		slot->tx_len - slot->tx_inflight);
	slot->tx_len -= slot->tx_inflight;
	slot->tx_inflight = 0;

	if (slot->kernel_released && slot->tx_len == 0 && slot->tx_buf) {
		free(slot->tx_buf);
		slot->tx_buf = NULL;
		slot->tx_cap = 0;
	}
}

static void discard_released_buffers(struct ktcp_slot *slot)
{
	if (!slot || !slot->kernel_released)
		return;

	if (slot->rx_buf) {
		free(slot->rx_buf);
		slot->rx_buf = NULL;
		slot->rx_cap = 0;
		slot->rx_len = 0;
	}

	if (slot->tx_len == 0 && slot->tx_buf) {
		free(slot->tx_buf);
		slot->tx_buf = NULL;
		slot->tx_cap = 0;
	}
}

int netdev_write_msg(const void *buf, unsigned int len)
{
	fd_set wfds;
	int ret;

	while (1) {
		ret = write(netdevfd, buf, len);
		if (ret == (int)len)
			return 0;
		if (ret < 0 && errno == EINTR)
			continue;
		if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			FD_ZERO(&wfds);
			FD_SET(netdevfd, &wfds);
			if (select(netdevfd + 1, NULL, &wfds, NULL, NULL) < 0) {
				if (errno == EINTR)
					continue;
			}
			continue;
		}
		uip_tracef("netdev write fail len=%u ret=%d errno=%d\n",
			len, ret, errno);
		return -1;
	}
}

static void retval_to_sock(void *sock, int retval)
{
	struct ndb_return_data return_data;

	return_data.type = NDT_RETURN;
	return_data.ret_value = retval;
	return_data.sock = sock;
	return_data.size = 0;
	return_data.addr_ip = 0;
	return_data.addr_port = 0;
	netdev_write_msg(&return_data, sizeof(return_data));
}

static struct ktcp_slot *find_listener_port(unsigned short port)
{
	int i;

	for (i = 0; i < KTCP_MAX_SOCKETS; i++) {
		if (!ktcp_slots[i].used || !ktcp_slots[i].listening)
			continue;
		if (ktcp_slots[i].local_port == port)
			return &ktcp_slots[i];
	}
	return NULL;
}

static struct ktcp_slot *find_pending_child(struct ktcp_slot *listener)
{
	int i;

	for (i = 0; i < KTCP_MAX_SOCKETS; i++) {
		if (!ktcp_slots[i].used || !ktcp_slots[i].pending_accept)
			continue;
		if (ktcp_slots[i].listener == listener && ktcp_slots[i].sock == NULL)
			return &ktcp_slots[i];
	}
	return NULL;
}

static void clear_uconn(struct ktcp_slot *slot)
{
	if (!slot->uconn)
		return;
	if (slot->uconn->appstate == slot)
		slot->uconn->appstate = NULL;
	slot->uconn = NULL;
}

static void assign_accept(struct ktcp_slot *listener, struct ktcp_slot *child,
	void *newsock)
{
	struct ndb_accept_ret accept_ret;

	uip_tracef("accept lp=%u rp=%u newsock=%u rx=%u\n",
		child->local_port, child->remote_port,
		newsock != NULL, child->rx_len);
	child->sock = newsock;
	child->pending_accept = 0;
	child->connected = 1;
	listener->pending_accept_sock = NULL;

	accept_ret.type = NDT_ACCEPT;
	accept_ret.ret_value = 0;
	accept_ret.sock = listener->sock;
	accept_ret.addr_ip = child->remote_addr;
	accept_ret.addr_port = KTCP_HTONS(child->remote_port);
	netdev_write_msg(&accept_ret, sizeof(accept_ret));

	/*
	 * Data or close events can arrive before the listener issues accept().
	 * Once the kernel socket is attached, replay any already-buffered state
	 * so the newly accepted socket does not block waiting for a fresh event.
	 */
	if (child->rx_len > 0)
		ktcp_notify_data_avail(child);
	else if (child->peer_closed)
		ktcp_notify_disconnect(child);
}

static struct ktcp_slot *alloc_incoming_slot(struct uip_conn *conn)
{
	struct ktcp_slot *listener;
	struct ktcp_slot *slot;
	unsigned short port;

	port = KTCP_NTOHS(conn->lport);
	listener = find_listener_port(port);
	if (!listener) {
		uip_tracef("incoming no-listener lp=%u rp=%u\n",
			port, KTCP_NTOHS(conn->rport));
		return NULL;
	}

	slot = ktcp_alloc_slot();
	if (!slot)
		return NULL;

	slot->listener = listener;
	slot->uconn = conn;
	slot->connected = 1;
	slot->pending_accept = 1;
	slot->local_addr = local_ip;
	slot->local_port = port;
	slot->remote_addr = uip_ipaddr_to_ip(conn->ripaddr);
	slot->remote_port = KTCP_NTOHS(conn->rport);
	slot->wanted_rcvbuf = KTCP_DEFAULT_RCVBUF;
	conn->appstate = slot;

	if (listener->pending_accept_sock)
		assign_accept(listener, slot, listener->pending_accept_sock);

	uip_tracef("incoming lp=%u rp=%u pend=%u\n",
		slot->local_port, slot->remote_port, slot->pending_accept);

	return slot;
}

struct ktcp_slot *ktcp_alloc_slot(void)
{
	int i;

	for (i = 0; i < KTCP_MAX_SOCKETS; i++) {
		if (ktcp_slots[i].used)
			continue;
		zero_slot(&ktcp_slots[i]);
		ktcp_slots[i].used = 1;
		return &ktcp_slots[i];
	}
	return NULL;
}

void ktcp_free_slot(struct ktcp_slot *slot)
{
	if (!slot || !slot->used)
		return;

	if (slot->listening)
		uip_unlisten(KTCP_HTONS(slot->local_port));
	clear_uconn(slot);
	free(slot->rx_buf);
	free(slot->tx_buf);
	zero_slot(slot);
}

struct ktcp_slot *ktcp_find_sock(void *sock)
{
	int i;

	for (i = 0; i < KTCP_MAX_SOCKETS; i++) {
		if (ktcp_slots[i].used && ktcp_slots[i].sock == sock)
			return &ktcp_slots[i];
	}
	return NULL;
}

struct ktcp_slot *ktcp_find_uconn(struct uip_conn *conn)
{
	int i;

	for (i = 0; i < KTCP_MAX_SOCKETS; i++) {
		if (ktcp_slots[i].used && ktcp_slots[i].uconn == conn)
			return &ktcp_slots[i];
	}
	return NULL;
}

unsigned short ktcp_alloc_port(void)
{
	do {
		if (++next_port < 1024 || next_port >= 32000)
			next_port = 1024;
	} while (ktcp_port_in_use(next_port, NULL));
	return next_port;
}

int ktcp_port_in_use(unsigned short port, struct ktcp_slot *skip)
{
	int i;

	for (i = 0; i < KTCP_MAX_SOCKETS; i++) {
		if (!ktcp_slots[i].used || &ktcp_slots[i] == skip)
			continue;
		if (ktcp_slots[i].local_port != port)
			continue;
		if (ktcp_slots[i].listening || ktcp_slots[i].connected ||
		    ktcp_slots[i].connect_pending || ktcp_slots[i].peer_closed ||
		    ktcp_slots[i].netconf || ktcp_slots[i].uconn)
			return 1;
	}
	return 0;
}

int ktcp_append_rx(struct ktcp_slot *slot, const unsigned char *data,
	unsigned int len)
{
	if (ensure_rx_capacity(slot, slot->rx_len + len) < 0)
		return -1;
	memcpy(slot->rx_buf + slot->rx_len, data, len);
	slot->rx_len += len;
	return 0;
}

int ktcp_queue_tx(struct ktcp_slot *slot, const unsigned char *data,
	unsigned int len)
{
	if (ensure_tx_capacity(slot, slot->tx_len + len) < 0)
		return -1;
	memcpy(slot->tx_buf + slot->tx_len, data, len);
	slot->tx_len += len;
	return 0;
}

unsigned int ktcp_retrans_memory(void)
{
	int i;
	unsigned int total;

	total = 0;
	for (i = 0; i < KTCP_MAX_SOCKETS; i++) {
		if (!ktcp_slots[i].used)
			continue;
		total += ktcp_slots[i].tx_inflight;
	}
	return total;
}

void notify_sock(void *sock, int type, int value)
{
	struct ndb_return_data return_data;

	uip_tracef("notify type=%d val=%d sock=%u\n",
		type, value, sock != NULL);
	return_data.type = type;
	return_data.ret_value = value;
	return_data.sock = sock;
	return_data.size = 0;
	return_data.addr_ip = 0;
	return_data.addr_port = 0;
	netdev_write_msg(&return_data, sizeof(return_data));
}

void ktcp_notify_data_avail(struct ktcp_slot *slot)
{
	if (!slot || !slot->sock || slot->rx_len == 0)
		return;
	uip_tracef("avail lp=%u rp=%u rx=%u\n",
		slot->local_port, slot->remote_port, slot->rx_len);
	notify_sock(slot->sock, NDT_AVAIL_DATA, slot->rx_len);
}

void ktcp_notify_disconnect(struct ktcp_slot *slot)
{
	if (!slot || !slot->sock || slot->disconnect_notified)
		return;
	slot->disconnect_notified = 1;
	notify_sock(slot->sock, NDT_CHG_STATE, SS_DISCONNECTING);
}

int ktcp_state_for_slot(struct ktcp_slot *slot)
{
	unsigned char flags;

	if (!slot)
		return TS_CLOSED;
	if (slot->listening)
		return TS_LISTEN;
	if (slot->netconf)
		return TS_ESTABLISHED;
	if (slot->connect_pending && !slot->connected)
		return TS_SYN_SENT;
	if (slot->peer_closed && !slot->close_requested)
		return TS_CLOSE_WAIT;
	if (!slot->uconn)
		return slot->connected ? TS_ESTABLISHED : TS_CLOSED;

	flags = slot->uconn->tcpstateflags & UIP_TS_MASK;
	switch (flags) {
	case UIP_SYN_RCVD:
		return TS_SYN_RECEIVED;
	case UIP_SYN_SENT:
		return TS_SYN_SENT;
	case UIP_ESTABLISHED:
		return TS_ESTABLISHED;
	case UIP_FIN_WAIT_1:
		return TS_FIN_WAIT_1;
	case UIP_FIN_WAIT_2:
		return TS_FIN_WAIT_2;
	case UIP_CLOSING:
		return TS_CLOSING;
	case UIP_TIME_WAIT:
		return TS_TIME_WAIT;
	case UIP_LAST_ACK:
		return slot->close_requested ? TS_LAST_ACK : TS_CLOSE_WAIT;
	default:
		return TS_CLOSED;
	}
}

void ktcp_cleanup_slots(void)
{
	int i;

	for (i = 0; i < KTCP_MAX_SOCKETS; i++) {
		if (!ktcp_slots[i].used)
			continue;
		if (ktcp_slots[i].uconn &&
		    (ktcp_slots[i].uconn->tcpstateflags & UIP_TS_MASK) == UIP_CLOSED)
			clear_uconn(&ktcp_slots[i]);
		if (!ktcp_slots[i].uconn && ktcp_slots[i].kernel_released &&
		    ktcp_slots[i].rx_len == 0 && !ktcp_slots[i].listening)
			ktcp_free_slot(&ktcp_slots[i]);
		else if (!ktcp_slots[i].uconn && ktcp_slots[i].pending_accept &&
			 ktcp_slots[i].sock == NULL)
			ktcp_free_slot(&ktcp_slots[i]);
	}
}

void ktcp_kick_slot(struct ktcp_slot *slot, int force_timer)
{
	if (!slot || !slot->uconn)
		return;

	if (force_timer)
		uip_periodic_conn(slot->uconn);
	else
		uip_poll_conn(slot->uconn);

	if (uip_len > 0)
		ktcp_send_uip_output(0);
}

int netdev_init(char *fdev)
{
	int fd;
	int flags;

	fd = open(fdev, O_RDWR);
	if (fd < 0)
		printf("uip: can't open netdev device %s\n", fdev);
	else {
		flags = fcntl(fd, F_GETFL, 0);
		if (flags >= 0 && !(flags & O_NONBLOCK))
			fcntl(fd, F_SETFL, flags | O_NONBLOCK);
		flags = fcntl(fd, F_GETFL, 0);
		uip_tracef("netdev open flags=%d\n", flags);
	}
	return fd;
}

static void tcpdev_bind(void)
{
	struct ndb_bind *db;
	struct ktcp_slot *slot;
	struct ndb_bind_ret bind_ret;
	unsigned short port;

	db = (struct ndb_bind *)sbuf;
	uip_tracef("tcp bind sock=%u fam=%u port=%u reuse=%u\n",
		db->sock != NULL, db->addr.sin_family,
		KTCP_NTOHS(db->addr.sin_port), db->reuse_addr != 0);
	if (db->sock_type != SOCK_STREAM) {
		retval_to_sock(db->sock, -EINVAL);
		return;
	}
	if (db->addr.sin_family != AF_INET) {
		retval_to_sock(db->sock, -EINVAL);
		return;
	}

	if (ktcp_find_sock(db->sock)) {
		retval_to_sock(db->sock, -EINVAL);
		return;
	}

	port = KTCP_NTOHS(db->addr.sin_port);
	if (port == 0)
		port = ktcp_alloc_port();
	else if (ktcp_port_in_use(port, NULL) && !db->reuse_addr) {
		retval_to_sock(db->sock, -EADDRINUSE);
		return;
	}

	slot = ktcp_alloc_slot();
	if (!slot) {
		retval_to_sock(db->sock, -ENOMEM);
		return;
	}

	slot->sock = db->sock;
	slot->reuse_addr = db->reuse_addr;
	slot->wanted_rcvbuf = db->rcv_bufsiz ? db->rcv_bufsiz : KTCP_DEFAULT_RCVBUF;
	slot->local_port = port;
	slot->local_addr = local_ip;

	bind_ret.type = NDT_BIND;
	bind_ret.ret_value = 0;
	bind_ret.sock = db->sock;
	bind_ret.addr_ip = local_ip;
	bind_ret.addr_port = KTCP_HTONS(port);
	netdev_write_msg(&bind_ret, sizeof(bind_ret));
}

static void tcpdev_listen(void)
{
	struct ndb_listen *db;
	struct ktcp_slot *slot;

	db = (struct ndb_listen *)sbuf;
	slot = ktcp_find_sock(db->sock);
	if (!slot || slot->listening || slot->uconn || slot->netconf) {
		uip_tracef("tcp listen fail sock=%u slot=%u lp=%u listen=%u uconn=%u netconf=%u\n",
			db->sock != NULL, slot != NULL,
			slot ? slot->local_port : 0,
			slot ? slot->listening : 0,
			slot ? slot->uconn != NULL : 0,
			slot ? slot->netconf : 0);
		retval_to_sock(db->sock, -EINVAL);
		return;
	}

	uip_tracef("tcp listen lp=%u backlog=%u\n",
		slot->local_port, db->backlog);
	uip_listen(KTCP_HTONS(slot->local_port));
	slot->listening = 1;
	retval_to_sock(db->sock, 0);
}

static void tcpdev_connect(void)
{
	struct ndb_connect *db;
	struct ktcp_slot *slot;
	struct uip_conn *conn;
	uip_ipaddr_t ipaddr;
	ipaddr_t addr;

	db = (struct ndb_connect *)sbuf;
	if (db->sock_type != SOCK_STREAM) {
		notify_sock(db->sock, NDT_CONNECT, -EINVAL);
		return;
	}
	slot = ktcp_find_sock(db->sock);
	if (!slot || slot->listening || slot->uconn || slot->netconf) {
		notify_sock(db->sock, NDT_CONNECT, -EINVAL);
		return;
	}

	addr = db->addr.sin_addr.s_addr;
	if (addr == KTCP_HTONL(INADDR_LOOPBACK))
		addr = local_ip;

	slot->remote_addr = addr;
	slot->remote_port = KTCP_NTOHS(db->addr.sin_port);
	slot->connected = 0;
	slot->connect_pending = 0;
	slot->peer_closed = 0;
	slot->disconnect_notified = 0;

	if (slot->remote_port == NETCONF_PORT && slot->remote_addr == 0) {
		slot->netconf = 1;
		slot->connected = 1;
		notify_sock(slot->sock, NDT_CONNECT, 0);
		return;
	}

	uip_ipaddr_from_ip(&ipaddr, slot->remote_addr);
	conn = uip_connect(&ipaddr, KTCP_HTONS(slot->remote_port));
	if (!conn) {
		notify_sock(db->sock, NDT_CONNECT, -ENOMEM);
		return;
	}

	conn->lport = KTCP_HTONS(slot->local_port);
	conn->appstate = slot;
	slot->uconn = conn;
	slot->connect_pending = 1;
	ktcp_kick_slot(slot, 1);
}

static void tcpdev_accept(void)
{
	struct ndb_accept *db;
	struct ktcp_slot *listener;
	struct ktcp_slot *child;

	db = (struct ndb_accept *)sbuf;
	listener = ktcp_find_sock(db->sock);
	if (!listener || !listener->listening) {
		retval_to_sock(db->sock, -EINVAL);
		return;
	}

	child = find_pending_child(listener);
	if (child) {
		uip_tracef("accept immediate sock=%u\n", db->newsock != NULL);
		assign_accept(listener, child, db->newsock);
		return;
	}

	if (db->nonblock) {
		retval_to_sock(db->sock, -EAGAIN);
		return;
	}

	if (listener->pending_accept_sock) {
		retval_to_sock(db->sock, -EAGAIN);
		return;
	}
	uip_tracef("accept wait sock=%u\n", db->newsock != NULL);
	listener->pending_accept_sock = db->newsock;
}

static void tcpdev_read(void)
{
	struct ndb_read *db;
	struct ktcp_slot *slot;
	struct ndb_return_data *ret_data;
	unsigned int count;
	void *sock;

	db = (struct ndb_read *)sbuf;
	sock = db->sock;
	slot = ktcp_find_sock(sock);
	uip_tracef("readcmd sock=%u slot=%u lp=%u rp=%u rx=%u nb=%u\n",
		sock != NULL, slot != NULL,
		slot ? slot->local_port : 0, slot ? slot->remote_port : 0,
		slot ? slot->rx_len : 0, db->nonblock != 0);
	if (!slot) {
		retval_to_sock(sock, -EINVAL);
		return;
	}

	if (slot->rx_len == 0) {
		if (slot->peer_closed)
			retval_to_sock(sock, -EPIPE);
		else if (db->nonblock)
			retval_to_sock(sock, -EAGAIN);
		else
			retval_to_sock(sock, -EINTR);
		return;
	}

	count = db->size < slot->rx_len ? db->size : slot->rx_len;
	uip_tracef("readret lp=%u rp=%u count=%u data=%02x%02x%02x%02x\n",
		slot->local_port, slot->remote_port, count,
		count > 0 ? slot->rx_buf[0] : 0,
		count > 1 ? slot->rx_buf[1] : 0,
		count > 2 ? slot->rx_buf[2] : 0,
		count > 3 ? slot->rx_buf[3] : 0);
	ret_data = (struct ndb_return_data *)sbuf;
	ret_data->type = NDT_RETURN;
	ret_data->ret_value = count;
	ret_data->sock = sock;
	ret_data->size = count;
	ret_data->addr_ip = 0;
	memcpy(ret_data->data, slot->rx_buf, count);
	if (count < slot->rx_len)
		memmove(slot->rx_buf, slot->rx_buf + count, slot->rx_len - count);
	slot->rx_len -= count;
	/*
	 * For stream reads, return the remaining buffered byte count in
	 * addr_port so the kernel can preserve read readiness without
	 * racing an out-of-band NDT_AVAIL_DATA notification.
	 */
	ret_data->addr_port = slot->rx_len;
	netdev_write_msg(ret_data, sizeof(*ret_data) + count);

	if (slot->rx_len > 0)
		ktcp_notify_data_avail(slot);
	else if (slot->peer_closed)
		ktcp_notify_disconnect(slot);
}

static void tcpdev_write(void)
{
	struct ndb_write *db;
	struct ktcp_slot *slot;

	db = (struct ndb_write *)sbuf;
	slot = ktcp_find_sock(db->sock);
	uip_tracef("writecmd sock=%u slot=%u lp=%u rp=%u size=%u\n",
		db->sock != NULL, slot != NULL,
		slot ? slot->local_port : 0, slot ? slot->remote_port : 0,
		db->size);
	if (!slot) {
		retval_to_sock(db->sock, -EINVAL);
		return;
	}

	if (slot->netconf) {
		struct stat_request_s *sr;
		int ret;

		if (db->size != sizeof(*sr)) {
			retval_to_sock(db->sock, -EINVAL);
			return;
		}
		sr = (struct stat_request_s *)db->data;
		slot->rx_len = 0;
		if (ensure_rx_capacity(slot, sizeof(struct packet_stats_s)) < 0) {
			retval_to_sock(db->sock, -ENOMEM);
			return;
		}
		ret = ktcp_netconf_response(slot->rx_buf, slot->rx_cap, sr);
		if (ret < 0) {
			retval_to_sock(db->sock, -EINVAL);
			return;
		}
		slot->rx_len = ret;
		slot->disconnect_notified = 0;
		ktcp_notify_data_avail(slot);
		retval_to_sock(db->sock, db->size);
		return;
	}

	if (!slot->connected || slot->peer_closed || !slot->uconn) {
		retval_to_sock(db->sock, -EPIPE);
		return;
	}

	if (ktcp_queue_tx(slot, db->data, db->size) < 0) {
		uip_tracef("write retry lp=%u rp=%u tx=%u inflight=%u cap=%u\n",
			slot->local_port, slot->remote_port, slot->tx_len,
			slot->tx_inflight, slot->tx_cap);
		retval_to_sock(db->sock, -ERESTARTSYS);
		return;
	}

	ktcp_kick_slot(slot, 0);
	retval_to_sock(db->sock, db->size);
}

static void release_pending_children(struct ktcp_slot *listener)
{
	int i;

	for (i = 0; i < KTCP_MAX_SOCKETS; i++) {
		if (!ktcp_slots[i].used || ktcp_slots[i].listener != listener)
			continue;
		if (ktcp_slots[i].sock != NULL)
			continue;
		if (ktcp_slots[i].uconn) {
			ktcp_slots[i].abort_requested = 1;
			ktcp_kick_slot(&ktcp_slots[i], 0);
		}
		if (!ktcp_slots[i].uconn)
			ktcp_free_slot(&ktcp_slots[i]);
	}
}

static void tcpdev_release(void)
{
	struct ndb_release *db;
	struct ktcp_slot *slot;

	db = (struct ndb_release *)sbuf;
	slot = ktcp_find_sock(db->sock);
	uip_tracef("release cmd sock=%u slot=%u lp=%u rp=%u listen=%u uconn=%u netconf=%u\n",
		db->sock != NULL, slot != NULL,
		slot ? slot->local_port : 0, slot ? slot->remote_port : 0,
		slot ? slot->listening : 0, slot ? slot->uconn != NULL : 0,
		slot ? slot->netconf : 0);
	if (!slot)
		return;

	if (slot->listening) {
		release_pending_children(slot);
		ktcp_free_slot(slot);
		return;
	}

	if (!slot->uconn || slot->netconf) {
		ktcp_free_slot(slot);
		return;
	}

	slot->kernel_released = 1;
	slot->sock = NULL;
	if (db->reset) {
		uip_tracef("release reset lp=%u rp=%u\n",
			slot->local_port, slot->remote_port);
		slot->abort_requested = 1;
	} else {
		uip_tracef("release close lp=%u rp=%u\n",
			slot->local_port, slot->remote_port);
		slot->close_requested = 1;
	}
	discard_released_buffers(slot);

	if (slot->peer_closed && slot->rx_len == 0 && !slot->uconn) {
		ktcp_free_slot(slot);
		return;
	}
	ktcp_kick_slot(slot, 0);
}

void netdev_process(void)
{
	int len;

	len = read(netdevfd, sbuf, sizeof(netdev_sbuf));
	if (len <= 0)
		return;

	uip_tracef("netdev cmd=%u len=%d\n", sbuf[0], len);

#if UIP_CONF_UDP
	if (kudp_handle_command(sbuf[0]))
		return;
#endif

	switch (sbuf[0]) {
	case NDC_BIND:
		tcpdev_bind();
		break;
	case NDC_ACCEPT:
		tcpdev_accept();
		break;
	case NDC_CONNECT:
		tcpdev_connect();
		break;
	case NDC_LISTEN:
		tcpdev_listen();
		break;
	case NDC_RELEASE:
		tcpdev_release();
		break;
	case NDC_READ:
		tcpdev_read();
		break;
	case NDC_WRITE:
		tcpdev_write();
		break;
	default:
		break;
	}
}

void uip_appcall(void)
{
	struct ktcp_slot *slot;
	unsigned char flags;
	unsigned int sendlen;
	ipaddr_t remote_addr;
	unsigned short remote_port;
	unsigned short local_port;

#if UIP_CONF_UDP
	if (uip_udpconnection()) {
#if UIP_CONF_DHCPC
		if (uip_udp_conn != NULL &&
		    uip_udp_conn->appstate == &dhcpc_appstate_tag) {
			dhcpc_appcall();
			return;
		}
#endif
#if UIP_CONF_RESOLV
		if (uip_udp_conn != NULL &&
		    uip_udp_conn->appstate == &resolv_appstate_tag) {
			resolv_appcall();
			return;
		}
#endif
		kudp_appcall();
		return;
	}
#endif

	slot = ktcp_find_uconn(uip_conn);
	remote_addr = uip_ipaddr_to_ip(uip_conn->ripaddr);
	remote_port = KTCP_NTOHS(uip_conn->rport);
	local_port = KTCP_NTOHS(uip_conn->lport);
	if (slot && uip_connected() &&
	    (slot->kernel_released || slot->local_port != local_port ||
	     slot->remote_port != remote_port ||
	     slot->remote_addr != remote_addr)) {
		uip_tracef("rebind lp=%u oldrp=%u newrp=%u\n",
			slot->local_port, slot->remote_port, remote_port);
		clear_uconn(slot);
		if (slot->kernel_released && slot->rx_len == 0 && !slot->listening)
			ktcp_free_slot(slot);
		slot = NULL;
	}
	if (!slot && uip_connected())
		slot = alloc_incoming_slot(uip_conn);
	if (!slot)
		return;

	uip_tracef("app lp=%u rp=%u c=%u n=%u a=%u x=%u p=%u sock=%u rx=%u tx=%u\n",
		KTCP_NTOHS(uip_conn->lport), KTCP_NTOHS(uip_conn->rport),
		uip_connected() != 0, uip_newdata() != 0, uip_acked() != 0,
		uip_rexmit() != 0, uip_poll() != 0, slot->sock != NULL,
		slot->rx_len, slot->tx_len);

	slot->remote_addr = remote_addr;
	slot->remote_port = remote_port;

	if (uip_connected()) {
		slot->connected = 1;
		if (slot->connect_pending) {
			slot->connect_pending = 0;
			notify_sock(slot->sock, NDT_CONNECT, 0);
		}
		if (slot->listener && slot->listener->pending_accept_sock &&
		    slot->pending_accept)
			assign_accept(slot->listener, slot,
				slot->listener->pending_accept_sock);
	}

	if (uip_acked())
		tx_acked(slot);

	if (uip_newdata()) {
		uip_tracef("newdata lp=%u rp=%u len=%u sock=%u pend=%u\n",
			slot->local_port, slot->remote_port, uip_datalen(),
			slot->sock != NULL, slot->pending_accept);
		if (ktcp_append_rx(slot, uip_appdata, uip_datalen()) == 0) {
			slot->disconnect_notified = 0;
			ktcp_notify_data_avail(slot);
		}
	}

	if (uip_aborted()) {
		if (slot->connect_pending && slot->sock) {
			slot->connect_pending = 0;
			notify_sock(slot->sock, NDT_CONNECT, -ECONNREFUSED);
		}
		slot->peer_closed = 1;
		slot->time_wait_exp = Now;
		clear_uconn(slot);
		discard_released_buffers(slot);
		if (slot->rx_len == 0)
			ktcp_notify_disconnect(slot);
		return;
	}

	if (uip_timedout()) {
		if (slot->connect_pending && slot->sock) {
			slot->connect_pending = 0;
			notify_sock(slot->sock, NDT_CONNECT, -ETIMEDOUT);
		}
		slot->peer_closed = 1;
		slot->time_wait_exp = Now;
		clear_uconn(slot);
		discard_released_buffers(slot);
		if (slot->rx_len == 0)
			ktcp_notify_disconnect(slot);
		return;
	}

	if (uip_closed()) {
		slot->peer_closed = 1;
		slot->time_wait_exp = Now + (timeq_t)UIP_TIME_WAIT_TIMEOUT * 8;
		flags = uip_conn->tcpstateflags & UIP_TS_MASK;
		if (flags == UIP_CLOSED)
			clear_uconn(slot);
		discard_released_buffers(slot);
		if (slot->rx_len == 0)
			ktcp_notify_disconnect(slot);
	}

	if (slot->abort_requested) {
		slot->abort_requested = 0;
		slot->time_wait_exp = Now;
		uip_abort();
		return;
	}

	if (slot->close_requested && !slot->peer_closed &&
	    slot->tx_len == 0 && slot->tx_inflight == 0) {
		slot->close_requested = 0;
		slot->time_wait_exp = Now + (timeq_t)UIP_TIME_WAIT_TIMEOUT * 8;
		uip_close();
		return;
	}

	if (slot->peer_closed)
		return;

	if (uip_rexmit()) {
		if (slot->tx_inflight > 0)
			uip_send(slot->tx_buf, slot->tx_inflight);
		return;
	}

	if (slot->tx_inflight == 0 &&
	    (uip_poll() || uip_connected() || uip_acked()) &&
	    slot->tx_len > 0) {
		sendlen = slot->tx_len;
		if (sendlen > uip_mss())
			sendlen = uip_mss();
		slot->tx_inflight = sendlen;
		uip_send(slot->tx_buf, sendlen);
	}
}
