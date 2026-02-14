#define _DEFAULT_SOURCE
#include "command.h"
#include <unistd.h>
#include <string.h> 
#include "client.h"
#include <stdlib.h>     
#include <stdio.h>   
#include <unistd.h>
#include <sys/time.h> 
#define TABLE_SIZE 1024


typedef struct entry{
	char* key;
	char* value;
	struct entry *next;
} entry;


struct entry *table[TABLE_SIZE];

struct expire *expiry_table[TABLE_SIZE];

typedef struct expire{
    char* key;
    long long expiry;
    struct expire *next;
}expire;

unsigned long hash(const char* key){
	unsigned long h= 5381;
	int c;

	while((c=*key++)){
		h=((h<<5)+h)+c;
	}
	return h%TABLE_SIZE;
}
long long now_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec * 1000LL) + (tv.tv_usec / 1000LL);
}

static void send_simple_string(client *c, const char *s) {
    if (!c || !s) return;
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "+%s\r\n", s);
    if (n > 0) write(c->client_fd, buf, (size_t)n);
    return;
}

//sets expiry for the key
void set_expiry(const char* key,long long expiry_time,const char* expiry_time_type){
    unsigned index=hash(key);
    if (strcmp(expiry_time_type, "EX") == 0) {
        expiry_time = expiry_time * 1000;   // seconds -> ms
    }

    struct expire* e= expiry_table[index];
    while(e){
        if(strcmp(key,e->key)==0){
            (e->expiry);
            e->expiry=now_ms()+expiry_time;
            return;
        }
        e=e->next;
    }
    long long now= now_ms();
    struct expire* new_expiry = malloc(sizeof(struct expire));
    new_expiry->key = (char*)strdup(key);
    new_expiry->expiry=now+ expiry_time;
    new_expiry->next = expiry_table[index];
    expiry_table[index] = new_expiry;
    return;  
}

void set_key(const char* key,const char* value){
    unsigned index=hash(key);
    struct entry* e= table[index];
    while(e){
        if(strcmp(key,e->key)==0){
            (e->value);
             e->value= (char*)strdup(value);
            return;
        }
        e=e->next;
    }

    struct  entry *newEntry = malloc(sizeof(struct entry));
    newEntry->key = (char*)strdup(key);
    newEntry->value = (char*)strdup(value);
    newEntry->next = table[index];
    table[index] = newEntry;
    return;
}
void cmd_ping(client *c){
    write(c->client_fd, "+PONG\r\n",7);
}

void cmd_echo(client* c){
    if(c->argc>=2){
        char hdr[64];
        int n = snprintf(hdr, sizeof(hdr), "$%zu\r\n", strlen(c->argv[1]));
        write(c->client_fd, hdr, n);
        write(c->client_fd, c->argv[1], strlen(c->argv[1]));
        write(c->client_fd, "\r\n", 2);
    } else {
        write(c->client_fd, "$-1\r\n", 5);
    }
}

void cmd_set(client *c) {
    if (!c || c->argc < 3) {
        const char *err = "-ERR wrong number of arguments for 'set' command\r\n";
        write(c->client_fd, err, strlen(err));
        return;
    }

    const char *key = c->argv[1];
    const char *value = c->argv[2];

    // optional expiry
    if (c->argc >= 5) {
        printf("setting Expiery");
        const char *type = c->argv[3];
        long long t = atoll(c->argv[4]);

        if ((strcmp(type, "EX") == 0 || strcmp(type, "PX") == 0) && t > 0) {
            set_expiry(key, t, type);
        }
    }

    set_key(key, value);
    send_simple_string(c, "all:OK");
}

static void send_bulk_reply(client *c, const char *s) {
    if (!s) {
        const char *nil = "$-1\r\n";
        write(c->client_fd, nil, strlen(nil));
        return;
    }
    char header[64];
    int n = snprintf(header, sizeof(header), "$%zu\r\n", strlen(s));
    if (n > 0) write(c->client_fd, header, (size_t)n);
    write(c->client_fd, s, strlen(s));
    write(c->client_fd, "\r\n", 2);
}

void cmd_get(client *c) {
    printf("Getting value for key");
    if (!c || !c->argv[1]) {
        send_bulk_reply(c, NULL);
        return;
    }
    long long now= now_ms();
    const char *key = c->argv[1];
    unsigned index = hash(key);
    struct entry *e = table[index];
    struct expire *exp = expiry_table[index];
    while (exp) {
        if (strcmp(key, exp->key) == 0) {
            if (exp->expiry != -1 && now > exp->expiry) {
                // expired → delete
                //delete_key(key);
                return;
            }
        }
        exp = exp->next;
    }
    while (e) {
        printf("Getting value");
        if (strcmp(key, e->key) == 0) {
            send_bulk_reply(c, e->value);
            return;
        }
        e = e->next;
    }

    send_bulk_reply(c, NULL);
}