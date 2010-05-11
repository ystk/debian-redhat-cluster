#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "fenced.h"
#include "libfenced.h"

static int do_read(int fd, void *buf, size_t count)
{
	int rv, off = 0;

	while (off < count) {
		rv = read(fd, (char *)buf + off, count - off);
		if (rv == 0)
			return -1;
		if (rv == -1 && errno == EINTR)
			continue;
		if (rv == -1)
			return -1;
		off += rv;
	}
	return 0;
}

static int do_write(int fd, void *buf, size_t count)
{
	int rv, off = 0;

 retry:
	rv = write(fd, (char *)buf + off, count);
	if (rv == -1 && errno == EINTR)
		goto retry;
	if (rv < 0) {
		return rv;
	}

	if (rv != count) {
		count -= rv;
		off += rv;
		goto retry;
	}
	return 0;
}

static int do_connect(const char *sock_path)
{
	struct sockaddr_un sun;
	socklen_t addrlen;
	int rv, fd;

	fd = socket(PF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		goto out;

	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	strcpy(&sun.sun_path[1], sock_path);
	addrlen = sizeof(sa_family_t) + strlen(sun.sun_path+1) + 1;

	rv = connect(fd, (struct sockaddr *) &sun, addrlen);
	if (rv < 0) {
		close(fd);
		fd = rv;
	}
 out:
	return fd;
}

static void init_header(struct fenced_header *h, int cmd, int extra_len)
{
	memset(h, 0, sizeof(struct fenced_header));

	h->magic = FENCED_MAGIC;
	h->version = FENCED_VERSION;
	h->len = sizeof(struct fenced_header) + extra_len;
	h->command = cmd;
}

int fenced_join(void)
{
	struct fenced_header h;
	int fd, rv;

	init_header(&h, FENCED_CMD_JOIN, 0);

	fd = do_connect(FENCED_SOCK_PATH);
	if (fd < 0) {
		rv = fd;
		goto out;
	}

	rv = do_write(fd, &h, sizeof(h));
	close(fd);
 out:
	return rv;
}

int fenced_leave(void)
{
	struct fenced_header h;
	int fd, rv;

	init_header(&h, FENCED_CMD_LEAVE, 0);

	fd = do_connect(FENCED_SOCK_PATH);
	if (fd < 0) {
		rv = fd;
		goto out;
	}

	rv = do_write(fd, &h, sizeof(h));
	close(fd);
 out:
	return rv;
}

int fenced_external(char *name)
{
	char msg[sizeof(struct fenced_header) + MAX_NODENAME_LEN + 1];
	struct fenced_header *hd = (struct fenced_header *)msg;
	int fd, rv;
	int namelen;

	memset(&msg, 0, sizeof(msg));

	init_header(hd, FENCED_CMD_EXTERNAL, MAX_NODENAME_LEN + 1);

	namelen = strlen(name);
	if (namelen > MAX_NODENAME_LEN)
		namelen = MAX_NODENAME_LEN;
	memcpy(msg + sizeof(struct fenced_header), name, namelen);

	fd = do_connect(FENCED_SOCK_PATH);
	if (fd < 0) {
		rv = fd;
		goto out;
	}

	rv = do_write(fd, hd, sizeof(msg));
	close(fd);
 out:
	return rv;
}

int fenced_dump_debug(char *buf)
{
	struct fenced_header h, *rh;
	char *reply;
	int reply_len;
	int fd, rv;

	init_header(&h, FENCED_CMD_DUMP_DEBUG, 0);

	reply_len = sizeof(struct fenced_header) + FENCED_DUMP_SIZE;
	reply = malloc(reply_len);
	if (!reply) {
		rv = -1;
		goto out;
	}
	memset(reply, 0, reply_len);

	fd = do_connect(FENCED_QUERY_SOCK_PATH);
	if (fd < 0) {
		rv = fd;
		goto out;
	}

	rv = do_write(fd, &h, sizeof(h));
	if (rv < 0)
		goto out_close;

	/* won't always get back the full reply_len */
	do_read(fd, reply, reply_len);

	rh = (struct fenced_header *)reply;
	rv = rh->data;
	if (rv < 0)
		goto out_close;

	memcpy(buf, (char *)reply + sizeof(struct fenced_header),
	       FENCED_DUMP_SIZE);
 out_close:
	close(fd);
 out:
	return rv;
}

int fenced_node_info(int nodeid, struct fenced_node *node)
{
	struct fenced_header h, *rh;
	char reply[sizeof(struct fenced_header) + sizeof(struct fenced_node)];
	int fd, rv;

	init_header(&h, FENCED_CMD_NODE_INFO, 0);
	h.data = nodeid;

	memset(reply, 0, sizeof(reply));

	fd = do_connect(FENCED_QUERY_SOCK_PATH);
	if (fd < 0) {
		rv = fd;
		goto out;
	}

	rv = do_write(fd, &h, sizeof(h));
	if (rv < 0)
		goto out_close;

	rv = do_read(fd, reply, sizeof(reply));
	if (rv < 0)
		goto out_close;

	rh = (struct fenced_header *)reply;
	rv = rh->data;
	if (rv < 0)
		goto out_close;

	memcpy(node, (char *)reply + sizeof(struct fenced_header),
	       sizeof(struct fenced_node));
 out_close:
	close(fd);
 out:
	return rv;
}

int fenced_domain_info(struct fenced_domain *domain)
{
	struct fenced_header h, *rh;
	char reply[sizeof(struct fenced_header) + sizeof(struct fenced_domain)];
	int fd, rv;

	init_header(&h, FENCED_CMD_DOMAIN_INFO, 0);

	memset(reply, 0, sizeof(reply));

	fd = do_connect(FENCED_QUERY_SOCK_PATH);
	if (fd < 0) {
		rv = fd;
		goto out;
	}

	rv = do_write(fd, &h, sizeof(h));
	if (rv < 0)
		goto out_close;

	rv = do_read(fd, reply, sizeof(reply));
	if (rv < 0)
		goto out_close;

	rh = (struct fenced_header *)reply;
	rv = rh->data;
	if (rv < 0)
		goto out_close;

	memcpy(domain, (char *)reply + sizeof(struct fenced_header),
	       sizeof(struct fenced_domain));
 out_close:
	close(fd);
 out:
	return rv;
}

int fenced_domain_nodes(int type, int max, int *count, struct fenced_node *nodes)
{
	struct fenced_header h, *rh;
	char *reply;
	int reply_len;
	int fd, rv, result, node_count;

	init_header(&h, FENCED_CMD_DOMAIN_NODES, 0);
	h.option = type;
	h.data = max;

	reply_len = sizeof(struct fenced_header) + (max * sizeof(struct fenced_node));
	reply = malloc(reply_len);
	if (!reply) {
		rv = -1;
		goto out;
	}
	memset(reply, 0, reply_len);

	fd = do_connect(FENCED_QUERY_SOCK_PATH);
	if (fd < 0) {
		rv = fd;
		goto out;
	}

	rv = do_write(fd, &h, sizeof(h));
	if (rv < 0)
		goto out_close;

	/* won't usually get back the full reply_len */
	do_read(fd, reply, reply_len);

	rh = (struct fenced_header *)reply;
	result = rh->data;
	if (result < 0 && result != -E2BIG) {
		rv = result;
		goto out_close;
	}

	if (result == -E2BIG) {
		*count = -E2BIG;
		node_count = max;
	} else {
		*count = result;
		node_count = result;
	}
	rv = 0;

	memcpy(nodes, (char *)reply + sizeof(struct fenced_header),
	       node_count * sizeof(struct fenced_node));
 out_close:
	close(fd);
 out:
	return rv;
}

