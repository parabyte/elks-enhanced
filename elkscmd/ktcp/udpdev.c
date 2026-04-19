#include "uip-glue.h"

#if UIP_CONF_UDP

#define sbuf netdev_sbuf

static unsigned short next_udp_port = 2047;

static int kudp_ensure_rx_buf(struct kudp_slot *slot)
{
	if (slot->rx_buf)
		return 0;
	slot->rx_buf = malloc(NDB_WRITE_MAX);
	return slot->rx_buf ? 0 : -ENOMEM;
}

static int kudp_ensure_tx_buf(struct kudp_slot *slot)
{
	if (slot->tx_buf)
		return 0;
	slot->tx_buf = malloc(NDB_WRITE_MAX);
	return slot->tx_buf ? 0 : -ENOMEM;
}

static void zero_slot(struct kudp_slot *slot)
{
	memset(slot, 0, sizeof(*slot));
	slot->local_addr = local_ip;
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

static void retval_to_slot(struct kudp_slot *slot, int retval)
{
	struct ndb_return_data return_data;

	return_data.type = NDT_RETURN;
	return_data.ret_value = retval;
	return_data.sock = slot->sock;
	return_data.size = 0;
	return_data.addr_ip = slot->local_addr;
	return_data.addr_port = KTCP_HTONS(slot->local_port);
	netdev_write_msg(&return_data, sizeof(return_data));
}

static void notify_sock(void *sock, int type, int value)
{
	struct ndb_return_data return_data;

	return_data.type = type;
	return_data.ret_value = value;
	return_data.sock = sock;
	return_data.size = 0;
	return_data.addr_ip = 0;
	return_data.addr_port = 0;
	netdev_write_msg(&return_data, sizeof(return_data));
	uip_tracef("udp notify sock=%u type=%d value=%d\n",
		sock != NULL, type, value);
}

static struct kudp_slot *kudp_alloc_slot(void)
{
	int i;

	for (i = 0; i < KUDP_MAX_SOCKETS; i++) {
		if (kudp_slots[i].used)
			continue;
		zero_slot(&kudp_slots[i]);
		kudp_slots[i].used = 1;
		return &kudp_slots[i];
	}
	return NULL;
}

static void kudp_free_slot(struct kudp_slot *slot)
{
	if (!slot || !slot->used)
		return;

	if (slot->uconn) {
		if (slot->uconn->appstate == slot)
			slot->uconn->appstate = NULL;
		uip_udp_remove(slot->uconn);
	}
	free(slot->rx_buf);
	free(slot->tx_buf);
	zero_slot(slot);
}

static struct kudp_slot *kudp_find_sock(void *sock)
{
	int i;

	for (i = 0; i < KUDP_MAX_SOCKETS; i++) {
		if (kudp_slots[i].used && kudp_slots[i].sock == sock)
			return &kudp_slots[i];
	}
	return NULL;
}

static int kudp_port_in_use(unsigned short port)
{
	int i;

	for (i = 0; i < KUDP_MAX_SOCKETS; i++) {
		if (!kudp_slots[i].used)
			continue;
		if (kudp_slots[i].local_port == port)
			return 1;
	}
	return 0;
}

static unsigned short kudp_alloc_port(void)
{
	do {
		if (++next_udp_port < 1024 || next_udp_port >= 32000)
			next_udp_port = 1024;
	} while (kudp_port_in_use(next_udp_port));
	return next_udp_port;
}

static void kudp_set_conn_peer(struct kudp_slot *slot)
{
	uip_ipaddr_t ipaddr;

	if (!slot->uconn)
		return;

	if (slot->connected) {
		uip_ipaddr_from_ip(&ipaddr, slot->remote_addr);
		uip_ipaddr_copy(slot->uconn->ripaddr, ipaddr);
		slot->uconn->rport = KTCP_HTONS(slot->remote_port);
	} else {
		memset(slot->uconn->ripaddr, 0, sizeof(slot->uconn->ripaddr));
		slot->uconn->rport = 0;
	}
}

static int kudp_ensure_conn(struct kudp_slot *slot)
{
	struct uip_udp_conn *conn;
	uip_ipaddr_t ipaddr;

	if (slot->uconn)
		return 0;

	if (slot->connected) {
		uip_ipaddr_from_ip(&ipaddr, slot->remote_addr);
		conn = uip_udp_new(&ipaddr, KTCP_HTONS(slot->remote_port));
	} else {
		conn = uip_udp_new(NULL, 0);
	}
	if (!conn)
		return -ENOMEM;

	uip_udp_bind(conn, KTCP_HTONS(slot->local_port));
	conn->appstate = slot;
	slot->uconn = conn;
	kudp_set_conn_peer(slot);
	return 0;
}

static struct kudp_slot *kudp_prepare_slot(void *sock)
{
	struct kudp_slot *slot;

	slot = kudp_find_sock(sock);
	if (slot)
		return slot;

	slot = kudp_alloc_slot();
	if (!slot)
		return NULL;
	slot->sock = sock;
	slot->local_port = kudp_alloc_port();
	slot->local_addr = local_ip;
	slot->bound = 1;
	return slot;
}

static int kudp_queue_send(struct kudp_slot *slot, const unsigned char *data,
	unsigned int len, ipaddr_t addr, unsigned short port)
{
	uip_ipaddr_t saved_addr;
	uip_ipaddr_t dest_addr;
	u16_t saved_port;

	if (len > NDB_WRITE_MAX)
		return -EINVAL;
	if (slot->tx_len != 0)
		return -EAGAIN;
	if (kudp_ensure_tx_buf(slot) < 0)
		return -ENOMEM;
	if (kudp_ensure_conn(slot) < 0)
		return -ENOMEM;

	uip_tracef("udp queue lp=%u rp=%u len=%u tx=%u\n",
		slot->local_port, port, len, slot->tx_len);

	memcpy(slot->tx_buf, data, len);
	slot->tx_len = len;
	slot->tx_use_addr = 1;
	slot->tx_addr = addr;
	slot->tx_port = port;

	uip_ipaddr_copy(saved_addr, slot->uconn->ripaddr);
	saved_port = slot->uconn->rport;
	uip_ipaddr_from_ip(&dest_addr, addr);
	uip_ipaddr_copy(slot->uconn->ripaddr, dest_addr);
	slot->uconn->rport = KTCP_HTONS(port);
	uip_udp_periodic_conn(slot->uconn);
	if (uip_len > 0)
		ktcp_send_uip_output(0);
	uip_ipaddr_copy(slot->uconn->ripaddr, saved_addr);
	slot->uconn->rport = saved_port;
	slot->tx_use_addr = 0;

	if (slot->tx_len != 0) {
		uip_tracef("udp queue failed lp=%u rp=%u len=%u\n",
			slot->local_port, port, len);
		slot->tx_len = 0;
		return -EIO;
	}

	return len;
}

static void udpdev_bind(void)
{
	struct ndb_bind *db;
	struct kudp_slot *slot;
	struct ndb_bind_ret bind_ret;
	unsigned short port;

	db = (struct ndb_bind *)sbuf;
	uip_tracef("udp bind sock=%u fam=%u port=%u\n",
		db->sock != NULL, db->addr.sin_family,
		KTCP_NTOHS(db->addr.sin_port));
	if (db->sock_type != SOCK_DGRAM || db->addr.sin_family != AF_INET) {
		retval_to_sock(db->sock, -EINVAL);
		return;
	}

	if (kudp_find_sock(db->sock)) {
		retval_to_sock(db->sock, -EINVAL);
		return;
	}

	port = KTCP_NTOHS(db->addr.sin_port);
	if (port == 0)
		port = kudp_alloc_port();
	else if (kudp_port_in_use(port)) {
		retval_to_sock(db->sock, -EADDRINUSE);
		return;
	}

	slot = kudp_alloc_slot();
	if (!slot) {
		retval_to_sock(db->sock, -ENOMEM);
		return;
	}

	slot->sock = db->sock;
	slot->bound = 1;
	slot->local_port = port;
	slot->local_addr = local_ip;
	if (kudp_ensure_conn(slot) < 0) {
		kudp_free_slot(slot);
		retval_to_sock(db->sock, -ENOMEM);
		return;
	}

	bind_ret.type = NDT_BIND;
	bind_ret.ret_value = 0;
	bind_ret.sock = db->sock;
	bind_ret.addr_ip = local_ip;
	bind_ret.addr_port = KTCP_HTONS(port);
	netdev_write_msg(&bind_ret, sizeof(bind_ret));
}

static void udpdev_connect(void)
{
	struct ndb_connect *db;
	struct kudp_slot *slot;
	ipaddr_t addr;

	db = (struct ndb_connect *)sbuf;
	uip_tracef("udp connect sock=%u fam=%u port=%u\n",
		db->sock != NULL, db->addr.sin_family,
		KTCP_NTOHS(db->addr.sin_port));
	if (db->sock_type != SOCK_DGRAM) {
		retval_to_sock(db->sock, -EINVAL);
		return;
	}

	slot = kudp_prepare_slot(db->sock);
	if (!slot) {
		retval_to_sock(db->sock, -ENOMEM);
		return;
	}
	if (slot->connected) {
		retval_to_sock(db->sock, -EISCONN);
		return;
	}

	addr = db->addr.sin_addr.s_addr;
	if (addr == KTCP_HTONL(INADDR_LOOPBACK))
		addr = local_ip;
	slot->remote_addr = addr;
	slot->remote_port = KTCP_NTOHS(db->addr.sin_port);
	if (slot->remote_port == 0) {
		retval_to_sock(db->sock, -EINVAL);
		return;
	}

	slot->connected = 1;
	if (kudp_ensure_conn(slot) < 0) {
		slot->connected = 0;
		retval_to_sock(db->sock, -ENOMEM);
		return;
	}
	kudp_set_conn_peer(slot);
	uip_tracef("udp connect lp=%u rp=%u\n", slot->local_port, slot->remote_port);
	notify_sock(slot->sock, NDT_CONNECT, 0);
}

static void udpdev_release(void)
{
	struct ndb_release *db;
	struct kudp_slot *slot;

	db = (struct ndb_release *)sbuf;
	slot = kudp_find_sock(db->sock);
	if (!slot)
		return;
	kudp_free_slot(slot);
}

static void udpdev_read_common(int want_addr)
{
	struct ndb_read *db;
	struct kudp_slot *slot;
	struct ndb_return_data *ret_data;
	void *sock;
	unsigned int count;

	db = (struct ndb_read *)sbuf;
	sock = db->sock;
	uip_tracef("udp read sock=%u want_addr=%u size=%u rx=%u\n",
		sock != NULL, want_addr != 0, db->size,
		kudp_find_sock(sock) ? kudp_find_sock(sock)->rx_len : 0);
	slot = kudp_find_sock(sock);
	if (!slot) {
		retval_to_sock(sock, -EINVAL);
		return;
	}

	if (slot->rx_len == 0) {
		retval_to_sock(sock, -EAGAIN);
		return;
	}

	count = db->size < slot->rx_len ? db->size : slot->rx_len;
	ret_data = (struct ndb_return_data *)sbuf;
	ret_data->type = NDT_RETURN;
	ret_data->ret_value = count;
	ret_data->sock = sock;
	ret_data->size = count;
	ret_data->addr_ip = want_addr ? slot->rx_addr : 0;
	ret_data->addr_port = want_addr ? KTCP_HTONS(slot->rx_port) : 0;
	memcpy(ret_data->data, slot->rx_buf, count);
	slot->rx_len = 0;
	slot->rx_addr = 0;
	slot->rx_port = 0;
	netdev_write_msg(ret_data, sizeof(*ret_data) + count);
	uip_tracef("udp read reply sock=%u count=%u\n",
		db->sock != NULL, count);
}

static void udpdev_write(void)
{
	struct ndb_write *db;
	struct kudp_slot *slot;
	int ret;

	db = (struct ndb_write *)sbuf;
	uip_tracef("udp write sock=%u size=%u\n",
		db->sock != NULL, db->size);
	slot = kudp_find_sock(db->sock);
	if (!slot || !slot->connected) {
		retval_to_sock(db->sock, -EINVAL);
		return;
	}

	ret = kudp_queue_send(slot, db->data, db->size, slot->remote_addr,
		slot->remote_port);
	if (ret < 0) {
		retval_to_sock(db->sock, ret);
		return;
	}

	retval_to_slot(slot, db->size);
}

static void udpdev_sendto(void)
{
	struct ndb_sendto *db;
	struct kudp_slot *slot;
	ipaddr_t addr;
	unsigned short port;
	int ret;

	db = (struct ndb_sendto *)sbuf;
	uip_tracef("udp sendto sock=%u size=%u addrlen=%u fam=%u port=%u\n",
		db->sock != NULL, db->size, db->addrlen, db->addr.sin_family,
		KTCP_NTOHS(db->addr.sin_port));
	slot = kudp_prepare_slot(db->sock);
	if (!slot) {
		retval_to_sock(db->sock, -ENOMEM);
		return;
	}

	if (db->addrlen != 0) {
		if (db->addrlen > sizeof(db->addr) || db->addr.sin_family != AF_INET) {
			retval_to_sock(db->sock, -EINVAL);
			return;
		}
		addr = db->addr.sin_addr.s_addr;
		if (addr == KTCP_HTONL(INADDR_LOOPBACK))
			addr = local_ip;
		port = KTCP_NTOHS(db->addr.sin_port);
		if (port == 0) {
			retval_to_sock(db->sock, -EINVAL);
			return;
		}
	} else if (slot->connected) {
		addr = slot->remote_addr;
		port = slot->remote_port;
	} else {
		retval_to_sock(db->sock, -EINVAL);
		return;
	}

	ret = kudp_queue_send(slot, db->data, db->size, addr, port);
	if (ret < 0) {
		uip_tracef("udp sendto ret=%d lp=%u rp=%u\n",
			ret, slot->local_port, port);
		retval_to_sock(db->sock, ret);
		return;
	}

	retval_to_slot(slot, db->size);
}

int kudp_handle_command(unsigned char cmd)
{
	switch (cmd) {
	case NDC_BIND:
		if (((struct ndb_bind *)sbuf)->sock_type != SOCK_DGRAM)
			return 0;
		udpdev_bind();
		return 1;
	case NDC_CONNECT:
		if (((struct ndb_connect *)sbuf)->sock_type != SOCK_DGRAM)
			return 0;
		udpdev_connect();
		return 1;
	case NDC_RELEASE:
		if (kudp_find_sock(((struct ndb_release *)sbuf)->sock) == NULL)
			return 0;
		udpdev_release();
		return 1;
	case NDC_READ:
		if (kudp_find_sock(((struct ndb_read *)sbuf)->sock) == NULL)
			return 0;
		udpdev_read_common(0);
		return 1;
	case NDC_RECVFROM:
		if (kudp_find_sock(((struct ndb_recvfrom *)sbuf)->sock) == NULL)
			return 0;
		udpdev_read_common(1);
		return 1;
	case NDC_WRITE:
		if (kudp_find_sock(((struct ndb_write *)sbuf)->sock) == NULL)
			return 0;
		udpdev_write();
		return 1;
	case NDC_SENDTO:
		udpdev_sendto();
		return 1;
	default:
		return 0;
	}
}

void kudp_appcall(void)
{
	struct kudp_slot *slot;
	struct uip_udpip_hdr *udpbuf;
	unsigned int len;

	slot = uip_udp_conn ? (struct kudp_slot *)uip_udp_conn->appstate : NULL;
	if (!slot)
		return;

	slot->remote_addr = uip_ipaddr_to_ip(uip_udp_conn->ripaddr);
	slot->remote_port = KTCP_NTOHS(uip_udp_conn->rport);
	slot->local_port = KTCP_NTOHS(uip_udp_conn->lport);

	if (uip_newdata()) {
		udpbuf = (struct uip_udpip_hdr *)&uip_buf[UIP_LLH_LEN];
		slot->rx_addr = uip_ipaddr_to_ip(udpbuf->srcipaddr);
		slot->rx_port = KTCP_NTOHS(udpbuf->srcport);
		uip_tracef("udp newdata lp=%u rp=%u len=%u\n",
			slot->local_port, slot->rx_port, uip_datalen());
		if (slot->rx_len == 0) {
			len = uip_datalen();
			if (len > NDB_WRITE_MAX)
				len = NDB_WRITE_MAX;
			if (kudp_ensure_rx_buf(slot) < 0) {
				uip_tracef("udp rx alloc failed lp=%u rp=%u\n",
					slot->local_port, slot->rx_port);
				return;
			}
			memcpy(slot->rx_buf, uip_appdata, len);
			slot->rx_len = len;
			uip_tracef("udp queued sock=%u len=%u rp=%u\n",
				slot->sock != NULL, slot->rx_len, slot->rx_port);
		}
	}

	if (uip_poll() && slot->tx_len > 0) {
		uip_tracef("udp poll lp=%u rp=%u len=%u\n",
			slot->local_port, slot->tx_port, slot->tx_len);
		memcpy(uip_appdata, slot->tx_buf, slot->tx_len);
		uip_udp_send(slot->tx_len);
		slot->tx_len = 0;
	}
}

void kudp_periodic(void)
{
}

int kudp_has_pending(void)
{
	return 0;
}

#endif
