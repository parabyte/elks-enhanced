/*
 * Minimal ELKS client for the local esd subset daemon.
 *
 * Usage:
 *   esdplay [file] [host] [port]
 * Defaults:
 *   file=stdin, host=127.0.0.1, port=16001
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifndef O_RDONLY
#define O_RDONLY 0
#endif

#define ESD_DEF_HOST "127.0.0.1"
#define ESD_DEF_PORT 16001

static char io_buf[1024];

static int write_full(int fd, const char *buf, int len)
{
	int off = 0;

	while (off < len) {
		int w = write(fd, buf + off, (unsigned)(len - off));
		if (w <= 0)
			return -1;
		off += w;
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *host = ESD_DEF_HOST;
	int port = ESD_DEF_PORT;
	int in = 0;
	int s;
	struct sockaddr_in local;
	struct sockaddr_in addr;
	int n;

	if (argc > 1 && strcmp(argv[1], "-")) {
		in = open(argv[1], O_RDONLY);
		if (in < 0) {
			perror("esdplay: open input");
			return 1;
		}
	}
	if (argc > 2)
		host = argv[2];
	if (argc > 3) {
		long p = atol(argv[3]);
		if (p > 0 && p <= 65535L)
			port = (int)p;
	}

	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) {
		perror("esdplay: socket");
		if (in != 0)
			close(in);
		return 1;
	}
	memset(&addr, 0, sizeof(addr));
	memset(&local, 0, sizeof(local));
	local.sin_family = AF_INET;
	local.sin_port = PORT_ANY;
	local.sin_addr.s_addr = INADDR_ANY;
	if (bind(s, (struct sockaddr *)&local, sizeof(local)) < 0) {
		perror("esdplay: bind");
		close(s);
		if (in != 0)
			close(in);
		return 1;
	}

	addr.sin_family = AF_INET;
	addr.sin_port = htons((unsigned short)port);
	addr.sin_addr.s_addr = in_gethostbyname((char *)host);
	if (addr.sin_addr.s_addr == 0UL) {
		fprintf(stderr, "esdplay: host lookup failed '%s'\n", host);
		close(s);
		if (in != 0)
			close(in);
		return 1;
	}
	if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("esdplay: connect");
		close(s);
		if (in != 0)
			close(in);
		return 1;
	}

	while ((n = read(in, io_buf, sizeof(io_buf))) > 0) {
		if (write_full(s, io_buf, n) < 0) {
			perror("esdplay: write");
			close(s);
			if (in != 0)
				close(in);
			return 1;
		}
	}

	close(s);
	if (in != 0)
		close(in);
	return 0;
}
