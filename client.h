// prevent double inclusion
#ifndef CLIENT_H
#define CLIENT_H

// import necessary libraries + header files (threading + message protocol (custom))
#include <pthread.h>
#include "proto.h"
 
// we allow a maximum of 128 clients
#define MAX_CLIENTS 128
 
// client record — one slot per connected user
typedef struct {
    int fd; // socket fd; -1 = slot free
    char name[MAX_NAME + 1]; // store client name in char array
    char status[MAX_STATUS + 1]; // store client status in a char arrray
} Client;
 
// global client table (defined in client.c)
extern Client clients[MAX_CLIENTS];
extern pthread_mutex_t clients_mutex;
 
// client_thread - handles one connected client for its lifetime.
// Argument: heap-allocated int* containing the slot index; freed on entry.
void *client_thread(void *arg);
 
#endif // CLIENT_H