#include <stdio.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include "packet.h"

int main() {
    Packet pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.msg_type    = MSG_JOIN;
    pkt.group_id    = htons(1);
    pkt.payload_len = 0;
    packet_stamp(&pkt);
    pkt.checksum = compute_checksum(&pkt);
    printf("Checksum set to %d\n", pkt.checksum);
    if (!verify_checksum(&pkt)) {
        printf("Verification FAILED!\n");
    } else {
        printf("Verification OK!\n");
    }
    return 0;
}
