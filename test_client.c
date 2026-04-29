// Usage: ./test_client <host> <port>
//
// recv_message() returns the raw body bytes (everything after the last
// header '|'), e.g. for 1|ERR|14|1|Name in use| the body is
// "1|Name in use|". For 1|MSG|30|#all|Bob|Welcome to the chat!|
// the body is "#all|Bob|Welcome to the chat!|".
//
// ERR body format: <code>|<explanation>|
// MSG body format: <sender>|<recip>|<message body>|
//
// Compile: gcc -Wall -g -o test_client test_client.c
 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/time.h>
 
// connect to host:port, set a 3s receive timeout, return fd
static int connect_to(const char *host, int port)
{
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);
    struct addrinfo *res;
    if (getaddrinfo(host, portstr, &hints, &res) != 0) {
        return -1;
    }
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { 
        freeaddrinfo(res); 
        return -1; 
    }
    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        close(fd); freeaddrinfo(res); 
        return -1; 
    }
    // 3-second receive timeout so tests never hang forever
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    freeaddrinfo(res);
    return fd;
}
 
static void send_raw(int fd, const char *msg) { write(fd, msg, strlen(msg)); }
 
static int recv_exact(int fd, char *buf, int n) {
    int got = 0;
    while (got < n) {
        int r = (int)read(fd, buf + got, n - got);
        if (r <= 0) {
            return got;
        }
        got += r;
    }
    return got;
}
 
// read one server message — fills type_out[4] and body buf (NUL-terminated)
// returns body length >= 0, or -1 on EOF/error
static int recv_message(int fd, char *type_out, char *buf, int buflen)
{
    int pos;
 
    // version field
    pos = 0;
    for (;;) {
        char c;
        if (recv_exact(fd, &c, 1) != 1) {
            return -1;
        }
        if (c == '|') {
            break;
        }
        if (pos++ >= 4) {
            return -1;
        }
    }
 
    // type field
    pos = 0;
    for (;;) {
        char c;
        if (recv_exact(fd, &c, 1) != 1) {
            return -1;
        }
        if (c == '|') { 
            type_out[pos] = '\0'; 
            break; 
        }
        if (pos >= 3) {
            return -1;
        }
        type_out[pos++] = c;
    }
 
    // length field
    char lenstr[12]; int lpos = 0;
    for (;;) {
        char c;
        if (recv_exact(fd, &c, 1) != 1) {
            return -1;
        }
        if (c == '|') {
            break;
        }
        if (lpos >= 10) {
            return -1;
        }
        lenstr[lpos++] = c;
    }
    lenstr[lpos] = '\0';
    int blen = atoi(lenstr);
    if (blen < 0 || blen >= buflen) {
        return -1;
    }
 
    if (recv_exact(fd, buf, blen) != blen) {
        return -1;
    }
    buf[blen] = '\0';
    return blen;
}
 
// test counters
static int tests_run = 0, tests_pass = 0;
 
static void check_contains(const char *desc, const char *buf, const char *needle) {
    tests_run++;
    if (strstr(buf, needle)) {
        printf(" PASS: %s\n", desc); tests_pass++;
    } 
    else {
        printf(" FAIL: %s\n expected to contain: [%s]\n got: [%s]\n", desc, needle, buf);
    }
}
 
static void check_type(const char *desc, const char *got, const char *expected) {
    tests_run++;
    if (strcmp(got, expected) == 0) {
        printf(" PASS: %s (type=%s)\n", desc, got); tests_pass++;
    } 
    else {
        printf(" FAIL: %s\n expected type [%s] got [%s]\n", desc, expected, got);
    }
}
 
// drain messages until we see Welcome, then flush for 150ms to clear
// any stale broadcasts that arrive right after joining
static int drain_until_welcome(int fd) {
    char t[4], b[4096];
    int saw_welcome = 0;
 
    // phase 1: wait up to 3s per message until we see Welcome
    for (int i = 0; i < 20 && !saw_welcome; i++) {
        int r = recv_message(fd, t, b, sizeof(b));
        if (r < 0) {
            return -1;
        }
        if (strcmp(t, "MSG") == 0 && strstr(b, "Welcome")){
            saw_welcome = 1;
        }
    }
    if (!saw_welcome) {
        return -1;
    }
 
    // phase 2: switch to 150ms timeout and drain any trailing stale msgs
    struct timeval tv = { .tv_sec = 0, .tv_usec = 150000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    for (;;) {
        int r = recv_message(fd, t, b, sizeof(b));
        if (r < 0) {
            break;
        } // timeout means queue is clean
    }
 
    // restore normal 3s timeout
    tv.tv_sec = 3; tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return 0;
}
 
// each test uses fresh connections with unique names
// where a SET generates a broadcast to both clients, we drain the
// sender's copy before reading the observer's copy
 
int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <host> <port>\n", argv[0]);
        return 1;
    }
    const char *host = argv[1];
    int port = atoi(argv[2]);
 
    char type[4], body[65536];
 
    // T1: NAM → Welcome
    printf("\n--- T1: NAM welcome ---\n");
    {
        int fd = connect_to(host, port);
        send_raw(fd, "1|NAM|4|Bob|");
        recv_message(fd, type, body, sizeof(body));
        check_type("NAM response is MSG", type, "MSG");
        check_contains("NAM → Welcome message", body, "Welcome");
        close(fd);
    }
 
    // T2: duplicate name → ERR code 1
    printf("\n--- T2: duplicate name ---\n");
    {
        int fd1 = connect_to(host, port);
        int fd2 = connect_to(host, port);
        send_raw(fd1, "1|NAM|6|Alice|");
        recv_message(fd1, type, body, sizeof(body)); // welcome
 
        send_raw(fd2, "1|NAM|6|Alice|");
        recv_message(fd2, type, body, sizeof(body));
        check_type("Duplicate name → ERR", type, "ERR");
        check_contains("Duplicate name → code 1", body, "1|");
        close(fd1); close(fd2);
    }
 
    // T3: illegal character in name → ERR code 3
    printf("\n--- T3: illegal char in name ---\n");
    {
        int fd = connect_to(host, port);
        send_raw(fd, "1|NAM|5|Bad!|");
        recv_message(fd, type, body, sizeof(body));
        check_type("Bad name char → ERR", type, "ERR");
        check_contains("Bad name char → code 3", body, "3|");
        close(fd);
    }
 
    // T4: name too long → ERR code 4
    printf("\n--- T4: name too long ---\n");
    {
        int fd = connect_to(host, port);
        // 33 a's
        send_raw(fd, "1|NAM|34|aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|");
        recv_message(fd, type, body, sizeof(body));
        check_type("Name too long → ERR", type, "ERR");
        check_contains("Name too long → code 4", body, "4|");
        close(fd);
    }
 
    // T5: SET status broadcasts to room
    printf("\n--- T5: SET status broadcast ---\n");
    {
        int sender = connect_to(host, port);
        int observer = connect_to(host, port);
 
        send_raw(sender, "1|NAM|5|Carl|");
        recv_message(sender, type, body, sizeof(body)); // Carl welcome
 
        send_raw(observer, "1|NAM|5|Dani|");
        recv_message(observer, type, body, sizeof(body)); // Dani welcome
 
        send_raw(sender, "1|SET|6|Happy|");
 
        // broadcast goes to both; drain sender's copy first
        recv_message(sender, type, body, sizeof(body));
        recv_message(observer, type, body, sizeof(body));
        check_type("SET broadcast is MSG", type, "MSG");
        check_contains("SET broadcast → name", body, "Carl");
        check_contains("SET broadcast → status text", body, "Happy");
 
        close(sender); close(observer);
    }
 
    // T6: MSG to #all forwarded
    printf("\n--- T6: MSG to #all ---\n");
    {
        usleep(300000); // let T5 departure broadcasts fully flush
        int fd1 = connect_to(host, port);
        int fd2 = connect_to(host, port);
 
        send_raw(fd1, "1|NAM|5|Eve_|");
        drain_until_welcome(fd1);
 
        send_raw(fd2, "1|NAM|6|Frank|");
        drain_until_welcome(fd2);
 
        send_raw(fd1, "1|MSG|19||#all|Hello world!|");
 
        recv_message(fd1, type, body, sizeof(body)); // drain fd1's own copy
        recv_message(fd2, type, body, sizeof(body)); // fd2 gets the broadcast
        check_type("MSG #all → type is MSG", type, "MSG");
        check_contains("MSG #all → body forwarded", body, "Hello world!");
        check_contains("MSG #all → sender field = Eve_", body, "Eve_");
 
        close(fd1); close(fd2);
    }
 
    // T7: private MSG
    printf("\n--- T7: private MSG ---\n");
    {
        usleep(300000);
        int fd1 = connect_to(host, port);
        int fd2 = connect_to(host, port);
 
        send_raw(fd1, "1|NAM|5|Gina|");
        drain_until_welcome(fd1);
 
        send_raw(fd2, "1|NAM|5|Hank|");
        drain_until_welcome(fd2);
 
        send_raw(fd1, "1|MSG|21||Hank|Secret message|");
 
        // only Hank receives this
        recv_message(fd2, type, body, sizeof(body));
        check_type("Private MSG → type is MSG", type, "MSG");
        check_contains("Private MSG → body delivered", body, "Secret message");
        check_contains("Private MSG → sender = Gina", body, "Gina");
 
        close(fd1); close(fd2);
    }
 
    // T8: MSG to unknown recipient → ERR code 2
    printf("\n--- T8: MSG unknown recipient ---\n");
    {
        usleep(300000);
        int fd = connect_to(host, port);
        send_raw(fd, "1|NAM|4|Ian|");
        drain_until_welcome(fd);
 
        send_raw(fd, "1|MSG|17||NoOne|Hi there!|");
        recv_message(fd, type, body, sizeof(body));
        check_type("MSG unknown recip → ERR", type, "ERR");
        check_contains("MSG unknown recip → code 2", body, "2|");
 
        close(fd);
    }
 
    // T9: WHO <user> with status
    printf("\n--- T9: WHO specific user ---\n");
    {
        int fd1 = connect_to(host, port);
        int fd2 = connect_to(host, port);
 
        send_raw(fd1, "1|NAM|4|Jan|");
        recv_message(fd1, type, body, sizeof(body)); // Jan welcome
 
        send_raw(fd2, "1|NAM|4|Kim|");
        recv_message(fd2, type, body, sizeof(body)); // Kim welcome
 
        send_raw(fd1, "1|SET|4|OK!|");
        recv_message(fd1, type, body, sizeof(body)); // drain Jan's copy
        recv_message(fd2, type, body, sizeof(body)); // drain Kim's copy
 
        send_raw(fd2, "1|WHO|4|Jan|");
        recv_message(fd2, type, body, sizeof(body));
        check_type("WHO user → MSG", type, "MSG");
        check_contains("WHO user → name in body", body, "Jan");
        check_contains("WHO user → status in body",body, "OK!");
 
        close(fd1); close(fd2);
    }
 
    // T10: WHO #all lists everyone
    printf("\n--- T10: WHO #all ---\n");
    {
        int fd1 = connect_to(host, port);
        int fd2 = connect_to(host, port);
 
        send_raw(fd1, "1|NAM|4|Leo|");
        recv_message(fd1, type, body, sizeof(body));
 
        send_raw(fd2, "1|NAM|4|Mia|");
        recv_message(fd2, type, body, sizeof(body));
 
        send_raw(fd1, "1|WHO|5|#all|");
        recv_message(fd1, type, body, sizeof(body));
        check_type("WHO #all → MSG", type, "MSG");
        check_contains("WHO #all → Leo listed", body, "Leo");
        check_contains("WHO #all → Mia listed", body, "Mia");
 
        close(fd1); close(fd2);
    }
 
    // T11: unknown protocol version → ERR code 0
    printf("\n--- T11: bad protocol version ---\n");
    {
        int fd = connect_to(host, port);
        send_raw(fd, "9|NAM|4|Ned|");
        recv_message(fd, type, body, sizeof(body));
        check_type("Bad version → ERR", type, "ERR");
        check_contains("Bad version → code 0", body, "0|");
        close(fd);
    }
 
    // T12: WHO unknown user → ERR code 2
    printf("\n--- T12: WHO unknown user ---\n");
    {
        int fd = connect_to(host, port);
        send_raw(fd, "1|NAM|4|Oli|");
        recv_message(fd, type, body, sizeof(body)); // welcome
 
        send_raw(fd, "1|WHO|6|Ghost|");
        recv_message(fd, type, body, sizeof(body));
        check_type("WHO unknown → ERR", type, "ERR");
        check_contains("WHO unknown → code 2", body, "2|");
 
        close(fd);
    }
 
    // T13: SET empty status suppresses broadcast
    printf("\n--- T13: SET empty status ---\n");
    {
        int fd1 = connect_to(host, port);
        int fd2 = connect_to(host, port);
 
        send_raw(fd1, "1|NAM|4|Pat|");
        recv_message(fd1, type, body, sizeof(body));
 
        send_raw(fd2, "1|NAM|6|Quinn|");
        recv_message(fd2, type, body, sizeof(body));
 
        // SET with empty status: body is just "|" = 1 byte
        send_raw(fd1, "1|SET|1||");
 
        // WHO Pat from Quinn — should report "No status"
        send_raw(fd2, "1|WHO|4|Pat|");
        recv_message(fd2, type, body, sizeof(body));
        check_contains("Empty SET → WHO shows No status", body, "No status");
 
        close(fd1); close(fd2);
    }
 
    // T14: MSG body too long → ERR code 4
    printf("\n--- T14: MSG too long ---\n");
    {
        usleep(300000);
        int fd = connect_to(host, port);
        send_raw(fd, "1|NAM|4|Rex|");
        drain_until_welcome(fd);
 
        // 81-char message body — one over the 80-char limit
        char longmsg[200];
        snprintf(longmsg, sizeof(longmsg),
            "1|MSG|88||#all|"
            "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
            "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx|"); // 81 x's
        send_raw(fd, longmsg);
        recv_message(fd, type, body, sizeof(body));
        check_type("MSG too long → ERR", type, "ERR");
        check_contains("MSG too long → code 4", body, "4|");
 
        close(fd);
    }
 
    // T15: status too long → ERR code 4
    printf("\n--- T15: SET status too long ---\n");
    {
        int fd = connect_to(host, port);
        send_raw(fd, "1|NAM|4|Sam|");
        drain_until_welcome(fd);
 
        // 65-char status — one over the 64-char limit
        char longset[200];
        snprintf(longset, sizeof(longset),
            "1|SET|66|"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "a|"); // 65 a's
        send_raw(fd, longset);
        recv_message(fd, type, body, sizeof(body));
        check_type("Status too long → ERR", type, "ERR");
        check_contains("Status too long → code 4", body, "4|");
 
        close(fd);
    }
 
    printf("\n══════════════════════════════\n");
    printf("%d / %d tests passed.\n", tests_pass, tests_run);
    printf("══════════════════════════════\n\n");
    return (tests_pass == tests_run) ? 0 : 1;
}
