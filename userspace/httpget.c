/* httpget - fetch a URL over TCP and print the response.
 *
 * Usage: httpget <ip> [port] [path]
 * Proves outbound TCP works (through QEMU slirp to the host/internet). */

#include "libc/unistd.h"
#include "libc/stdlib.h"
#include "libc/stdio.h"
#include "libc/string.h"
#include "libc/sys/socket.h"
#include "libc/arpa/inet.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fputs("usage: httpget <ip> [port] [path]\n", STDERR_FILENO);
        return 1;
    }
    const char *ip = argv[1];
    int port = (argc >= 3) ? atoi(argv[2]) : 80;
    const char *path = (argc >= 4) ? argv[3] : "/";

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { fputs("httpget: socket failed\n", STDERR_FILENO); return 1; }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = inet_addr(ip);
    if (sa.sin_addr.s_addr == INADDR_NONE) {
        fputs("httpget: bad ip\n", STDERR_FILENO);
        return 1;
    }

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fputs("httpget: connect failed\n", STDERR_FILENO);
        close(fd);
        return 1;
    }

    char req[512];
    int rl = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
                      path, ip);
    if (send(fd, req, (size_t)rl, 0) < 0) {
        fputs("httpget: send failed\n", STDERR_FILENO);
        close(fd);
        return 1;
    }

    char buf[1024];
    ssize_t n;
    while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) {
        write(STDOUT_FILENO, buf, (size_t)n);
    }
    putchar('\n');
    close(fd);
    return 0;
}
