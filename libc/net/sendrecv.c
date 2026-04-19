#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

extern int ksendto(int sock, const void *message, size_t length,
	struct socket_sendto_args *args);
extern int krecvfrom(int sock, void *buffer, size_t length,
	struct socket_recvfrom_args *args);

/*
 * ELKS syscalls can only marshal one user data segment per call.
 * Bounce both the payload and sockaddr args through libc-owned memory so
 * the kernel sees a consistent segment for datagram syscalls.
 */
static struct socket_sendto_args sendto_args;
static struct socket_recvfrom_args recvfrom_args;

int sendto(int sock, const void *message, size_t length, unsigned int flags,
	const struct sockaddr *dest_addr, socklen_t dest_len)
{
	void *bounce = NULL;
	const void *payload = message;
	int ret;

	memset(&sendto_args, 0, sizeof(sendto_args));
	sendto_args.flags = flags;
	if (dest_addr && dest_len) {
		if (dest_len > sizeof(sendto_args.addr))
			dest_len = sizeof(sendto_args.addr);
		sendto_args.addrlen = dest_len;
		memcpy(&sendto_args.addr, dest_addr, dest_len);
	}

	if (length) {
		bounce = malloc(length);
		if (!bounce) {
			errno = ENOMEM;
			return -1;
		}
		memcpy(bounce, message, length);
		payload = bounce;
	}

	ret = ksendto(sock, payload, length, &sendto_args);
	free(bounce);
	return ret;
}

int recvfrom(int sock, void *buffer, size_t length, unsigned int flags,
	struct sockaddr *address, socklen_t *address_len)
{
	void *bounce = NULL;
	socklen_t len;
	int ret;

	memset(&recvfrom_args, 0, sizeof(recvfrom_args));
	recvfrom_args.flags = flags;
	if (address && address_len) {
		len = *address_len;
		if (len > sizeof(recvfrom_args.addr))
			len = sizeof(recvfrom_args.addr);
		recvfrom_args.addrlen = len;
	}

	if (length) {
		bounce = malloc(length);
		if (!bounce) {
			errno = ENOMEM;
			return -1;
		}
	}

	ret = krecvfrom(sock, bounce, length, &recvfrom_args);
	if (ret < 0) {
		free(bounce);
		return ret;
	}

	if (ret > 0)
		memcpy(buffer, bounce, ret);

	if (address_len) {
		len = recvfrom_args.addrlen;
		if (address && len) {
			socklen_t copylen = len;

			if (copylen > *address_len)
				copylen = *address_len;
			memcpy(address, &recvfrom_args.addr, copylen);
		}
		*address_len = len;
	}

	free(bounce);
	return ret;
}
