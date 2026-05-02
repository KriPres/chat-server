// Protocol I/O implementation

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "proto.h"
 
// Read exactly n bytes from fd. Returns n on success, <=0 on EOF/error.
static int read_exact(int fd, char *buf, size_t n)
{
    // number of bytes read successfully so far
    size_t got = 0;

    // loop until we successfully read n bytes
    while (got < n) {

        // read up to (n - got) bytes from fd and store them to position (buf + got)
        ssize_t r = read(fd, buf + got, n - got);

        // if no bytes read, return 0
        if (r <= 0) {
            return (int)r;
        }

        // if read some bytes successfully, increment got by number of bytes read
        got += (size_t)r;
    }

    // return total number of bytes read
    return (int)n;
}

// helper function that reads a message
// returns 1 on success; 0 on failure, or some other code if read_exact has another error code
// takes in:
// fd : file descriptor
// char * type_out : message type
// char * body_out : message body
// size_t body_max : limit on body message size
// int * body_len_out : pointer to specifed number of bytes for message body

// parses smth like this: 1|MSG|61|#all|Bob|Alice: I was here first\nBob: Smiling politely\nCarol|
int read_message(int fd, char *type_out, char *body_out, size_t body_max, int *body_len_out)
{  
    // buffer to read header fields
    char hdr[64];

    // integer to mark position of header
    int hpos = 0;
 
    // version field - parse each character
    while (hpos < (int)sizeof(hdr) - 1) {

        // read exactly 1 byte from fd into hdr buffer
        int r = read_exact(fd, hdr + hpos, 1);

        // if nothing read successfully, we are done, so return r 
        if (r <= 0) {
            return r;
        }

        // if we reach a vertical bar, mark end of string in buffer using null terminator and break from loop
        if (hdr[hpos] == '|') { 
            hdr[hpos] = '\0'; 
            break; 
        }

        // increment hpos to continue looping through string
        hpos++;
    }

    // confirm that the first read string is "1": the only available protocol
    // send error if bad protocol; terminate early by returning 0
    if (strcmp(hdr, "1") != 0) {
        send_err(fd, 0, "Bad protocol version");
        return 0;
    }
 
    // parsing the type field
    // we treat hdr as a temp buffer for things, so we overwrite its contents
    hpos = 0;

    // keep iterating until end of string or other break condition
    while (hpos < (int)sizeof(hdr) - 1) {

        // parse one character from file descriptor into buffer
        int r = read_exact(fd, hdr + hpos, 1);

        // if error parsing character, return error code returned by read_exact
        if (r <= 0) {
            return r;
        }

        // if vertical bar, mark with null terminator and break from loop
        if (hdr[hpos] == '|') { 
            hdr[hpos] = '\0'; 
            break; 
        }

        // increment hpos buffer position variable
        hpos++;
    }

    // ensure that sting length of the message type is 3
    // if it is not, invalid message and send error, return 0 to stop
    if (strlen(hdr) != 3) {
        send_err(fd, 0, "Bad message type length");
        return 0;
    }

    // we made it this far, so message type is valid
    // paste it into type_out from our buffer that processed it
    strncpy(type_out, hdr, 4);
 
    // parsing body-length field
    // reset position in buffer to overwrite it
    hpos = 0;

    // iterate one byte at a time as before
    while (hpos < (int)sizeof(hdr) - 1) {
        int r = read_exact(fd, hdr + hpos, 1);
        if (r <= 0) {
            return r;
        }
        if (hdr[hpos] == '|') { 
            hdr[hpos] = '\0'; 
            break; 
        }
        hpos++;
    }

    // pointer endp to where we stopped parsing the integer part of the string
    char *endp;

    // convert string to long using start pointer within string, end pointer within string and base 10
    long blen = strtol(hdr, &endp, 10);

    // ensure endp non empty, and between 0 and 99999
    // if not, send error message, return 0
    if (*endp != '\0' || blen < 0 || blen > 99999) {
        send_err(fd, 0, "Bad body length");
        return 0;
    }
    
    // reasonable body length value confirmed

    // parsing body — drain and error if too large to fit our buffer
    if ((size_t)blen + 1 > body_max) {

        // draining message to discard it
        char drain[512];
        long left = blen;

        // while message portion remains
        while (left > 0) {

            // read up to 512 bytes at a time (sizeof(drain)) if possible, or remaining bytes
            int chunk = (left > (long)sizeof(drain)) ? (int)sizeof(drain) : (int)left;
            
            // ignore the bytes
            if (read_exact(fd, drain, chunk) <= 0) {
                return -1;
            }

            // decrement left
            left -= chunk;
        }

        // send error indicating body is too large, return 0 to terminate
        send_err(fd, 0, "Body too large");
        return 0;
    }
    
    // message is a reasonable size

    // read full message body to body_out
    int r = read_exact(fd, body_out, (size_t)blen);

    // if error reading, return that error
    if (r <= 0) {
        return r;
    }

    // if read successfully, end message body with null terminator
    body_out[blen] = '\0';

    // pointer to body_len_out - needed to tell caller how long the body is
    *body_len_out = (int)blen;
    
    // if body length is 0 or doesn't end with '|', send error and return 0
    // short circuit evaluation to prevent errors with out of bound index
    if (blen == 0 || body_out[blen - 1] != '|') {
        send_err(fd, 0, "Message does not end with |");
        return 0;
    }
    
    // return 1, indicating success
    return 1;
}
 

// function to send message based on protocol
// int fd: socket to send message to
// char * sender: string containing sender name
// char * recipt: string containing recipient name
// char * body: actual message content as a string
int send_msg(int fd, const char *sender, const char *recip, const char *body)
{

    // compute body length: sender|recipient|message|
    int blen = (int)(strlen(sender) + 1 + strlen(recip) + 1 + strlen(body) + 1);

    // create ouput buffer big enough to store message based on protocol, with some leeway / math
    char wire[6 + 4 + 6 + blen + 4];

    // stores size of final string
    // print into wire buffer the string formatted message based on body length, sender, recipient, and body 
    int wlen = snprintf(wire, sizeof(wire), "1|MSG|%d|%s|%s|%s|", blen, sender, recip, body);
    
    // return -1 if nothing written into buffer or it is too big to fit in the buffer (extra safety condition)
    if (wlen <= 0 || wlen >= (int)sizeof(wire)) {
        return -1;
    }
    
    // write the the message to socket and get result w
    ssize_t w = write(fd, wire, wlen);

    // return 0 if everything successfully written, -1 otherwise
    return (w == wlen) ? 0 : -1;
}
 
// function to build error message
// int fd: file descriptor for socket
// int code: error code
// const char *explt: explanation string
int send_err(int fd, int code, const char *expl)
{
    // buffer for code_str
    char code_str[16];

    // fill error code into buffer
    snprintf(code_str, sizeof(code_str), "%d", code);

    // determine message length based on message format
    // code_str is always 1 char here, but this code approachh enables generalizability
    int blen = (int)(strlen(code_str) + 1 + strlen(expl) + 1);
    
    // wire buffer
    char wire[256];

    // process error message into buffer using string formatting of blen, code, expl
    int wlen = snprintf(wire, sizeof(wire), "1|ERR|%d|%d|%s|", blen, code, expl);

    // if problem printing to buffer, return -1 to indicate failure
    if (wlen <= 0 || wlen >= (int)sizeof(wire)) {
        return -1;
    }
    
    // write the message to the socket and get result w
    ssize_t w = write(fd, wire, wlen);

    // return 0 if everything successfully written, -1 otherwise.
    return (w == wlen) ? 0 : -1;
}