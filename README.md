# Minimal Redis Server

## A minimal Redis-inspired TCP server using C language.

### This project implements:

1. Non-blocking sockets
2. epoll-based event loop
3. Per-client state management
4. Inline command parsing (Redis-compatible style)
5. Basic command dispatch table


## Flow

The server utilizes [eventpoll](https://man7.org/linux/man-pages/man7/epoll.7.html) system calls to be able to handle multiple clients
the FD for server and each client is set to be nonblocking using [fcntl](https://man7.org/linux/man-pages/man2/fcntl.2.html) then added to epoll instance
then we accept new connections or commands through the ready epoll objects

In sequence 

Read client data-> Parse commands -> Dispatch the arguments to command table -> Process command ->Write response

## Features

### Supports the following commands: 

```
PING
ECHO {value}
SET key value
GET key
```

## Testing

```
#compile
gcc *.c -o main

#run server
./main

#connect to the server
nc localhost 6379
PING
#the server should respond with +PONG
```

### For windows OS
---

1. Replace epoll with select or IOCP

2. Replace POSIX calls with Winsock

3. Add WSAStartup

4. Change nonblocking setu