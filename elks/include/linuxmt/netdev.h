#ifndef __LINUXMT_NETDEV_H
#define __LINUXMT_NETDEV_H

#include <linuxmt/in.h>
#include <linuxmt/net.h>

#define NET_DEVICE_NAME "netdev"

/* Keep this aligned with the userspace uIP bridge payload limit. */
#define NDB_WRITE_MAX 512

#define NETDEV_INBUFFERSIZE 1500

/* outgoing ops */
#define NDC_BIND 1
#define NDC_ACCEPT 2
#define NDC_CONNECT 3
#define NDC_LISTEN 4
#define NDC_RELEASE 5
#define NDC_READ 8
#define NDC_WRITE 9
#define NDC_SENDTO 10
#define NDC_RECVFROM 11

struct ndb_release {
    unsigned char cmd;
    struct socket *sock;
    int reset;
};

struct ndb_accept {
    unsigned char cmd;
    struct socket *sock;
    struct socket *newsock;
    int nonblock;
};

struct ndb_listen {
    unsigned char cmd;
    struct socket *sock;
    int backlog;
};

struct ndb_bind {
    unsigned char cmd;
    unsigned char sock_type;
    struct socket *sock;
    int reuse_addr;
    int rcv_bufsiz;
    struct sockaddr_in addr;
};

struct ndb_connect {
    unsigned char cmd;
    unsigned char sock_type;
    struct socket *sock;
    struct sockaddr_in addr;
};

struct ndb_read {
    unsigned char cmd;
    struct socket *sock;
    int size;
    int nonblock;
};

struct ndb_write {
    unsigned char cmd;
    struct socket *sock;
    int size;
    int nonblock;
    unsigned char data[NDB_WRITE_MAX];
};

struct ndb_sendto {
    unsigned char cmd;
    struct socket *sock;
    int size;
    int nonblock;
    int addrlen;
    struct sockaddr_in addr;
    unsigned char data[NDB_WRITE_MAX];
};

struct ndb_recvfrom {
    unsigned char cmd;
    struct socket *sock;
    int size;
    int nonblock;
};

/* incoming (uip to kernel) ops */
#define NDT_RETURN 1
#define NDT_CHG_STATE 2
#define NDT_AVAIL_DATA 3
#define NDT_ACCEPT 4
#define NDT_BIND 5
#define NDT_CONNECT 6

struct ndb_return_data {
    char type;
    int ret_value;
    struct socket *sock;
    int size;
    __u32 addr_ip;
    __u16 addr_port;
    unsigned char data[];
};

struct ndb_accept_ret {
    char type;
    int ret_value;
    struct socket *sock;
    __u32 addr_ip;
    __u16 addr_port;
};

struct ndb_bind_ret {
    char type;
    int ret_value;
    struct socket *sock;
    __u32 addr_ip;
    __u16 addr_port;
};

#define NETDEV_OUTBUFFERSIZE (sizeof(struct ndb_sendto))
#define NETDEV_MAXREAD (NETDEV_INBUFFERSIZE - sizeof(struct ndb_return_data))

extern void netdev_clear_data_avail(void);
extern int inet_process_netdev(char *buf, int len);

#endif
