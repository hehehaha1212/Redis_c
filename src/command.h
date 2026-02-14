#ifndef COMMANDS_H
#define COMMANDS_H

#include "client.h"

/* forward declaration in case client.h isn't visible at include time */

void cmd_ping(client* c);
void cmd_echo(client* c);
void cmd_set(client* c);
void cmd_get(client* c);
#endif