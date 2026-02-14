#ifndef CLIENT_H
#define CLIENT_H

#include <stddef.h>

#define MAX_ARGS 256

typedef struct client
{
	int client_fd;

	char querybuf[4096];
	size_t qb_len; // how many bytes currently inside querybuf
	size_t qb_pos; // current parse position

	long multibulklen; // how many arguments expected; -1 means not known yet
	long bulklen;	   // length of current bulk string

	int argc;			  // number of parsed args
	char *argv[MAX_ARGS]; // parsed args
} client;

#endif