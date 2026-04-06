/*
 * client.c — Reliable Group Notification Subscriber
 * FIX: send_control() now uses the bound sock so the server
 *      records the correct source port for delivery.
 *
 * Build:  gcc -Wall -O2 client.c -o client
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include "packet.h"

#define SEEN_WINDOW 256
static uint16_t seen[SEEN_WINDOW];
static int      seen_count = 0;

static int already_seen(uint16_t seq) {
    for (int i = 0; i < seen_count; i++)
        if (seen[i] == seq) return 1;
    return 0;
}

static void mark_seen(uint16_t seq) {
    if (seen_count < SEEN_WINDOW) {
        seen[seen_count++] = seq;
    } else {
        memmove(seen, seen + 1, (SEEN_WINDOW - 1) * sizeof(uint16_t));
        seen[SEEN_WINDOW - 1] = seq;
    }
}

static int        sock;
static struct sockaddr_in server_data_addr;
static struct sockaddr_in server_ctrl_addr;
static uint16_t   my_group;
static volatile int running = 1;

static void send_control(uint8_t msg_type) {
    Packet pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.msg_type    = msg_type;
    pkt.group_id    = htons(my_group);
    pkt.payload_len = 0;
    packet_stamp(&pkt);
    pkt.checksum = compute_checksum(&pkt);
    sendto(sock, &pkt, HEADER_SIZE, 0,
           (struct sockaddr *)&server_ctrl_addr, sizeof(server_ctrl_addr));
}

static void on_signal(int sig) { (void)sig; running = 0; }

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0); /* Disable buffering so it prints immediately */
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <server_ip> <group_id>\n", argv[0]);
        return 1;
    }

    const char *server_ip = argv[1];
    my_group = (uint16_t)atoi(argv[2]);

    memset(&server_data_addr, 0, sizeof(server_data_addr));
    server_data_addr.sin_family = AF_INET;
    server_data_addr.sin_port   = htons(SERVER_PORT);
    inet_pton(AF_INET, server_ip, &server_data_addr.sin_addr);

    memset(&server_ctrl_addr, 0, sizeof(server_ctrl_addr));
    server_ctrl_addr.sin_family = AF_INET;
    server_ctrl_addr.sin_port   = htons(CONTROL_PORT);
    inet_pton(AF_INET, server_ip, &server_ctrl_addr.sin_addr);

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family      = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port        = 0;
    if (bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        perror("bind"); return 1;
    }

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    send_control(MSG_JOIN);
    printf("[CLIENT] Joined group %u on %s, waiting for notifications...\n",
           my_group, server_ip);

    long stat_received = 0, stat_duplicates = 0, stat_bad_cs = 0;

    while (running) {
        Packet pkt;
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);

        ssize_t n = recvfrom(sock, &pkt, sizeof(pkt), 0,
                             (struct sockaddr *)&src, &slen);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            perror("recvfrom"); break;
        }
        if (n < (ssize_t)HEADER_SIZE) continue;
        if (pkt.msg_type != MSG_NOTIFY) continue;

        if (!verify_checksum(&pkt)) {
            fprintf(stderr, "[WARN] Bad checksum on seq %u\n", ntohs(pkt.seq_num));
            stat_bad_cs++; continue;
        }

        uint16_t seq = ntohs(pkt.seq_num);
        uint16_t gid = ntohs(pkt.group_id);

        if (already_seen(seq)) {
            printf("[DUP]  seq=%u (ignored)\n", seq);
            stat_duplicates++;
        } else {
            mark_seen(seq);
            double age_ms = packet_age_ms(&pkt);
            printf("[RECV] seq=%-5u group=%-3u latency=%.2f ms  \"%.*s\"\n",
                   seq, gid, age_ms,
                   (int)ntohs(pkt.payload_len), pkt.payload);
            stat_received++;
        }

        Packet ack;
        memset(&ack, 0, sizeof(ack));
        ack.msg_type    = MSG_ACK;
        ack.seq_num     = pkt.seq_num;
        ack.group_id    = pkt.group_id;
        ack.payload_len = 0;
        packet_stamp(&ack);
        ack.checksum = compute_checksum(&ack);
        sendto(sock, &ack, HEADER_SIZE, 0,
               (struct sockaddr *)&server_data_addr, sizeof(server_data_addr));
    }

    send_control(MSG_LEAVE);
    printf("\n[CLIENT] Left group %u\n", my_group);
    printf("  Received:   %ld\n  Duplicates: %ld\n  Bad CRC:    %ld\n",
           stat_received, stat_duplicates, stat_bad_cs);

    close(sock);
    return 0;
}
