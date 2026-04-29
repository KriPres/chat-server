#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include "proto.h"
#include "client.h"
 
// global client table
Client clients[MAX_CLIENTS];
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
 
// Returns 1 if every character in s is in [32,126].
static int valid_printable(const char *s, int len)
{
    for (int i = 0; i < len; i++){
        if ((unsigned char)s[i] < 32 || (unsigned char)s[i] > 126){
            return 0;
        }
    }
    return 1;
}
 
// Returns 1 if every character is a letter, digit, hyphen, or underscore.
static int valid_name(const char *s, int len)
{
    if (len < 1) return 0;
    for (int i = 0; i < len; i++) {
        char c = s[i];
        if (!isalnum((unsigned char)c) && c != '-' && c != '_')
            return 0;
    }
    return 1;
}
 
// Send a MSG to every named client. Caller must hold clients_mutex.
static void broadcast_locked(const char *sender, const char *recip, const char *body)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd >= 0 && clients[i].name[0] != '\0'){
            send_msg(clients[i].fd, sender, recip, body);
        }
    }
}
 
void *client_thread(void *arg)
{
    int slot = *(int *)arg;
    free(arg);
 
    int fd = clients[slot].fd;
    int named = 0;
 
    char type[4];
    char body[100001]; // max 5-digit body length + NUL
 
    for (;;) {
        int body_len;
        int rc = read_message(fd, type, body, sizeof(body), &body_len);
        if (rc <= 0) {
            goto disconnect;
        }
 
        // NAM
        if (strcmp(type, "NAM") == 0) {
            int name_len = body_len - 1; // strip trailing '|'
            body[name_len] = '\0';
 
            if (!valid_name(body, name_len)) {
                send_err(fd, 3, "Illegal character in name");
                continue;
            }
            if (name_len > MAX_NAME) {
                send_err(fd, 4, "Name too long");
                continue;
            }
 
            pthread_mutex_lock(&clients_mutex);
            int dup = 0;
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (i == slot){
                    continue;
                }
                if (clients[i].fd >= 0 && strcmp(clients[i].name, body) == 0) {
                    dup = 1; 
                    break;
                }
            }
            if (dup) {
                pthread_mutex_unlock(&clients_mutex);
                send_err(fd, 1, "Name in use");
                continue;
            }
            strncpy(clients[slot].name, body, MAX_NAME);
            clients[slot].name[MAX_NAME] = '\0';
            named = 1;
            pthread_mutex_unlock(&clients_mutex);
 
            send_msg(fd, "#all", body, "Welcome to the chat!");
            continue;
        }
 
        // all subsequent types require a completed NAM
        if (!named) {
            send_err(fd, 0, "Must send NAM first");
            goto disconnect;
        }
 
        char my_name[MAX_NAME + 1];
        pthread_mutex_lock(&clients_mutex);
        strncpy(my_name, clients[slot].name, MAX_NAME + 1);
        pthread_mutex_unlock(&clients_mutex);
 
        // SET
        if (strcmp(type, "SET") == 0) {
            int status_len = body_len - 1;
            body[status_len] = '\0';
 
            if (!valid_printable(body, status_len)) {
                send_err(fd, 3, "Illegal character in status");
                continue;
            }
            if (status_len > MAX_STATUS) {
                send_err(fd, 4, "Status too long");
                continue;
            }
 
            pthread_mutex_lock(&clients_mutex);
            strncpy(clients[slot].status, body, MAX_STATUS);
            clients[slot].status[MAX_STATUS] = '\0';
 
            if (status_len > 0) {
                char announce[MAX_NAME + MAX_STATUS + 16];
                snprintf(announce, sizeof(announce),
                         "%.*s is now \"%.*s\"",
                         MAX_NAME, my_name, MAX_STATUS, body);
                broadcast_locked("#all", "#all", announce);
            }
            pthread_mutex_unlock(&clients_mutex);
            continue;
        }
 
        // MSG
        if (strcmp(type, "MSG") == 0) {
            // body from client: <sender(ignored)>|<recip>|<message>|
            // sender is always empty so body starts with '|'
            char *p = body;
 
            char *sender_end = memchr(p, '|', body_len);
            if (!sender_end) { 
                send_err(fd, 0, "Malformed MSG"); 
                goto disconnect; 
            }
            *sender_end = '\0';
            p = sender_end + 1;
            int remaining = body_len - (int)(p - body);
 
            char *recip_end = memchr(p, '|', remaining);
            if (!recip_end) { 
                send_err(fd, 0, "Malformed MSG"); 
                goto disconnect; 
            }
            *recip_end = '\0';
            char *recip = p;
            p = recip_end + 1;
            remaining = body_len - (int)(p - body);
 
            if (remaining < 1) { 
                send_err(fd, 0, "Malformed MSG"); 
                goto disconnect; 
            }
            int msg_len = remaining - 1; // strip trailing '|'
            char msg_body[body_len + 1];
            memcpy(msg_body, p, msg_len);
            msg_body[msg_len] = '\0';
 
            if (!valid_printable(msg_body, msg_len)) {
                send_err(fd, 3, "Illegal character in message");
                continue;
            }
            if (msg_len > MAX_MSG) {
                send_err(fd, 4, "Message too long");
                continue;
            }
            if (!valid_printable(recip, (int)strlen(recip))) {
                send_err(fd, 3, "Illegal character in recipient");
                continue;
            }
 
            pthread_mutex_lock(&clients_mutex);
            if (strcmp(recip, "#all") == 0) {
                broadcast_locked(my_name, "#all", msg_body);
            } 
            else {
                int found = 0;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (clients[i].fd >= 0 && strcmp(clients[i].name, recip) == 0) {
                        found = 1;
                        send_msg(clients[i].fd, my_name, recip, msg_body);
                        break;
                    }
                }
                if (!found) {
                    pthread_mutex_unlock(&clients_mutex);
                    send_err(fd, 2, "Unknown recipient");
                    continue;
                }
            }
            pthread_mutex_unlock(&clients_mutex);
            continue;
        }
 
        // WHO
        if (strcmp(type, "WHO") == 0){
            int target_len = body_len - 1;
            body[target_len] = '\0';
            char *target = body;
 
            char *resp = NULL;
            size_t resp_size = 0;
            FILE *resp_fp = open_memstream(&resp, &resp_size);
            if (!resp_fp) { 
                send_err(fd, 0, "Internal error"); 
                continue; 
            }
 
            pthread_mutex_lock(&clients_mutex);
 
            if (strcmp(target, "#all") == 0){
                int first = 1;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (clients[i].fd < 0 || clients[i].name[0] == '\0'){
                        continue;
                    }
                    if (!first){
                        fprintf(resp_fp, "\n");
                    }
                    first = 0;
                    if (clients[i].status[0] != '\0'){
                        fprintf(resp_fp, "%s: %s", clients[i].name, clients[i].status);
                        }
                    else{
                        fprintf(resp_fp, "%s", clients[i].name);
                    }
                }
            } 
            else {
                int found = 0;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (clients[i].fd >= 0 && strcmp(clients[i].name, target) == 0) {
                        found = 1;
                        if (clients[i].status[0] != '\0'){
                            fprintf(resp_fp, "%s: %s", clients[i].name, clients[i].status);
                            }
                        else{
                            fprintf(resp_fp, "No status");
                        }
                        break;
                    }
                }
                if (!found) {
                    pthread_mutex_unlock(&clients_mutex);
                    fclose(resp_fp);
                    free(resp);
                    send_err(fd, 2, "Unknown user");
                    continue;
                }
            }
            pthread_mutex_unlock(&clients_mutex);
            fclose(resp_fp);
 
            send_msg(fd, "#all", my_name, resp ? resp : "");
            free(resp);
            continue;
        }
 
        // unknown type
        send_err(fd, 0, "Unknown message type");
        goto disconnect;
    }
 
disconnect:
    pthread_mutex_lock(&clients_mutex);
    // only clear the slot if it still belongs to this thread's fd —
    // a slow thread could arrive here after a new client reused the slot
    int was_named = 0;
    char departed[MAX_NAME + 1];
    departed[0] = '\0';
    if (clients[slot].fd == fd) {
        was_named = (clients[slot].name[0] != '\0');
        strncpy(departed, clients[slot].name, MAX_NAME + 1);
        clients[slot].fd = -1;
        clients[slot].name[0] = '\0';
        clients[slot].status[0] = '\0';
    }
    if (was_named) {
        char ann[MAX_NAME + 32];
        snprintf(ann, sizeof(ann), "%s has left the chat", departed);
        broadcast_locked("#all", "#all", ann);
    }
    pthread_mutex_unlock(&clients_mutex);
 
    close(fd);
    return NULL;
}