#ifndef PROTO_H
#define PROTO_H
 
// limits
#define PROTO_VER "1"
#define MAX_NAME 32
#define MAX_STATUS 64
#define MAX_MSG 80
 
// read_message - read one complete protocol message from fd.
// Wire format: 1|<TYPE>|<BODYLEN>|<BODY>
// BODY is exactly BODYLEN bytes and ends with '|'.
// On success: fills type_out (3 chars + NUL), body_out (body_len bytes + NUL),
//             sets *body_len_out, returns 1.
// On protocol error: sends ERR 0 to client, returns 0.
// On EOF / IO error: returns <= 0.
int read_message(int fd, char *type_out, char *body_out, size_t body_max, int *body_len_out);
 
// send_msg - send 1|MSG|<len>|<sender>|<recip>|<body>|
// Returns 0 on success, -1 on error.
int send_msg(int fd, const char *sender, const char *recip, const char *body);
 
// send_err - send 1|ERR|<len>|<code>|<explanation>|
// Returns 0 on success, -1 on error.
int send_err(int fd, int code, const char *expl);
 
#endif // PROTO_H