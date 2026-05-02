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
    // if more than one argument, error with non-zero exit status
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(1);
    }
    
    // extract port argument as integer, make sure it is a 16 bit integer, 
    // error with non-zero exit status otherwise
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

    // error with non-zero exit status if issue with creating listening socket
    if (listenfd < 0) { 
        perror("socket"); 
        exit(1); 
    }
    
    // allows for port to be reused if it was recently used; avoid address alread in use error
    int yes = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    
    // define key server attributes
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons((uint16_t)port)
    };

    // try to assocaite socket to port on our host if possible; attach
    // if bind unsuccessful, error with binding
    if (bind(listenfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); 
        exit(1);
    }

    // maximum backlog = 16
    // if listen unsuccessful, error with listening
    if (listen(listenfd, 16) < 0) { 
        perror("listen"); 
        exit(1); 
    }
    
    // display message to stderr showing which port listening on
    // sent to stderr to avoid piping to stdout, which wouldn't work since output would 
    // be sent to a file
    fprintf(stderr, "chatd listening on port %d\n", port);
 
    // accept loop (while true)
    for (;;) {

        // initialize client address and create a socket for that
        struct sockaddr_in cli_addr;

        // in built socket length type to store length of client address
        socklen_t cli_len = sizeof(cli_addr);

        // try to accept the clients socket
        int connfd = accept(listenfd, (struct sockaddr *)&cli_addr, &cli_len);

        // if unsuccessful due to connfd < 0
        if (connfd < 0) {

            // if call interrupted by a signal, continue and try again
            if (errno == EINTR) {
                continue;
            }

            // real error, perror and continue to keep server running
            perror("accept");
            continue;
        }
 
        // find a free slot

        // we use a lock here in case other clients are trying to modify shared client data at the same time
        pthread_mutex_lock(&clients_mutex);

        // if there is an empty slot, we can assign that slot to the client
        int slot = -1;
        for (int i = 0; i < MAX_CLIENTS; i++){
            if (clients[i].fd == -1) { 
                slot = i; 
                break; 
            }
        }

        // if no slot found, release lock and send error and go to next iteration
        if (slot == -1) {
            pthread_mutex_unlock(&clients_mutex);
            send_err(connfd, 0, "Server full");
            close(connfd);
            continue;
        }

        // within clients array, go to current slot index
        // set corresponding attributes of client object at that index
        // fd = connection fd
        // name and status is null
        // clearing out leftover data from previous clients
        // in case a client disconnected from an fd
        clients[slot].fd = connfd;
        clients[slot].name[0] = '\0';
        clients[slot].status[0] = '\0';

        // once we are done making those modifications on shared data, we relinquish lock
        pthread_mutex_unlock(&clients_mutex);
 
        // initialize integer pointer
        int *arg = malloc(sizeof(int));

        // if no room left from malloc, then give up
        if (!arg) {
            clients[slot].fd = -1;
            close(connfd);
            continue;
        }

        // store slot index to arg pointer
        *arg = slot;
        pthread_t tid;

        // try to create a new thread which has thread id pointer (&tid)
        // what actually spawns a new thread that calls client_thread
        if (pthread_create(&tid, NULL, client_thread, arg) != 0){

            // if cannot create thread, error out, clean up memory, close connections
            // set fd to -1 and continue so server doesn't crash
            perror("pthread_create");
            free(arg);
            clients[slot].fd = -1;
            close(connfd);
            continue;
        }

        // instead of joining and picking up results of the thread, it handles itself
        pthread_detach(tid);
    }
}