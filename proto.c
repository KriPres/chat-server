// Protocol I/O implementation

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "proto.h"
 
// Read exactly n bytes from fd. Returns n on success, <=0 on EOF/error.
static int read_exact(int fd, char *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r <= 0) {
            return (int)r;
        }
        got += (size_t)r;
    }
    return (int)n;
}
 
int read_message(int fd, char *type_out, char *body_out, size_t body_max, int *body_len_out)
{
    char hdr[64];
    int hpos = 0;
 
    // version field
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
    if (strcmp(hdr, "1") != 0) {
        send_err(fd, 0, "Bad protocol version");
        return 0;
    }
 
    // type field
    hpos = 0;
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
    if (strlen(hdr) != 3) {
        send_err(fd, 0, "Bad message type length");
        return 0;
    }
    strncpy(type_out, hdr, 4);
 
    // body-length field
    hpos = 0;
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
    char *endp;
    long blen = strtol(hdr, &endp, 10);
    if (*endp != '\0' || blen < 0 || blen > 99999) {
        send_err(fd, 0, "Bad body length");
        return 0;
    }
 
    // body — drain and error if too large to fit our buffer
    if ((size_t)blen + 1 > body_max) {
        char drain[512];
        long left = blen;
        while (left > 0) {
            int chunk = (left > (long)sizeof(drain)) ? (int)sizeof(drain) : (int)left;
            if (read_exact(fd, drain, chunk) <= 0) {
                return -1;
            }
            left -= chunk;
        }
        send_err(fd, 0, "Body too large");
        return 0;
    }
 
    int r = read_exact(fd, body_out, (size_t)blen);
    if (r <= 0) {
        return r;
    }
    body_out[blen] = '\0';
    *body_len_out = (int)blen;
 
    if (blen == 0 || body_out[blen - 1] != '|') {
        send_err(fd, 0, "Message does not end with |");
        return 0;
    }
 
    return 1;
}
 
int send_msg(int fd, const char *sender, const char *recip, const char *body)
{
    int blen = (int)(strlen(sender) + 1 + strlen(recip) + 1 + strlen(body) + 1);
 
    char wire[6 + 4 + 6 + blen + 4];
    int wlen = snprintf(wire, sizeof(wire), "1|MSG|%d|%s|%s|%s|", blen, sender, recip, body);
    if (wlen <= 0 || wlen >= (int)sizeof(wire)) {
        return -1;
    }
 
    ssize_t w = write(fd, wire, wlen);
    return (w == wlen) ? 0 : -1;
}
 
int send_err(int fd, int code, const char *expl)
{
    char code_str[16];
    snprintf(code_str, sizeof(code_str), "%d", code);
    int blen = (int)(strlen(code_str) + 1 + strlen(expl) + 1);
 
    char wire[256];
    int wlen = snprintf(wire, sizeof(wire), "1|ERR|%d|%d|%s|", blen, code, expl);
    if (wlen <= 0 || wlen >= (int)sizeof(wire)) {
        return -1;
    }
 
    ssize_t w = write(fd, wire, wlen);
    return (w == wlen) ? 0 : -1;
}