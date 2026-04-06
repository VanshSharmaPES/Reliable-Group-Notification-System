/*
 * server_baseline.c — Best-effort UDP server (no ACK / retransmit)
 *
 * Used as the comparison baseline in benchmarking.
 * Sends each notification exactly once with no reliability mechanisms.
 *
 * Build:  gcc -Wall -O2 -pthread server_baseline.c -o server_baseline
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include "packet.h"

typedef struct {
    struct sockaddr_in addr;
    int active;
} Sub;

static Sub      subs[MAX_SUBSCRIBERS];
static int      sub_count = 0;
static pthread_mutex_t sub_lock = PTHREAD_MUTEX_INITIALIZER;

static int      data_sock;
static int      ctrl_sock;
static uint16_t seq_ctr = 0;
static long     stat_sent = 0;

static void *ctrl_thread(void *arg) {
    (void)arg;
    struct sockaddr_in src;
    socklen_t slen = sizeof(src);
    Packet pkt;
    while (1) {
        if (recvfrom(ctrl_sock, &pkt, sizeof(pkt), 0,
                     (struct sockaddr *)&src, &slen) < (ssize_t)HEADER_SIZE)
            continue;
        if (!verify_checksum(&pkt)) continue;
        pthread_mutex_lock(&sub_lock);
        if (pkt.msg_type == MSG_JOIN) {
            int dup = 0;
            for (int i = 0; i < sub_count; i++)
                if (subs[i].addr.sin_addr.s_addr == src.sin_addr.s_addr &&
                    subs[i].addr.sin_port == src.sin_port) { dup = 1; break; }
            if (!dup && sub_count < MAX_SUBSCRIBERS) {
                subs[sub_count].addr   = src;
                subs[sub_count].active = 1;
                sub_count++;
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &src.sin_addr, ip, INET_ADDRSTRLEN);
                printf("[JOIN] %s:%d (%d subs)\n", ip, ntohs(src.sin_port), sub_count);
            }
        } else if (pkt.msg_type == MSG_LEAVE) {
            for (int i = 0; i < sub_count; i++) {
                if (subs[i].addr.sin_addr.s_addr == src.sin_addr.s_addr &&
                    subs[i].addr.sin_port == src.sin_port) {
                    subs[i] = subs[--sub_count]; break;
                }
            }
        }
        pthread_mutex_unlock(&sub_lock);
    }
    return NULL;
}

int main(void) {
    struct sockaddr_in addr;
    int opt = 1;

    data_sock = socket(AF_INET, SOCK_DGRAM, 0);
    setsockopt(data_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    ctrl_sock = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(CONTROL_PORT);
    if (bind(ctrl_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind ctrl"); return 1;
    }

    printf("[BASELINE] Control on :%d  (no ACK / retransmit)\n", CONTROL_PORT);

    pthread_t t;
    pthread_create(&t, NULL, ctrl_thread, NULL);

    char line[512];
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\n")] = '\0';
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';
        const char *msg = colon + 1;

        Packet pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.msg_type    = MSG_NOTIFY;
        pkt.seq_num     = htons(seq_ctr++);
        pkt.group_id    = htons((uint16_t)atoi(line));
        pkt.payload_len = htons((uint16_t)strlen(msg));
        strncpy(pkt.payload, msg, MAX_PAYLOAD - 1);
        packet_stamp(&pkt);
        pkt.checksum = compute_checksum(&pkt);

        pthread_mutex_lock(&sub_lock);
        for (int i = 0; i < sub_count; i++) {
            sendto(data_sock, &pkt, HEADER_SIZE + ntohs(pkt.payload_len), 0,
                   (struct sockaddr *)&subs[i].addr, sizeof(subs[i].addr));
            stat_sent++;
        }
        printf("[SEND] seq=%u to %d subs (best-effort)\n",
               seq_ctr - 1, sub_count);
        pthread_mutex_unlock(&sub_lock);
    }
    printf("Total sent: %ld\n", stat_sent);
    return 0;
}
