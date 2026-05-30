/* echocli - connect to a TCP echo server, send a message, print the reply.
 *
 * Usage: echocli <ip> <port> <message...> */

#include "libc/unistd.h"
#include "libc/stdlib.h"
#include "libc/stdio.h"
#include "libc/string.h"
#include "libc/sys/socket.h"
#include "libc/arpa/inet.h"

int main(int argc, char **argv) {
    if (argc < 4) {
        fputs("usage: echocli <ip> <port> <message...>\n", STDERR_FILENO);
        return 1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { fputs("echocli: socket failed\n", STDERR_FILENO); return 1; }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)atoi(argv[2]));
    sa.sin_addr.s_addr = inet_addr(argv[1]);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fputs("echocli: connect failed\n", STDERR_FILENO);
        close(fd);
        return 1;
    }

    /* Join the remaining argv into one space-separated message. */
    char msg[512];
    size_t len = 0;
    for (int i = 3; i < argc && len < sizeof(msg) - 2; i++) {
        size_t al = strlen(argv[i]);
        if (i > 3 && len < sizeof(msg) - 1) msg[len++] = ' ';
        if (len + al >= sizeof(msg) - 1) al = sizeof(msg) - 1 - len;
        memcpy(msg + len, argv[i], al);
        len += al;
    }
    msg[len++] = '\n';

    if (send(fd, msg, len, 0) < 0) {
        fputs("echocli: send failed\n", STDERR_FILENO);
        close(fd);
        return 1;
    }

    char buf[512];
    ssize_t got = 0;
    while ((size_t)got < len) {
        ssize_t n = recv(fd, buf + got, sizeof(buf) - (size_t)got, 0);
        if (n <= 0) break;
        got += n;
    }
    if (got > 0) {
        fputs("echocli: reply: ", STDOUT_FILENO);
        write(STDOUT_FILENO, buf, (size_t)got);
    }
    close(fd);
    return 0;
}
