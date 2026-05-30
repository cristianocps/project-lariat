/* echosrv - a TCP echo server.
 *
 * Usage: echosrv [port]   (default 5555)
 * Accepts connections one at a time and echoes everything back until the
 * peer closes.  Reachable from the host via QEMU hostfwd. */

#include "libc/unistd.h"
#include "libc/stdlib.h"
#include "libc/stdio.h"
#include "libc/string.h"
#include "libc/sys/socket.h"
#include "libc/arpa/inet.h"

int main(int argc, char **argv) {
    int port = (argc >= 2) ? atoi(argv[1]) : 5555;

    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) { fputs("echosrv: socket failed\n", STDERR_FILENO); return 1; }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = INADDR_ANY;
    if (bind(sfd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fputs("echosrv: bind failed\n", STDERR_FILENO);
        return 1;
    }
    if (listen(sfd, 4) < 0) {
        fputs("echosrv: listen failed\n", STDERR_FILENO);
        return 1;
    }
    printf("echosrv: listening on port %d\n", port);

    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int cfd = accept(sfd, (struct sockaddr *)&peer, &plen);
        if (cfd < 0) { fputs("echosrv: accept failed\n", STDERR_FILENO); continue; }
        printf("echosrv: client connected\n");

        char buf[1024];
        ssize_t n;
        while ((n = recv(cfd, buf, sizeof(buf), 0)) > 0) {
            ssize_t off = 0;
            while (off < n) {
                ssize_t w = send(cfd, buf + off, (size_t)(n - off), 0);
                if (w <= 0) break;
                off += w;
            }
        }
        printf("echosrv: client disconnected\n");
        close(cfd);
    }
    return 0;
}
