#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include "packet.h"

int main() {
    int control_sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port = htons(9001);
    srv.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(control_sock, (struct sockaddr *)&srv, sizeof(srv));

    struct sockaddr_in src;
    socklen_t slen = sizeof(src);
    Packet pkt;
    printf("Listening...\n");
    ssize_t n = recvfrom(control_sock, &pkt, sizeof(pkt), 0, (struct sockaddr *)&src, &slen);
    printf("Received %zd bytes\n", n);
    if (n < HEADER_SIZE) printf("Too small\n");
    if (!verify_checksum(&pkt)) {
        printf("Checksum failed: computed=%d, received=%d\n", compute_checksum(&pkt), pkt.checksum);
    } else {
        printf("Checksum OK!\n");
    }
    return 0;
}
