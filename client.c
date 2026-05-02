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
// The function by itself is not thread safe since we are modifying clients array
// We assume that caller has obtained lockbefore calling this
static void broadcast_locked(const char *sender, const char *recip, const char *body)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd >= 0 && clients[i].name[0] != '\0'){
            send_msg(clients[i].fd, sender, recip, body);
        }
    }
}
 
// Function passed into p_thread create used to handle message parsing + displaying and
// storing everything in the server
void *client_thread(void *arg)
{
    // get slot integer passed in via pointer and free that passed in arg since it
    // is no longer needed
    int slot = *(int *)arg;
    free(arg);
    
    // get file descriptor for that particular client at that slot
    int fd = clients[slot].fd;

    // boolean variable to indicate whether or not the user is named
    int named = 0;
    
    // string (char array) to store message type (3 upperchase chars + '\0')
    char type[4];

    // max 5-digit body length + '\0'
    char body[100001]; 
    
    // while True loop for reading and sending messages
    for (;;) {

        // get number of bytes from reading message from fd
        int body_len;

        // storing key msg attributes to type and body
        int rc = read_message(fd, type, body, sizeof(body), &body_len);

        // if unable to read anything successfully, go to disconnect block
        if (rc <= 0) {
            goto disconnect;
        }
 
        // Handle case where using NAM command to initialize user name
        if (strcmp(type, "NAM") == 0) {

            // strip trailing '|'
            int name_len = body_len - 1; 

            // add terminator for string
            body[name_len] = '\0';
            
            // if name is invalid due to characters used, call send_err function with appropriate args:
            // error code, descriptions
            // then skip remainder of code here to process next command

            if (!valid_name(body, name_len)) {
                send_err(fd, 3, "Illegal character in name");
                continue;
            }

            // if name is invalid due to being too long, call send_err function with appropriate args:
            // error code, descriptions
            // then skip remainder of code here to process next command
            
            if (name_len > MAX_NAME) {
                send_err(fd, 4, "Name too long");
                continue;
            }
            
            // if we got to this point, we have a chance for a valid name
            // we now want to check for a duplicate name
            // during this checking process we lock to avoid race conditions
            pthread_mutex_lock(&clients_mutex);

            // boolean flag to determine if there is a duplicate name
            int dup = 0;

            // go through each client in the clients array
            for (int i = 0; i < MAX_CLIENTS; i++) {

                // ignore the current client being used while checking for duplicates
                if (i == slot){
                    continue;
                }

                // if the current slot in clients array is active and same as requested name, 
                // mark boolean flag for duplicate and break from loop
                if (clients[i].fd >= 0 && strcmp(clients[i].name, body) == 0) {
                    dup = 1; 
                    break;
                }
            }

            // if we determined that requested name is already in use, send corresponding error
            // skip remainder of code to process next command
            if (dup) {
                pthread_mutex_unlock(&clients_mutex);
                send_err(fd, 1, "Name in use");
                continue;
            }

            // copy body of message (requested username for NAM command) into client array slot for the name attribute,
            // up to max name characters; stnrncpy will pad rest with null bytes
            strncpy(clients[slot].name, body, MAX_NAME);

            // add null terminator at end
            clients[slot].name[MAX_NAME] = '\0';

            // set boolean named flag to true
            named = 1;

            // after wrapping up this modification, unlock the thread
            pthread_mutex_unlock(&clients_mutex);
            
            // call send_msg function to welcome the user to the chat
            send_msg(fd, "#all", body, "Welcome to the chat!");
            continue;
        }
 
        // all subsequent types require a completed NAM
        // if not named, send an error indicating that a name must be declared before chat participation
        if (!named) {
            send_err(fd, 0, "Must send NAM first");
            goto disconnect;
        }
        
        // string variable to store user name (extra space for terminator)
        char my_name[MAX_NAME + 1];

        // access user name from clients array and copy it to local my_name variable
        // use thread safe lock and unlock to access name
        pthread_mutex_lock(&clients_mutex);
        strncpy(my_name, clients[slot].name, MAX_NAME + 1);
        pthread_mutex_unlock(&clients_mutex);
 
        // Case where server command is SET type
        if (strcmp(type, "SET") == 0) {

            // strip trailing '|', add null terminator at end
            int status_len = body_len - 1;
            body[status_len] = '\0';
            
            // if status has unprintable chars, send approprioate error message
            // skip remainder of code to process next command
            if (!valid_printable(body, status_len)) {
                send_err(fd, 3, "Illegal character in status");
                continue;
            }

            // if status length is to long, send appropriate error message
            // skip remainder of code to process next command
            if (status_len > MAX_STATUS) {
                send_err(fd, 4, "Status too long");
                continue;
            }
            
            // obtain lock for clients array, edit status
            pthread_mutex_lock(&clients_mutex);
            strncpy(clients[slot].status, body, MAX_STATUS);
            clients[slot].status[MAX_STATUS] = '\0';
            
            // if new status is non empty, announce to all users currently logged into server
            if (status_len > 0) {

                // save status message to announce buffer and then send message from #all to #all
                // indicating the change; status change is more a property of the whole chat
                // than the user messaging the chat
                char announce[MAX_NAME + MAX_STATUS + 16];
                snprintf(announce, sizeof(announce),
                         "%.*s is now \"%.*s\"",
                         MAX_NAME, my_name, MAX_STATUS, body);

                // call the broadcast locked function to send message to all active users in the chat 
                broadcast_locked("#all", "#all", announce);
            }

            // once we send message to everyone about the status, relinquish the lock and skip remainder to next command
            pthread_mutex_unlock(&clients_mutex);
            continue;
        }
 
        // Handle a MSG type message
        if (strcmp(type, "MSG") == 0) {

            // body from client: <sender(ignored)>|<recip>|<message>|
            // sender is always empty so body starts with '|'

            // pointer variable p for body of message
            char *p = body;
            
            // find first occurrence of '|' in message to indicate end of sender portion form body
            char *sender_end = memchr(p, '|', body_len);

            // if no '|' in the message it is malformed, so print error
            // fatal error so disconnect
            if (!sender_end) { 
                send_err(fd, 0, "Malformed MSG"); 
                goto disconnect; 
            }

            // set sender_end string (position of first '|' to be '\0' null terminator)
            *sender_end = '\0';

            // set p char pointer to be right after sender_end (demarcate beginning of receipient field)
            p = sender_end + 1;

            // compute remaining length of unconsumed bytes (what remains after sender)
            int remaining = body_len - (int)(p - body);
            
            // look through remaining characters and look for first occurrence of '|'
            char *recip_end = memchr(p, '|', remaining);

            // if no bars to indicate improperly formatted recipient field, send err,
            // disconnect
            if (!recip_end) { 
                send_err(fd, 0, "Malformed MSG"); 
                goto disconnect; 
            }

            // set recip_end string (position of next '|' to be '\0' null terminator)
            *recip_end = '\0';

            // save pointer to recipient name
            char *recip = p;

            // move p pointer to end of recipient field
            p = recip_end + 1;

            // determine reaining number of bytes (what remains after sender and recipient)
            remaining = body_len - (int)(p - body);
            
            // if no ending '|', send error indicating malformed message + disconnect
            if (remaining < 1) { 
                send_err(fd, 0, "Malformed MSG"); 
                goto disconnect; 
            }

            // strip trailing '|' and save msg body to msg_body
            int msg_len = remaining - 1; 

            // we make a string (char array with more than enough room using body_len + 1)
            // accommodate largest possible message body
            char msg_body[body_len + 1];

            // copy current position of p (pointer to actual message content) to msg_body (up to msg_len bytes)
            memcpy(msg_body, p, msg_len);

            // add null terminator for cleanliness
            msg_body[msg_len] = '\0';
            
            // if message body has invalid characters, 
            // send appropriate error message and skip remainder to process next command
            if (!valid_printable(msg_body, msg_len)) {
                send_err(fd, 3, "Illegal character in message");
                continue;
            }

            // if message body too long,
            // send appropriate error message and skip remainder to process next command
            if (msg_len > MAX_MSG) {
                send_err(fd, 4, "Message too long");
                continue;
            }

            // additional check for illegal character in recipient
            // if invalid characters in recipient, send appropriate error message and
            // skip remainder to process next command
            if (!valid_printable(recip, (int)strlen(recip))) {
                send_err(fd, 3, "Illegal character in recipient");
                continue;
            }
            
            // Now that we have finally confirmed that message is clean and ready to send
            // obtain lock because we now have to modify shared data
            pthread_mutex_lock(&clients_mutex);

            // if we are sending it to all, use the send all function to send message to everyone
            if (strcmp(recip, "#all") == 0) {
                broadcast_locked(my_name, "#all", msg_body);
            } 

            // if we are trying to send to a specific individual
            else {

                // look for specific individual in client list
                int found = 0;
                for (int i = 0; i < MAX_CLIENTS; i++) {

                    // if we find them, send the message, mark found as true, and break from loop
                    if (clients[i].fd >= 0 && strcmp(clients[i].name, recip) == 0) {
                        found = 1;
                        send_msg(clients[i].fd, my_name, recip, msg_body);
                        break;
                    }
                }

                // if recipient not active or existent, send error message indicating unknown recipient and
                // skip to next command
                if (!found) {
                    pthread_mutex_unlock(&clients_mutex);
                    send_err(fd, 2, "Unknown recipient");
                    continue;
                }
            }

            // having finished working on shared data, relinquish lock and skip remainder of code to go to next command
            pthread_mutex_unlock(&clients_mutex);
            continue;
        }
 
        // Handle a WHO type message
        if (strcmp(type, "WHO") == 0){

            // determine length of user that is target of the who command
            int target_len = body_len - 1;

            // replace '|' with null terminator
            body[target_len] = '\0';

            // string for target user; initially pointing to body
            char *target = body;
            
            // initialize pointer string for response, with initialize size to be 0
            char *resp = NULL;
            size_t resp_size = 0;

            // use open memstream to treat response kinda of like a file we can write to
            // convenient for dynamic string manipulation
            FILE *resp_fp = open_memstream(&resp, &resp_size);

            // if unable to open memstream, send a non fatal error and skip remainder
            // of code to continue with next iteration
            if (!resp_fp) { 
                send_err(fd, 0, "Internal error"); 
                continue; 
            }

            // obtain lock since we are acessing client data to get status info
            pthread_mutex_lock(&clients_mutex);
            
            // if we requested for status for everyone (#all)
            if (strcmp(target, "#all") == 0){

                // first boolean which is indicator for whether or not to print new line
                int first = 1;

                // iterate over every entry in client array
                for (int i = 0; i < MAX_CLIENTS; i++) {

                    // skip empty fds or blank client names
                    if (clients[i].fd < 0 || clients[i].name[0] == '\0'){
                        continue;
                    }

                    // print new line as long as this is not the first entry 
                    if (!first){
                        fprintf(resp_fp, "\n");
                    }

                    // after the first line, mark first line boolean indicator to false
                    first = 0;

                    // if client has a status, add status string line to resp_fp buffer
                    if (clients[i].status[0] != '\0'){
                        fprintf(resp_fp, "%s: %s", clients[i].name, clients[i].status);
                        }

                    // otherwise just add their screen name to resp_fp buffer
                    else{
                        fprintf(resp_fp, "%s", clients[i].name);
                    }
                }
            } 

            // otherwise, we are interested in a specific buffer
            else {

                int found = 0;

                // look for that user and update resp_fp with their name and status ("No status" if empty status)
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

                // if user not found, unlock, close resp_fp file pointer and free resp pointer
                // also send unknown user error, skip to next command processing step
                if (!found) {
                    pthread_mutex_unlock(&clients_mutex);
                    fclose(resp_fp);
                    free(resp);
                    send_err(fd, 2, "Unknown user");
                    continue;
                }
            }

            // having wrapped up looking at shared data, relinquish lock
            pthread_mutex_unlock(&clients_mutex);

            // close file pointer
            fclose(resp_fp);
            
            // server responds to WHO query to user from #all
            // if resp is NULL, send_msg gets passed empty string
            send_msg(fd, "#all", my_name, resp ? resp : "");

            // free resp pointer
            free(resp);
            continue;
        }
 
        // If we receive an unknown message type, send error and disconnect
        send_err(fd, 0, "Unknown message type");
        goto disconnect;
    }
 
// disconnect block (called by goto in a few cases)
disconnect:

    // obtain lock for clients array
    pthread_mutex_lock(&clients_mutex);

    // only clear the slot if it still belongs to this thread's fd —
    // a slow thread could arrive here after a new client reused the slot

    // boolean to determine if it was named
    int was_named = 0;

    // create and initialize departed as an empty string (char array)
    char departed[MAX_NAME + 1];
    departed[0] = '\0';

    // if current fd matches clients[slot].fd clean up (shield during race condition)
    // another client may have used this slot
    if (clients[slot].fd == fd) {

        // determine if it was named
        was_named = (clients[slot].name[0] != '\0');

        // copy user to a temporarily variable before cleaning it up
        strncpy(departed, clients[slot].name, MAX_NAME + 1);

        // clean up slot entry in client
        clients[slot].fd = -1;
        clients[slot].name[0] = '\0';
        clients[slot].status[0] = '\0';
    }

    // if was named
    if (was_named) {
        
        // announcement for user leaving the chat
        char ann[MAX_NAME + 32];

        // store user leaving the chat message to the ann string
        snprintf(ann, sizeof(ann), "%s has left the chat", departed);

        // broadcast user departure from chat to #alll (from #all)
        broadcast_locked("#all", "#all", ann);
    }

    // we are done accessign shared data, so relinquish lock
    pthread_mutex_unlock(&clients_mutex);
    
    // close file descriptor
    close(fd);

    // return NULL to adhere to void * return type 
    return NULL;
}