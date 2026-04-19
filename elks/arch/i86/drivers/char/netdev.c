/*
 * netdev driver for ELKS kernel
 *
 * Used by the user-space uIP stack to pass
 * in and out data to the kernel.
 */

#include <linuxmt/config.h>
#include <linuxmt/kernel.h>
#include <linuxmt/sched.h>
#include <linuxmt/errno.h>
#include <linuxmt/major.h>
#include <linuxmt/fcntl.h>
#include <linuxmt/mm.h>
#include <linuxmt/netdev.h>
#include <linuxmt/debug.h>

#include <arch/segment.h>

#ifdef CONFIG_INET

unsigned char ndin_buf[NETDEV_INBUFFERSIZE];
unsigned char ndout_buf[NETDEV_OUTBUFFERSIZE];

sem_t ndbufin_sem, ndbufout_sem;

static unsigned int ndin_tail, ndout_tail;
static struct wait_queue netdevq;

char netdev_inuse;

char *get_ndout_buf(void)
{
    down(&ndbufout_sem);
    return (char *)ndout_buf;
}

static size_t netdev_read(struct inode *inode, struct file *filp, char *data,
                          unsigned int len)
{
    while (ndout_tail == 0) {
        if (filp->f_flags & O_NONBLOCK)
            return -EAGAIN;
        interruptible_sleep_on(&netdevq);
        if (current->signal)
            return -ERESTARTSYS;
    }

    if (len < ndout_tail)
        debug_net("NETDEV(%P) read len too small %u\n", len);
    else
        len = ndout_tail;
    memcpy_tofs(data, ndout_buf, len);
    ndout_tail = 0;
    up(&ndbufout_sem);
    return len;
}

int netdev_inetwrite(void *data, unsigned int len)
{
    if (len > NETDEV_OUTBUFFERSIZE)
        return -EINVAL;

    ndout_tail = len;
    wake_up(&netdevq);
    return 0;
}

void netdev_clear_data_avail(void)
{
    up(&ndbufin_sem);
    wake_up(&netdevq);
}

static size_t netdev_write(struct inode *inode, struct file *filp,
                           char *data, size_t len)
{
    if (len > NETDEV_INBUFFERSIZE)
        return -EINVAL;
    if (len > 0) {
        if ((filp->f_flags & O_NONBLOCK) && ndbufin_sem < 0)
            return -EAGAIN;
        down(&ndbufin_sem);

        ndin_tail = (unsigned int)len;
        memcpy_fromfs(ndin_buf, data, len);

        inet_process_netdev((char *)ndin_buf, len);
    }
    return len;
}

static int netdev_select(struct inode *inode, struct file *filp, int sel_type)
{
    int ret = 0;

    switch (sel_type) {
    case SEL_OUT:
        if (ndbufin_sem >= 0)
            ret = 1;
        else
            select_wait(&netdevq);
        break;
    case SEL_IN:
        if (ndout_tail != 0)
            ret = 1;
        else
            select_wait(&netdevq);
        break;
    }
    return ret;
}

static int netdev_open(struct inode *inode, struct file *file)
{
    if (!suser())
        return -EPERM;
    if (netdev_inuse)
        return -EBUSY;
    ndin_tail = ndout_tail = 0;
    netdev_inuse = 1;
    return 0;
}

static void netdev_release(struct inode *inode, struct file *file)
{
    netdev_inuse = 0;
}

static struct file_operations netdev_fops = {
    NULL,
    netdev_read,
    netdev_write,
    NULL,
    netdev_select,
    NULL,
    netdev_open,
    netdev_release
};

void INITPROC netdev_init(void)
{
    register_chrdev(NETDEV_MAJOR, NET_DEVICE_NAME, &netdev_fops);
    ndbufin_sem = ndbufout_sem = 0;
    netdev_inuse = 0;
}

#endif
