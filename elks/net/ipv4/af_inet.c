/*
 * net/ipv4/af_inet.c
 *
 * TCP/IP stack by Harry Kalogirou
 *
 * (C) 2001 Harry Kalogirou (harkal@rainbow.cs.unipi.gr)
 * 4 Aug 20 Greg Haerr - debugged semaphores and added multiprocess support
 *
 * The kernel side part of the ELKS TCP/IP stack. It uses netdev.c to
 * communicate with the actual TCP/IP stack that resides in user space.
 */

//#define DEBUG 1
#include <linuxmt/config.h>
#include <linuxmt/errno.h>
#include <linuxmt/socket.h>
#include <linuxmt/fs.h>
#include <linuxmt/mm.h>
#include <linuxmt/stat.h>
#include <linuxmt/fcntl.h>
#include <linuxmt/sched.h>
#include <linuxmt/net.h>
#include <linuxmt/in.h>
#include <linuxmt/string.h>
#include <linuxmt/netdev.h>
#include <linuxmt/debug.h>
#include <arch/irq.h>

#include "af_inet.h"

#ifdef CONFIG_INET

extern unsigned char ndin_buf[];
extern sem_t ndbufin_sem, ndbufout_sem;
extern char netdev_inuse;
extern int netdev_inetwrite(void *data, unsigned int len);
extern char *get_ndout_buf(void);

static sem_t rwlock;    /* global inet_read/write semaphore*/
static unsigned char inet_dgram_reply[NETDEV_MAXREAD];

static int inet_sendto(struct socket *sock, void *buff, int len, int nonblock,
                       unsigned int flags, struct sockaddr *uaddr,
                       size_t uaddrlen);
static int inet_recvfrom(struct socket *sock, void *buff, int len, int nonblock,
                         unsigned int flags, struct sockaddr *uaddr,
                         int *uaddrlen);

static int inet_is_dgram(struct socket *sock)
{
    return sock->type == SOCK_DGRAM;
}

static int inet_udp_ready(struct socket *sock)
{
    return sock->localaddr != 0 || sock->localport != 0 ||
        sock->state == SS_CONNECTED;
}

static unsigned char *inet_in_buf(struct socket *sock)
{
    return ndin_buf;
}

static void inet_clear_data_avail(struct socket *sock)
{
    netdev_clear_data_avail();
}

static char *inet_get_out_buf(struct socket *sock)
{
    return get_ndout_buf();
}

static int inet_dev_write(struct socket *sock, void *data, unsigned int len)
{
    return netdev_inetwrite(data, len);
}

static int inet_wait_reply(struct socket *sock)
{
    while (1) {
        prepare_to_wait_interruptible(sock->wait);
        down(&sock->sem);
        if (sock->reply_ready) {
            up(&sock->sem);
            finish_wait(sock->wait);
            return 0;
        }
        up(&sock->sem);
        do_wait();
        finish_wait(sock->wait);
        if (current->signal)
            return -ERESTARTSYS;
    }
}

static void inet_reset_reply(struct socket *sock)
{
    down(&sock->sem);
    sock->reply_ready = 0;
    sock->reply_type = 0;
    sock->reply_addr = 0;
    sock->reply_port = 0;
    sock->retval = 0;
    up(&sock->sem);
}

int inet_process_netdev(register char *buf, int len)
{
    register struct socket *sock;
    register struct ndb_return_data *ret;

    ret = (struct ndb_return_data *)buf;
    sock = ret->sock;
    debug_net("INET(%P) process_netdev sock %x type %d wait %x\n",
        sock, ret->type, sock->wait);

    switch (ret->type) {
    case NDT_CHG_STATE:
        sock->state = (unsigned char)ret->ret_value;
        netdev_clear_data_avail();
        debug_net("INET(%P) chg_state sock %x %d\n", sock, sock->state);
        if (sock->state == SS_DISCONNECTING) {
            sock->flags |= SF_CLOSING;
            wake_up(sock->wait);
        }
        break;

    case NDT_AVAIL_DATA:
        if (sock->type == SOCK_DGRAM) {
            sock->avail_data = ret->ret_value;
            if (ret->size > 0) {
                sock->udp_len = ret->size;
                if (sock->udp_len > NDB_WRITE_MAX)
                    sock->udp_len = NDB_WRITE_MAX;
                memcpy(sock->udp_data, ret->data, (size_t)sock->udp_len);
                sock->udp_addr = ret->addr_ip;
                sock->udp_port = ret->addr_port;
            }
        } else {
            down(&sock->sem);
            sock->avail_data = ret->ret_value;
            debug_net("INET(%P) sock %x avail %u bufin %d\n",
                sock, sock->avail_data, ndbufin_sem);
            up(&sock->sem);
        }
        netdev_clear_data_avail();
        wake_up(sock->wait);
        break;

    case NDT_CONNECT:
        down(&sock->sem);
        sock->flags |= SF_CONNECT;
        sock->retval = ret->ret_value;
        sock->reply_ready = 1;
        sock->reply_type = ret->type;
        sock->reply_addr = ret->addr_ip;
        sock->reply_port = ret->addr_port;
        debug_net("INET(%P) sock %x connect %d bufin %d\n",
            sock, sock->retval, ndbufin_sem);
        up(&sock->sem);
        netdev_clear_data_avail();
        wake_up(sock->wait);
        break;

    case NDT_RETURN:
        down(&sock->sem);
        sock->retval = ret->ret_value;
        sock->reply_ready = 1;
        sock->reply_type = ret->type;
        sock->reply_addr = ret->addr_ip;
        sock->reply_port = ret->addr_port;
        up(&sock->sem);
        if (ret->size == 0)
            netdev_clear_data_avail();
        wake_up(sock->wait);
        break;

    case NDT_ACCEPT:
    case NDT_BIND:
        down(&sock->sem);
        sock->retval = ret->ret_value;
        sock->reply_ready = 1;
        sock->reply_type = ret->type;
        sock->reply_addr = ret->addr_ip;
        sock->reply_port = ret->addr_port;
        if (ret->type == NDT_BIND) {
            sock->localaddr = ret->addr_ip;
            sock->localport = ret->addr_port;
        }
        up(&sock->sem);
        netdev_clear_data_avail();
        wake_up(sock->wait);
        break;
    }

    return 1;
}

static int inet_create(struct socket *sock, int protocol)
{
    debug_net("INET(%P) create sock %x type %d\n", sock, sock->type);

    if (protocol != 0)
        return -EINVAL;

    if (sock->type == SOCK_STREAM || sock->type == SOCK_DGRAM)
        return netdev_inuse ? 0 : -ENETDOWN;

    return -EINVAL;
}

static int inet_dup(struct socket *newsock, struct socket *oldsock)
{
    return inet_create(newsock, 0);
}

static int inet_release(struct socket *sock, struct socket *peer)
{
    int ret;

    debug_net("INET(%P) release sock %x type %d\n", sock, sock->type);
    {
        register struct ndb_release *cmd;

        if (!netdev_inuse)
            return -EINVAL;
        cmd = (struct ndb_release *)get_ndout_buf();
        cmd->cmd = NDC_RELEASE;
        cmd->sock = sock;
        cmd->reset = sock->flags & SF_RST_ON_CLOSE;
        ret = netdev_inetwrite(cmd, sizeof(struct ndb_release));
    }
    return (ret >= 0 ? 0 : ret);
}

static int inet_bind(register struct socket *sock, struct sockaddr *addr,
                     size_t sockaddr_len)
{
    struct sockaddr_in kaddr;
    unsigned char *buf;
    int ret;

    debug_net("INET(%P) bind sock %x type %d\n", sock, sock->type);

    if (!sockaddr_len || sockaddr_len > sizeof(struct sockaddr_in))
        return -EINVAL;

    memcpy_fromfs(&kaddr, addr, sockaddr_len);

    down(&rwlock);
    buf = (unsigned char *)inet_get_out_buf(sock);
    {
        register struct ndb_bind *cmd;

        cmd = (struct ndb_bind *)buf;
        cmd->cmd = NDC_BIND;
        cmd->sock_type = sock->type;
        cmd->sock = sock;
        cmd->reuse_addr = sock->flags & SF_REUSE_ADDR;
        cmd->rcv_bufsiz = sock->rcv_bufsiz;
        memcpy(&cmd->addr, &kaddr, sockaddr_len);
        inet_reset_reply(sock);
        ret = inet_dev_write(sock, cmd, sizeof(struct ndb_bind));
        if (ret < 0) {
            up(&rwlock);
            return ret;
        }
        ret = inet_wait_reply(sock);
        if (ret < 0) {
            up(&rwlock);
            return ret;
        }
        down(&sock->sem);
        ret = sock->retval;
        sock->reply_ready = 0;
        sock->reply_type = 0;
        up(&sock->sem);
    }

    up(&rwlock);

    debug_net("INET(%P) bind returns %d\n", ret);
    return (ret >= 0 ? 0 : ret);
}

static int inet_connect(struct socket *sock, struct sockaddr *uservaddr,
                        size_t sockaddr_len, int flags)
{
    struct sockaddr_in kaddr;
    int ret;

    debug_net("INET(%P) connect sock %x type %d\n", sock, sock->type);

    if (!sockaddr_len || sockaddr_len > sizeof(struct sockaddr_in))
        return -EINVAL;

    memcpy_fromfs(&kaddr, uservaddr, sockaddr_len);
    if (kaddr.sin_family != AF_INET)
        return -EINVAL;

    if (sock->state == SS_CONNECTING)
        return -EINPROGRESS;

    sock->flags &= ~SF_CONNECT;
    {
        register struct ndb_connect *cmd;

        cmd = (struct ndb_connect *)get_ndout_buf();
        cmd->cmd = NDC_CONNECT;
        cmd->sock_type = sock->type;
        cmd->sock = sock;
        memcpy(&cmd->addr, &kaddr, sockaddr_len);
        ret = netdev_inetwrite(cmd, sizeof(struct ndb_connect));
    }
    if (ret < 0)
        return ret;

    prepare_to_wait_interruptible(sock->wait);
    do {
        if (sock->flags & SF_CONNECT)
            break;
        do_wait();
        if (current->signal) {
            finish_wait(sock->wait);
            return -ETIMEDOUT;
        }
    } while (1);
    finish_wait(sock->wait);

    if (sock->retval == 0) {
        sock->state = SS_CONNECTED;
        sock->remaddr = kaddr.sin_addr.s_addr;
        sock->remport = kaddr.sin_port;
    }
    return sock->retval;
}

static int inet_listen(register struct socket *sock, int backlog)
{
    register struct ndb_listen *cmd;
    int ret;

    if (inet_is_dgram(sock))
        return -EINVAL;

    debug("inet_listen(socket : 0x%x)\n", sock);
    down(&rwlock);
    cmd = (struct ndb_listen *)get_ndout_buf();
    cmd->cmd = NDC_LISTEN;
    cmd->sock = sock;
    cmd->backlog = backlog;
    inet_reset_reply(sock);

    ret = netdev_inetwrite(cmd, sizeof(struct ndb_listen));
    if (ret < 0) {
        up(&rwlock);
        return ret;
    }

    ret = inet_wait_reply(sock);
    if (ret < 0) {
        up(&rwlock);
        return ret;
    }

    down(&sock->sem);
    ret = sock->retval;
    sock->reply_ready = 0;
    sock->reply_type = 0;
    up(&sock->sem);
    up(&rwlock);

    return ret;
}

static int inet_accept(register struct socket *sock, struct socket *newsock, int flags)
{
    register struct ndb_accept *cmd;
    int ret;

    if (inet_is_dgram(sock))
        return -EINVAL;

    debug_tune("INET(%P) accept wait sock %x newsock %x\n", sock, newsock);
    cmd = (struct ndb_accept *)get_ndout_buf();
    cmd->cmd = NDC_ACCEPT;
    cmd->sock = sock;
    cmd->newsock = newsock;
    cmd->nonblock = flags & O_NONBLOCK;
    inet_reset_reply(sock);

    ret = netdev_inetwrite(cmd, sizeof(struct ndb_accept));
    if (ret < 0)
        return ret;

    ret = inet_wait_reply(sock);
    if (ret < 0)
        return ret;

    debug_tune("INET(%P) accepted sock %x newsock %x\n", sock, newsock);
    down(&sock->sem);
    newsock->remaddr = sock->reply_addr;
    newsock->remport = sock->reply_port;
    ret = sock->retval;
    sock->reply_ready = 0;
    sock->reply_type = 0;
    up(&sock->sem);
    if (ret >= 0) {
        newsock->state = SS_CONNECTED;
        ret = 0;
    }
    return ret;
}

static int inet_wait_data(struct socket *sock, int nonblock)
{
    while (sock->avail_data == 0) {
        if (!inet_is_dgram(sock) && (sock->flags & SF_CLOSING))
            return 0;
        if (nonblock)
            return -EAGAIN;

        prepare_to_wait_interruptible(sock->wait);
        if (sock->avail_data == 0)
            do_wait();
        finish_wait(sock->wait);
        if (current->signal)
            return -EINTR;
    }
    return 1;
}

static int inet_dgram_read(struct socket *sock, char *ubuf, int size, int nonblock,
                           int want_addr, struct sockaddr *uaddr, int *uaddrlen)
{
    struct ndb_return_data *ret_data;
    struct ndb_recvfrom *cmd;
    struct sockaddr_in addr;
    int addrspace;
    int copylen;
    int ret;

    if (size > NETDEV_MAXREAD)
        size = NETDEV_MAXREAD;
    if (!inet_udp_ready(sock))
        return -EINVAL;

    while (1) {
        down(&rwlock);
        cmd = (struct ndb_recvfrom *)inet_get_out_buf(sock);
        cmd->cmd = NDC_RECVFROM;
        cmd->sock = sock;
        cmd->size = size;
        cmd->nonblock = 1;
        inet_reset_reply(sock);
        ret = inet_dev_write(sock, cmd, sizeof(struct ndb_recvfrom));
        if (ret < 0) {
            up(&rwlock);
            return ret;
        }

        ret = inet_wait_reply(sock);
        if (ret < 0) {
            up(&rwlock);
            return ret;
        }

        down(&sock->sem);
        ret_data = (struct ndb_return_data *)inet_in_buf(sock);
        ret = sock->retval;
        copylen = 0;
        if (ret > 0) {
            copylen = ret_data->size;
            if (copylen > size)
                copylen = size;
            memcpy_tofs(ubuf, ret_data->data, (size_t)copylen);
            if (want_addr && uaddr && uaddrlen) {
                memset(&addr, 0, sizeof(addr));
                addr.sin_family = AF_INET;
                addr.sin_port = ret_data->addr_port;
                addr.sin_addr.s_addr = ret_data->addr_ip;
                addrspace = *uaddrlen;
                if (addrspace > (int)sizeof(addr))
                    addrspace = sizeof(addr);
                if (addrspace > 0)
                    memcpy((char *)uaddr, (char *)&addr, (size_t)addrspace);
                *uaddrlen = sizeof(addr);
            }
        }
        sock->reply_ready = 0;
        sock->reply_type = 0;
        up(&sock->sem);

        if (ret > 0)
            inet_clear_data_avail(sock);
        up(&rwlock);

        if (ret != -EAGAIN)
            return ret > 0 ? copylen : ret;

        if (nonblock)
            return -EAGAIN;
        if (current->signal)
            return -EINTR;

        current->state = TASK_INTERRUPTIBLE;
        current->timeout = jiffies() + (HZ / 10);
        schedule();
    }
}

static int inet_dgram_sendto(struct socket *sock, char *ubuf, int size, int nonblock,
                             struct sockaddr *uaddr, size_t uaddrlen)
{
    struct ndb_sendto *cmd;
    struct sockaddr_in addr;
    int ret;

    if (size <= 0)
        return 0;
    if (size > NDB_WRITE_MAX)
        return -EINVAL;
    if (uaddrlen > sizeof(struct sockaddr_in))
        return -EINVAL;
    if (uaddrlen == 0 && sock->state != SS_CONNECTED)
        return -EINVAL;

    memset(&addr, 0, sizeof(addr));
    if (uaddrlen != 0) {
        memcpy(&addr, uaddr, uaddrlen);
        if (addr.sin_family != AF_INET)
            return -EINVAL;
    }

    down(&rwlock);
    cmd = (struct ndb_sendto *)get_ndout_buf();
    cmd->cmd = NDC_SENDTO;
    cmd->sock = sock;
    cmd->nonblock = nonblock;
    cmd->size = size;
    cmd->addrlen = uaddrlen;
    if (uaddrlen)
        memcpy(&cmd->addr, &addr, uaddrlen);
    else
        memset(&cmd->addr, 0, sizeof(cmd->addr));
    memcpy_fromfs(cmd->data, ubuf, (size_t)cmd->size);
    inet_reset_reply(sock);

    ret = netdev_inetwrite(cmd, sizeof(struct ndb_sendto));
    if (ret < 0) {
        up(&rwlock);
        return ret;
    }

    ret = inet_wait_reply(sock);
    if (ret < 0) {
        up(&rwlock);
        return ret;
    }

    down(&sock->sem);
    ret = sock->retval;
    if (ret >= 0 && sock->reply_port != 0) {
        sock->localaddr = sock->reply_addr;
        sock->localport = sock->reply_port;
    }
    sock->reply_ready = 0;
    sock->reply_type = 0;
    up(&sock->sem);
    up(&rwlock);
    return ret;
}

static int inet_read(struct socket *sock, char *ubuf, int size, int nonblock)
{
    debug_net("INET(%P) read sock %x size %d nonblock %d buf %d\n",
           sock, size, nonblock, *inet_buf_sem(sock));

    if (inet_is_dgram(sock))
        return inet_dgram_read(sock, ubuf, size, nonblock, 0, NULL, NULL);

    {
        struct ndb_return_data *ret_data;
        struct ndb_read *cmd;
        int ret;
        unsigned char *buf;

        if (size > NETDEV_MAXREAD)
            size = NETDEV_MAXREAD;

        ret = inet_wait_data(sock, nonblock);
        if (ret <= 0)
            return ret;

        down(&rwlock);
        buf = (unsigned char *)inet_get_out_buf(sock);
        cmd = (struct ndb_read *)buf;
        cmd->cmd = NDC_READ;
        cmd->sock = sock;
        cmd->size = size;
        cmd->nonblock = nonblock;
        inet_reset_reply(sock);
        ret = netdev_inetwrite(cmd, sizeof(struct ndb_read));
        if (ret < 0) {
            up(&rwlock);
            return ret;
        }

        ret = inet_wait_reply(sock);
        if (ret < 0) {
            up(&rwlock);
            return ret;
        }

        down(&sock->sem);
        ret_data = (struct ndb_return_data *)inet_in_buf(sock);
        ret = sock->retval;
        if (ret > 0) {
            memcpy_tofs(ubuf, &ret_data->data, (size_t)ret_data->size);
            sock->avail_data = ret_data->addr_port;
        }
        sock->reply_ready = 0;
        sock->reply_type = 0;
        up(&sock->sem);

        inet_clear_data_avail(sock);
        up(&rwlock);
        return ret;
    }
}

static int inet_write(register struct socket *sock, char *ubuf, int size,
                      int nonblock)
{
    register int ret;

    debug("INET(%P) write sock %x size %d nonblock %d type %d\n",
          sock, size, nonblock, sock->type);
    if (size <= 0)
        return 0;

    if (!inet_is_dgram(sock) && sock->state == SS_DISCONNECTING)
        return -EPIPE;

    if (inet_is_dgram(sock))
        return inet_dgram_sendto(sock, ubuf, size, nonblock, NULL, 0);

    if (sock->state != SS_CONNECTED)
        return -EINVAL;

    {
        register struct ndb_write *cmd;
        int usize, count;

        count = size;
        while (count) {
            down(&rwlock);
            cmd = (struct ndb_write *)get_ndout_buf();
            cmd->cmd = NDC_WRITE;
            cmd->sock = sock;
            cmd->nonblock = nonblock;
            cmd->size = count > NDB_WRITE_MAX ? NDB_WRITE_MAX : count;

            memcpy_fromfs(cmd->data, ubuf, (size_t) cmd->size);
            usize = cmd->size;
            inet_reset_reply(sock);
            ret = netdev_inetwrite(cmd, sizeof(struct ndb_write));
            if (ret < 0) {
                up(&rwlock);
                return ret;
            }

            ret = inet_wait_reply(sock);
            if (ret < 0) {
                up(&rwlock);
                return ret;
            }

            down(&sock->sem);
            ret = sock->retval;
            sock->reply_ready = 0;
            sock->reply_type = 0;
            up(&sock->sem);
            up(&rwlock);

            if (ret < 0) {
                if (ret == -ERESTARTSYS) {
                    current->state = TASK_INTERRUPTIBLE;
                    current->timeout = jiffies() + (HZ / 10);
                    schedule();
                } else
                    return ret;
            } else {
                count -= usize;
                ubuf += usize;
            }
        }
    }

    return size;
}

static int inet_select(register struct socket *sock, int sel_type)
{
    debug_net("INET(%P) select sock %04x wait %04x type %d avail %u\n",
         sock, sock->wait, sel_type, sock->avail_data);

    if (sel_type == SEL_IN) {
        if (sock->avail_data ||
            (!inet_is_dgram(sock) && sock->state != SS_CONNECTED))
            return 1;
        select_wait(sock->wait);
        return 0;
    } else if (sel_type == SEL_OUT)
        return 1;
    return 0;
}

static int inet_send(struct socket *sock, void *buff, int len, int nonblock,
                     unsigned int flags)
{
    return inet_sendto(sock, buff, len, nonblock, flags, NULL, 0);
}

static int inet_recv(struct socket *sock, void *buff, int len, int nonblock,
                     unsigned int flags)
{
    return inet_recvfrom(sock, buff, len, nonblock, flags, NULL, NULL);
}

static int inet_sendto(struct socket *sock, void *buff, int len, int nonblock,
                       unsigned int flags, struct sockaddr *uaddr, size_t uaddrlen)
{
    if (flags != 0)
        return -EINVAL;

    if (!inet_is_dgram(sock)) {
        if (uaddr != NULL && uaddrlen != 0)
            return -EISCONN;
        return inet_write(sock, buff, len, nonblock);
    }

    return inet_dgram_sendto(sock, (char *)buff, len, nonblock, uaddr, uaddrlen);
}

static int inet_recvfrom(struct socket *sock, void *buff, int len, int nonblock,
                         unsigned int flags, struct sockaddr *uaddr, int *uaddrlen)
{
    if (flags != 0)
        return -EINVAL;

    if (!inet_is_dgram(sock)) {
        if (uaddr != NULL || uaddrlen != NULL)
            return -EISCONN;
        return inet_read(sock, buff, len, nonblock);
    }

    return inet_dgram_read(sock, (char *)buff, len, nonblock, 1, uaddr, uaddrlen);
}

static int inet_getname(struct socket *sock, struct sockaddr *usockaddr,
        int *usockaddr_len, int peer)
{
    struct sockaddr_in sockaddr;

    sockaddr.sin_family = AF_INET;
    if (peer) {
        if (sock->state != SS_CONNECTED)
            return -EINVAL;
        sockaddr.sin_port = sock->remport;
        sockaddr.sin_addr.s_addr = sock->remaddr;
    } else {
        sockaddr.sin_port = sock->localport;
        sockaddr.sin_addr.s_addr = sock->localaddr;
    }

    return move_addr_to_user((char *)&sockaddr, sizeof(struct sockaddr_in),
                                    (char *)usockaddr, usockaddr_len);
}

int not_implemented(void)
{
    debug("not_implemented\n");
    return 0;
}

static struct proto_ops inet_proto_ops = {
    AF_INET,
    inet_create,
    inet_dup,
    inet_release,
    inet_bind,
    inet_connect,
    not_implemented,    /* inet_socketpair */
    inet_accept,
    inet_getname,
    inet_read,
    inet_write,
    inet_select,
    not_implemented,    /* inet_ioctl */
    inet_listen,
    inet_send,
    inet_recv,
    inet_sendto,
    inet_recvfrom,
};

void inet_proto_init(struct net_proto *pro)
{
    sock_register(inet_proto_ops.family, &inet_proto_ops);
}

#endif
