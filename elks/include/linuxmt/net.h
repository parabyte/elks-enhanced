#ifndef __LINUXMT_NET_H
#define __LINUXMT_NET_H

#include <linuxmt/config.h>
#include <linuxmt/socket.h>

/* Number of protocols */
#define NPROTO 3

#define SOCK_INODE(S)	((S)->inode)

typedef enum {
    SS_FREE = 0,
    SS_UNCONNECTED,
    SS_CONNECTING,
    SS_CONNECTED,
    SS_DISCONNECTING
} socket_state;

#ifdef __KERNEL__
struct socket {
    unsigned char state;
    unsigned char flags;
    unsigned char type;
    struct wait_queue *wait;
    unsigned int rcv_bufsiz;
    struct proto_ops *ops;
    struct inode *inode;
    struct file *file;

#if defined(CONFIG_UNIX)
    struct socket *conn;
    struct socket *iconn;
    struct socket *next;
    void *data;
#endif

#if defined(CONFIG_INET)
    sem_t sem;			/* one operation at a time per socket */
    int avail_data;		/* data available for reading from ktcp */
    int retval;			/* event return value from ktcp */
    unsigned char reply_ready;	/* synchronous /dev/netdev reply pending */
    unsigned char reply_type;	/* NDT_* code for pending reply */
    __u32 remaddr;		/* all in network byte order */
    __u32 localaddr;
    __u32 reply_addr;
    __u16 remport;
    __u16 localport;
    __u16 reply_port;
    unsigned short udp_len;
    __u32 udp_addr;
    __u16 udp_port;
    unsigned char udp_data[512];
#endif

};

struct proto_ops {
    int family;
    int (*create) ();
    int (*dup) ();
    int (*release) ();
    int (*bind) ();
    int (*connect) ();
    int (*socketpair) ();
    int (*accept) ();
    int (*getname) ();
    int (*read) ();
    int (*write) ();
    int (*select) ();
    int (*ioctl) ();
    int (*listen) ();
    int (*send) ();
    int (*recv) ();
    int (*sendto) ();
    int (*recvfrom) ();
};
#endif

/* careful: option names are close to public SO_ options in socket.h */
#define SF_CLOSING	(1 << 0) /* inet */
#define SF_ACCEPTCON	(1 << 1) /* unix, nano, sockets */
#define SF_WAITDATA	(1 << 2) /* unix, nano */
#define SF_NOSPACE	(1 << 3) /* unix, nano */
#define SF_RST_ON_CLOSE	(1 << 4) /* inet */
#define SF_REUSE_ADDR	(1 << 5) /* inet */
#define SF_CONNECT	(1 << 6) /* inet */

struct net_proto {
    const char *name;		/* Protocol name */
    void (*init_func) ();	/* Bootstrap */
};

#endif
