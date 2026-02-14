#include "command_table.h"
#include "command.h"
#include <strings.h>
#include <ctype.h>

redisCommand commandTable[]={
    {"PING",1,  cmd_ping},
    {"ECHO",2,  cmd_echo},
    {"SET", 3, cmd_set},
    {"GET", 2, cmd_get},
    {NULL, 0, NULL}    // terminator
};

redisCommand *lookupCommand(const char* name){
    for(int i=0;commandTable[i].name!=NULL;++i){
        if(strcasecmp(commandTable[i].name,name)==0){
            return &commandTable[i];
        }
    }
    return NULL;
}