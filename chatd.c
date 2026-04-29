// chatd.c - Chat server main: set up listening socket, accept connections

// Usage: chatd <port>
// Design:
//   main() creates the listening socket and accepts connections in a loop.
//   Each accepted connection is handed to a new detached client_thread().
//   The global clients[] table (client.h) tracks all connected, named users.
//   All protocol I/O is handled by the functions in proto.h / proto.c.
 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "proto.h"
#include "client.h"
 
int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(1);
    }
 
    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port: %s\n", argv[1]);
        exit(1);
    }
 
    // initialise client table
    for (int i = 0; i < MAX_CLIENTS; i++){
        clients[i].fd = -1;
    }
 
    // create listening socket
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) { 
        perror("socket"); 
        exit(1); 
    }
 
    int yes = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
 
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons((uint16_t)port)
    };

    if (bind(listenfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); 
        exit(1);
    }

    if (listen(listenfd, 16) < 0) { 
        perror("listen"); 
        exit(1); 
    }
 
    fprintf(stderr, "chatd listening on port %d\n", port);
 
    // accept loop
    for (;;) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int connfd = accept(listenfd, (struct sockaddr *)&cli_addr, &cli_len);
        if (connfd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            continue;
        }
 
        // find a free slot
        pthread_mutex_lock(&clients_mutex);
        int slot = -1;
        for (int i = 0; i < MAX_CLIENTS; i++){
            if (clients[i].fd == -1) { 
                slot = i; 
                break; 
            }
        }
        if (slot == -1) {
            pthread_mutex_unlock(&clients_mutex);
            send_err(connfd, 0, "Server full");
            close(connfd);
            continue;
        }
        clients[slot].fd = connfd;
        clients[slot].name[0] = '\0';
        clients[slot].status[0] = '\0';
        pthread_mutex_unlock(&clients_mutex);
 
        // hand off to a new thread
        int *arg = malloc(sizeof(int));
        if (!arg) {
            clients[slot].fd = -1;
            close(connfd);
            continue;
        }
        *arg = slot;
 
        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, arg) != 0){
            perror("pthread_create");
            free(arg);
            clients[slot].fd = -1;
            close(connfd);
            continue;
        }
        pthread_detach(tid);
    }
}