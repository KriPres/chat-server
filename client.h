#ifndef CLIENT_H
#define CLIENT_H
 
#include <pthread.h>
#include "proto.h"
 
#define MAX_CLIENTS 128
 
// client record — one slot per connected user
typedef struct {
    int fd; // socket fd; -1 = slot free
    char name[MAX_NAME + 1];
    char status[MAX_STATUS + 1];
} Client;
 
// global client table (defined in client.c)
extern Client clients[MAX_CLIENTS];
extern pthread_mutex_t clients_mutex;
 
// client_thread - handles one connected client for its lifetime.
// Argument: heap-allocated int* containing the slot index; freed on entry.
void *client_thread(void *arg);
 
#endif // CLIENT_H