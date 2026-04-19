#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define UDPCHECK_TIMEOUT 20
#define UDPCHECK_RETRY_DELAY 1

static void msg(const char *s)
{
	write(2, s, strlen(s));
	write(2, "\n", 1);
	exit(1);
}

static void die_errno(const char *s)
{
	perror(s);
	exit(1);
}

static unsigned long lookup_host(char *host)
{
	unsigned long ip;

	ip = in_gethostbyname(host);
	if (!ip)
		msg("host lookup failed");
	return ip;
}

static void bind_local(int s, unsigned short port)
{
	struct sockaddr_in addr;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = INADDR_ANY;
	if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0)
		msg("bind failed");
}

static void write_ready(char *path)
{
	int fd;

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		die_errno("readyfile");
	if (write(fd, "ready\n", 6) != 6) {
		close(fd);
		die_errno("readyfile");
	}
	close(fd);
	sync();
}

static void set_peer(struct sockaddr_in *addr, char *host, unsigned short port)
{
	memset(addr, 0, sizeof(*addr));
	addr->sin_family = AF_INET;
	addr->sin_port = htons(port);
	addr->sin_addr.s_addr = lookup_host(host);
}

static int recv_retry(int s, char *buf, int buflen, struct sockaddr_in *addr,
	socklen_t *addrlen)
{
	int n;
	int tries;

	for (tries = 0; tries < UDPCHECK_TIMEOUT; tries++) {
		n = recvfrom(s, buf, buflen, 0, (struct sockaddr *)addr, addrlen);
		if (n >= 0)
			return n;
		if (errno != EAGAIN && errno != EINTR)
			die_errno("recvfrom");
		sleep(UDPCHECK_RETRY_DELAY);
	}

	msg("timeout");
	return -1;
}

int main(int argc, char **argv)
{
	int s;
	int n;
	int flags;
	char buf[512];
	struct sockaddr_in addr;
	socklen_t addrlen;
	char *expect;
	size_t expect_len;

	if (argc < 2)
		msg("usage: udpcheck <host> <port> <localport> <message> | udpcheck -l <localport> [message]");

	s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0)
		die_errno("socket");
	flags = fcntl(s, F_GETFL, 0);
	if (flags >= 0)
		fcntl(s, F_SETFL, flags | O_NONBLOCK);

	if (!strcmp(argv[1], "-l")) {
		char *readyfile;

		if (argc != 3 && argc != 4 && argc != 5)
			msg("usage: udpcheck -l <localport> [message] [readyfile]");
		expect = argc == 4 ? argv[3] : NULL;
		if (argc == 5) {
			expect = argv[3];
			readyfile = argv[4];
		} else readyfile = NULL;
		expect_len = expect ? strlen(expect) : 0;
		bind_local(s, (unsigned short)atoi(argv[2]));
		if (readyfile)
			write_ready(readyfile);
		for (;;) {
			addrlen = sizeof(addr);
			n = recv_retry(s, buf, sizeof(buf), &addr, &addrlen);
			write(1, buf, n);
			sync();
			if (!expect || ((size_t)n == expect_len &&
				memcmp(buf, expect, expect_len) == 0))
				break;
		}
		if (sendto(s, buf, n, 0, (struct sockaddr *)&addr, addrlen) != n)
			die_errno("sendto");
		sync();
		close(s);
		_exit(0);
	}

	if (argc != 5)
		msg("usage: udpcheck <host> <port> <localport> <message>");

	bind_local(s, (unsigned short)atoi(argv[3]));
	set_peer(&addr, argv[1], (unsigned short)atoi(argv[2]));
	if (sendto(s, argv[4], strlen(argv[4]), 0,
		(struct sockaddr *)&addr, sizeof(addr)) != (int)strlen(argv[4]))
		die_errno("sendto");
	addrlen = sizeof(addr);
	n = recv_retry(s, buf, sizeof(buf), &addr, &addrlen);
	write(1, buf, n);
	sync();
	close(s);
	_exit(0);
}
