#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <ctype.h>
#include "command_table.h"

#define MAX_CLIENTS 1024
#define PORT 6379
#define QUERYBUF_LEN 4096
#define MAX_ARGS 256
#define TABLE_SIZE 1024

static client *clients[MAX_CLIENTS];

static void set_nonblocking(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags != -1)
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int inline_parser(client *c)
{
    if (c->qb_pos >= c->qb_len)
        return 0;

    char *newline = memchr(
        c->querybuf + c->qb_pos,
        '\n',
        c->qb_len - c->qb_pos);

    if (!newline)
        return 0;

    size_t linelen = newline - (c->querybuf + c->qb_pos) + 1;

    char line[QUERYBUF_LEN];
    memcpy(line, c->querybuf + c->qb_pos, linelen);
    line[linelen] = '\0';

    /* strip \n */
    if (linelen >= 1 && line[linelen - 1] == '\n')
        line[linelen - 1] = '\0';

    /* strip optional \r */
    if (linelen >= 2 && line[linelen - 2] == '\r')
        line[linelen - 2] = '\0';

    char *p = line;
    while (*p && isspace((unsigned char)*p))
        p++;

    int argc = 0;

    while (*p && argc < MAX_ARGS)
    {
        char *start = p;
        while (*p && !isspace((unsigned char)*p))
            p++;

        size_t len = p - start;

        char *arg = malloc(len + 1);
        memcpy(arg, start, len);
        arg[len] = '\0';

        c->argv[argc++] = arg;

        while (*p && isspace((unsigned char)*p))
            p++;
    }

    c->argc = argc;
    c->multibulklen = argc;

    c->qb_pos += linelen;

    return 1;
}
static int parse_multibulk_length(client *c)
{
	// Need at least one byte '*' and a newline later
	if (c->qb_pos >= c->qb_len)
		return 0;
	if (c->querybuf[c->qb_pos] != '*')
		return inline_parser(c);

	// Find '\n' in the available bytes to know the end of line
	char *newline = memchr(c->querybuf + c->qb_pos, '\n', c->qb_len - c->qb_pos);
	if (!newline || newline[-1] != '\r')
		return 0;

	char tmp[32];
	size_t linelen = newline - (c->querybuf + c->qb_pos) + 1;
	if (linelen >= sizeof(tmp))
		return 0;
	memcpy(tmp, c->querybuf + c->qb_pos, linelen);
	tmp[linelen] = '\0';

	// parse number after '*'
	char *endptr;
	long num = strtol(tmp + 1, &endptr, 10);
	if (endptr == tmp + 1)
		return 0; // failed parse

	c->multibulklen = num;
	c->qb_pos += linelen;
	return 1;
}

static int parse_bulk_argument(client *c)
{
	// parse header $len\r\n if needed
	if (c->bulklen == -1)
	{
		if (c->qb_pos >= c->qb_len)
			return 0;
		if (c->querybuf[c->qb_pos] != '$')
			return 0;

		char *newline = memchr(c->querybuf + c->qb_pos, '\n', c->qb_len - c->qb_pos);
		if (!newline || newline[-1] != '\r')
			return 0;
		// read line into tmp
		char tmp[64];
		size_t linelen = newline - (c->querybuf + c->qb_pos) + 1;
		if (linelen >= sizeof(tmp))
			return 0;
		memcpy(tmp, c->querybuf + c->qb_pos, linelen);
		tmp[linelen] = '\0';

		char *endptr;
		long len = strtol(tmp + 1, &endptr, 10);
		if (endptr == tmp + 1)
			return 0;

		c->bulklen = len;
		c->qb_pos += linelen;
	}

	// ensure we have <data>\r\n available
	if ((long)(c->qb_len - c->qb_pos) < c->bulklen + 2)
		return 0;

	// allocate and copy argument
	if (c->argc >= MAX_ARGS)
		return 0;
	char *arg = malloc(c->bulklen + 1);
	memcpy(arg, c->querybuf + c->qb_pos, c->bulklen);
	arg[c->bulklen] = '\0';
	c->argv[c->argc++] = arg;

	c->qb_pos += c->bulklen + 2; // skip data + \r\n
	c->bulklen = -1;
	return 1;
}

// static void send_bulk_reply(int fd, const char *s)
// {
// char hdr[64];
// int n = snprintf(hdr, sizeof(hdr), "$%zu\r\n", strlen(s));
// write header, body, trailing CRLF
// write(fd, hdr, n);
// write(fd, s, strlen(s));
// write(fd, "\r\n", 2);
// }

static void process_command(client *c)
{
	if (c->argc == 0)
	{
		write(c->client_fd, "-ERR unknown command\r\n", 22);
		return;
	}
	redisCommand *cmd = lookupCommand(c->argv[0]);
	if (!cmd)
	{
		write(c->client_fd, "-ERR unknown command\r\n", 22);
		return;
	}

	if (cmd->arity > 0 && c->argc != cmd->arity)
	{
		write(c->client_fd, "-ERR wrong number of command\r\n", 29);
		return;
	}
	printf("processing command");
	cmd->proc(c);
}

static void reset_client_state(client *c)
{
	// free parsed argv
	for (int i = 0; i < c->argc; ++i)
	{
		free(c->argv[i]);
		c->argv[i] = NULL;
	}
	c->argc = 0;
	c->multibulklen = -1;
	c->bulklen = -1;

	// if there are leftover bytes after qb_pos, move them to front
	if (c->qb_pos < c->qb_len)
	{
		size_t left = c->qb_len - c->qb_pos;
		memmove(c->querybuf, c->querybuf + c->qb_pos, left);
		c->qb_len = left;
	}
	else
	{
		c->qb_len = 0;
	}
	c->qb_pos = 0;
}

static client *create_client(int fd)
{
	client *c = calloc(1, sizeof(client));
	if (!c)
		return NULL;
	c->client_fd = fd;
	c->qb_len = 0;
	c->qb_pos = 0;
	c->multibulklen = -1;
	c->bulklen = -1;
	c->argc = 0;
	if (fd < MAX_CLIENTS)
		clients[fd] = c;
	return c;
}

static void destroy_client(client *c)
{
	if (!c)
		return;
	if (c->client_fd >= 0 && c->client_fd < MAX_CLIENTS)
		clients[c->client_fd] = NULL;
	for (int i = 0; i < c->argc; ++i)
		free(c->argv[i]);
	free(c);
}

static void handle_query_from_client(client *c)
{
	printf(
		"STATE: qb_len=%zu qb_pos=%zu argc=%d multibulk=%ld bulklen=%ld\n",
		c->qb_len, c->qb_pos, c->argc, c->multibulklen, c->bulklen);
	// Attempt to parse as many complete commands as possible
	while (1)
	{
		if (c->qb_pos >= c->qb_len)
			break;

		if (c->multibulklen == -1)
		{
			if (!parse_multibulk_length(c))
				break;
			// handle special case: *0 or negative? we assume >=1 here
		}

		// parse arguments
		while (c->argc < c->multibulklen)
		{
			if (!parse_bulk_argument(c))
				break;
		}

		if (c->argc == c->multibulklen)
		{
			// we have a full command
			process_command(c);
			reset_client_state(c);
			// continue loop and try parsing next command if any bytes left
		}
		else
		{
			// incomplete arguments; wait for more data
			break;
		}
	}
}

// C instance works as the client, and we pass the epfd to handle deletion of client
static void handle_client(client *c, int epfd)
{
	int client_fd = c->client_fd;
	if (!c)
	{
		close(client_fd);
		return;
	}

	while (1)
	{
		ssize_t byte_read = read(client_fd, c->querybuf + c->qb_len, sizeof(c->querybuf) - c->qb_len);
		if (byte_read > 0)
		{
			printf("Client sends %d bytes to read\n", byte_read);
			c->qb_len += (size_t)byte_read;
			handle_query_from_client(c);
			continue;
		}
		else if (byte_read == 0)
		{
			// remote closed
			printf("Client %d disconnected\n", client_fd);
			break;
		}
		else
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK)
			{
				// no more data now — return to epoll loop
				return;
			}
			else if (errno == EINTR)
			{
				continue;
			}
			else
			{
				perror("read\n");

				break;
			}
		}
	}

	// cleanup
	epoll_ctl(epfd, EPOLL_CTL_DEL, client_fd, NULL);
	destroy_client(c);
	close(client_fd);
}

int main()
{
	setbuf(stdout, NULL);
	setbuf(stderr, NULL);

	int server_fd, client_addr_len;
	struct sockaddr_in client_addr;

	server_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (server_fd == -1)
	{
		printf("Socket creation failed: %s\n", strerror(errno));
		return 1;
	}

	int reuse = 1;
	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0)
	{
		printf("SO_REUSEADDR failed: %s\n", strerror(errno));
		return 1;
	}

	struct sockaddr_in serv_addr = {
		.sin_family = AF_INET,
		.sin_port = htons(PORT),
		.sin_addr = {htonl(INADDR_ANY)},
	};

	if (bind(server_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) != 0)
	{
		printf("Bind failed: %s\n", strerror(errno));
		return 1;
	}

	if (listen(server_fd, 16) != 0)
	{
		printf("Listen failed: %s\n", strerror(errno));
		return 1;
	}

	set_nonblocking(server_fd);
	int epfd = epoll_create1(0);
	if (epfd == -1)
	{
		perror("epoll_create1\n");
		return 1;
	}

	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.fd = server_fd;
	epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &ev);

	printf("Server listening on port %d\n", PORT);

	struct epoll_event events[100];
	while (1)
	{
		int n = epoll_wait(epfd, events, 100, 1000);
		if (n < 0)
		{
			if (errno == EINTR)
				continue;
			perror("epoll_wait\n");
			break;
		}

		for (int i = 0; i < n; ++i)
		{
			int fd = events[i].data.fd;
			if (fd == server_fd)
			{
				client_addr_len = sizeof(client_addr);
				int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, (socklen_t *)&client_addr_len);
				if (client_fd == -1)
				{
					if (errno == EAGAIN || errno == EWOULDBLOCK)
						continue;
					perror("accept\n");
					continue;
				}
				set_nonblocking(client_fd);
				client *c = create_client(client_fd);
				struct epoll_event client_ev = {.events = EPOLLIN, .data = {.fd = client_fd}};
				if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &client_ev) == -1)
				{
					perror("epoll_ctl ADD client\n");
					close(client_fd);
					continue;
				}
				printf("Client with fd %d connected\n", client_fd);
			}
			else
			{
				client *c = NULL;
				if (fd >= 0 && fd < MAX_CLIENTS)
					c = clients[fd];
				if (c == NULL)
				{
					epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
					close(fd);
					continue;
				}

				handle_client(c, epfd);
			}
		}
	}

	close(server_fd);
	close(epfd);
	return 0;
}
