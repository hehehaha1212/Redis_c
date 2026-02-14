#ifndef COMMAND_TABLE_H
#define COMMAND_TABLE_H

#include "client.h"

typedef void(*commandFunc)(client*);

typedef struct {
    const char *name;
    int arity;
    commandFunc proc;
} redisCommand;


extern redisCommand commandTable[];

extern redisCommand *lookupCommand(const char* name);
#endif