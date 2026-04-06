/*
 * server.c — Reliable Group Notification Server
 *
 * Architecture:
 *   - Main thread:    listens on CONTROL_PORT for JOIN/LEAVE
 *   - Notify thread:  reads notifications from stdin, delivers to groups
 *   - ACK thread:     collects ACKs on SERVER_PORT, updates bitmaps
 *   - Reaper thread:  scans retransmit queue, fires timeouts
 *
 * Build:  gcc -Wall -O2 -pthread server.c -o server
 * Run:    ./server
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include "packet.h"

/* ─── Subscriber entry ────────────────────────────────────────────────── */
typedef struct {
    struct sockaddr_in addr;
    int  active;
    char ip[INET_ADDRSTRLEN];
    int  port;
} Subscriber;

/* ─── Group ───────────────────────────────────────────────────────────── */
typedef struct {
    uint16_t   group_id;
    int        count;
    Subscriber subs[MAX_SUBSCRIBERS];
    pthread_mutex_t lock;
} Group;

/* ─── Retransmit queue entry ──────────────────────────────────────────── */
#define MAX_PENDING 256

typedef struct {
    Packet   pkt;
    uint16_t group_id;
    int      ack_bitmap[MAX_SUBSCRIBERS]; /* 1 = ACKed */
    int      sub_count;
    int      retries;
    struct timeval deadline;
    int      active;
} PendingPkt;

/* ─── Global state ────────────────────────────────────────────────────── */
static Group       groups[MAX_GROUPS];
static int         group_count = 0;
static pthread_mutex_t groups_lock = PTHREAD_MUTEX_INITIALIZER;

static PendingPkt  pending[MAX_PENDING];
static pthread_mutex_t pending_lock = PTHREAD_MUTEX_INITIALIZER;

static int notify_sock;   /* outbound DATA socket */
static int control_sock;  /* inbound JOIN/LEAVE   */
static int ack_sock;       /* inbound ACK socket   */

static uint16_t seq_counter = 0;
static pthread_mutex_t seq_lock = PTHREAD_MUTEX_INITIALIZER;

/* ─── Stats ────────────────────────────────────────────────────────────── */
static volatile long stat_sent       = 0;
static volatile long stat_delivered  = 0;
static volatile long stat_retransmit = 0;
static volatile long stat_expired    = 0;

/* ─── Helpers ─────────────────────────────────────────────────────────── */
static uint16_t next_seq(void) {
    pthread_mutex_lock(&seq_lock);
    uint16_t s = seq_counter++;
    pthread_mutex_unlock(&seq_lock);
    return s;
}

static Group *find_or_create_group(uint16_t gid) {
    pthread_mutex_lock(&groups_lock);
    for (int i = 0; i < group_count; i++) {
        if (groups[i].group_id == gid) {
            pthread_mutex_unlock(&groups_lock);
            return &groups[i];
        }
    }
    if (group_count >= MAX_GROUPS) {
        pthread_mutex_unlock(&groups_lock);
        return NULL;
    }
    Group *g = &groups[group_count++];
    g->group_id = gid;
    g->count    = 0;
    pthread_mutex_init(&g->lock, NULL);
    pthread_mutex_unlock(&groups_lock);
    return g;
}

static Group *find_group(uint16_t gid) {
    pthread_mutex_lock(&groups_lock);
    for (int i = 0; i < group_count; i++) {
        if (groups[i].group_id == gid) {
            pthread_mutex_unlock(&groups_lock);
            return &groups[i];
        }
    }
    pthread_mutex_unlock(&groups_lock);
    return NULL;
}

static void add_subscriber(Group *g, struct sockaddr_in *addr) {
    pthread_mutex_lock(&g->lock);
    /* Check for duplicate */
    for (int i = 0; i < g->count; i++) {
        if (g->subs[i].active &&
            g->subs[i].addr.sin_addr.s_addr == addr->sin_addr.s_addr &&
            g->subs[i].addr.sin_port == addr->sin_port) {
            pthread_mutex_unlock(&g->lock);
            return; /* already subscribed */
        }
    }
    if (g->count >= MAX_SUBSCRIBERS) {
        pthread_mutex_unlock(&g->lock);
        fprintf(stderr, "[WARN] Group %u is full\n", g->group_id);
        return;
    }
    Subscriber *s = &g->subs[g->count++];
    s->addr   = *addr;
    s->active = 1;
    inet_ntop(AF_INET, &addr->sin_addr, s->ip, INET_ADDRSTRLEN);
    s->port = ntohs(addr->sin_port);
    printf("[JOIN] %s:%d joined group %u (%d subscribers)\n",
           s->ip, s->port, g->group_id, g->count);
    pthread_mutex_unlock(&g->lock);
}

static void remove_subscriber(Group *g, struct sockaddr_in *addr) {
    pthread_mutex_lock(&g->lock);
    for (int i = 0; i < g->count; i++) {
        if (g->subs[i].active &&
            g->subs[i].addr.sin_addr.s_addr == addr->sin_addr.s_addr &&
            g->subs[i].addr.sin_port == addr->sin_port) {
            printf("[LEAVE] %s:%d left group %u\n",
                   g->subs[i].ip, g->subs[i].port, g->group_id);
            /* Compact array */
            g->subs[i] = g->subs[--g->count];
            pthread_mutex_unlock(&g->lock);
            return;
        }
    }
    pthread_mutex_unlock(&g->lock);
}

/* ─── Deliver a packet to one subscriber ──────────────────────────────── */
static void send_to(int sock, const Packet *pkt, const struct sockaddr_in *dst) {
    size_t len = HEADER_SIZE + ntohs(pkt->payload_len);
    sendto(sock, pkt, len, 0,
           (const struct sockaddr *)dst, sizeof(*dst));
}

/* ─── Enqueue a new pending delivery ──────────────────────────────────── */
static void enqueue_pending(const Packet *pkt, Group *g) {
    pthread_mutex_lock(&pending_lock);
    for (int i = 0; i < MAX_PENDING; i++) {
        if (!pending[i].active) {
            pending[i].pkt        = *pkt;
            pending[i].group_id   = g->group_id;
            pending[i].sub_count  = g->count;
            pending[i].retries    = 0;
            pending[i].active     = 1;
            memset(pending[i].ack_bitmap, 0, sizeof(pending[i].ack_bitmap));
            /* Copy subscriber addresses into snapshot */
            struct timeval now;
            gettimeofday(&now, NULL);
            pending[i].deadline.tv_sec  = now.tv_sec;
            pending[i].deadline.tv_usec = now.tv_usec + TIMEOUT_MS * 1000;
            if (pending[i].deadline.tv_usec >= 1000000) {
                pending[i].deadline.tv_sec++;
                pending[i].deadline.tv_usec -= 1000000;
            }
            pthread_mutex_unlock(&pending_lock);
            return;
        }
    }
    pthread_mutex_unlock(&pending_lock);
    fprintf(stderr, "[WARN] Pending queue full — dropping seq %u\n",
            ntohs(pkt->seq_num));
}

/* ─── Send notification to all group members, enqueue for reliability ─── */
static void notify_group(uint16_t gid, const char *msg) {
    Group *g = find_group(gid);
    if (!g) {
        fprintf(stderr, "[WARN] Group %u not found\n", gid);
        return;
    }

    Packet pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.msg_type    = MSG_NOTIFY;
    pkt.seq_num     = htons(next_seq());
    pkt.group_id    = htons(gid);
    pkt.payload_len = htons((uint16_t)strlen(msg));
    strncpy(pkt.payload, msg, MAX_PAYLOAD - 1);
    packet_stamp(&pkt);
    pkt.checksum = compute_checksum(&pkt);

    pthread_mutex_lock(&g->lock);
    int n = g->count;
    struct sockaddr_in addrs[MAX_SUBSCRIBERS];
    memcpy(addrs, g->subs, n * sizeof(Subscriber));
    pthread_mutex_unlock(&g->lock);

    printf("[NOTIFY] seq=%u group=%u msg=\"%s\" → %d subscriber(s)\n",
           ntohs(pkt.seq_num), gid, msg, n);

    for (int i = 0; i < n; i++) {
        if (((Subscriber *)addrs)[i].active) {
            send_to(notify_sock, &pkt,
                    &((Subscriber *)addrs)[i].addr);
            stat_sent++;
        }
    }

    enqueue_pending(&pkt, g);
}

/* ─── ACK receiver thread ─────────────────────────────────────────────── */
static void *ack_thread(void *arg) {
    (void)arg;
    struct sockaddr_in src;
    socklen_t slen = sizeof(src);
    Packet pkt;

    while (1) {
        ssize_t n = recvfrom(ack_sock, &pkt, sizeof(pkt), 0,
                             (struct sockaddr *)&src, &slen);
        if (n < (ssize_t)HEADER_SIZE) continue;
        if (pkt.msg_type != MSG_ACK) continue;
        if (!verify_checksum(&pkt)) {
            fprintf(stderr, "[WARN] Bad checksum on ACK from port %d\n",
                    ntohs(src.sin_port));
            continue;
        }

        uint16_t seq = ntohs(pkt.seq_num);
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &src.sin_addr, ip, INET_ADDRSTRLEN);

        pthread_mutex_lock(&pending_lock);
        for (int i = 0; i < MAX_PENDING; i++) {
            if (!pending[i].active) continue;
            if (ntohs(pending[i].pkt.seq_num) != seq) continue;

            Group *g = find_group(pending[i].group_id);
            if (!g) break;

            pthread_mutex_lock(&g->lock);
            for (int j = 0; j < pending[i].sub_count; j++) {
                if (g->subs[j].addr.sin_addr.s_addr == src.sin_addr.s_addr &&
                    g->subs[j].addr.sin_port == src.sin_port) {
                    if (!pending[i].ack_bitmap[j]) {
                        pending[i].ack_bitmap[j] = 1;
                        stat_delivered++;
                        printf("[ACK] seq=%u from %s:%d\n",
                               seq, ip, ntohs(src.sin_port));
                    }
                    break;
                }
            }
            pthread_mutex_unlock(&g->lock);

            /* Check if all ACKed */
            int all = 1;
            for (int j = 0; j < pending[i].sub_count; j++) {
                if (!pending[i].ack_bitmap[j]) { all = 0; break; }
            }
            if (all) {
                printf("[DONE] seq=%u fully delivered\n", seq);
                pending[i].active = 0;
            }
            break;
        }
        pthread_mutex_unlock(&pending_lock);
    }
    return NULL;
}

/* ─── Reaper / retransmit thread ──────────────────────────────────────── */
static void *reaper_thread(void *arg) {
    (void)arg;
    while (1) {
        usleep(50000); /* check every 50ms */
        struct timeval now;
        gettimeofday(&now, NULL);

        pthread_mutex_lock(&pending_lock);
        for (int i = 0; i < MAX_PENDING; i++) {
            if (!pending[i].active) continue;

            int past = (now.tv_sec > pending[i].deadline.tv_sec) ||
                       (now.tv_sec == pending[i].deadline.tv_sec &&
                        now.tv_usec >= pending[i].deadline.tv_usec);
            if (!past) continue;

            if (pending[i].retries >= MAX_RETRIES) {
                int lost = 0;
                for (int j = 0; j < pending[i].sub_count; j++)
                    if (!pending[i].ack_bitmap[j]) lost++;
                fprintf(stderr,
                        "[EXPIRE] seq=%u gave up after %d retries "
                        "(%d subscriber(s) never ACKed)\n",
                        ntohs(pending[i].pkt.seq_num),
                        MAX_RETRIES, lost);
                stat_expired += lost;
                pending[i].active = 0;
                continue;
            }

            /* Selective retransmit: only to un-ACKed subscribers */
            Group *g = find_group(pending[i].group_id);
            if (!g) { pending[i].active = 0; continue; }

            int count = 0;
            pthread_mutex_lock(&g->lock);
            for (int j = 0; j < pending[i].sub_count && j < g->count; j++) {
                if (!pending[i].ack_bitmap[j]) {
                    send_to(notify_sock, &pending[i].pkt, &g->subs[j].addr);
                    stat_retransmit++;
                    count++;
                }
            }
            pthread_mutex_unlock(&g->lock);

            pending[i].retries++;
            /* Reset deadline */
            pending[i].deadline.tv_usec = now.tv_usec + TIMEOUT_MS * 1000;
            pending[i].deadline.tv_sec  = now.tv_sec;
            if (pending[i].deadline.tv_usec >= 1000000) {
                pending[i].deadline.tv_sec++;
                pending[i].deadline.tv_usec -= 1000000;
            }
            printf("[RETRANSMIT] seq=%u attempt=%d to %d subscriber(s)\n",
                   ntohs(pending[i].pkt.seq_num),
                   pending[i].retries, count);
        }
        pthread_mutex_unlock(&pending_lock);
    }
    return NULL;
}

/* ─── Control thread (JOIN / LEAVE) ───────────────────────────────────── */
static void *control_thread(void *arg) {
    (void)arg;
    struct sockaddr_in src;
    socklen_t slen = sizeof(src);
    Packet pkt;

    while (1) {
        ssize_t n = recvfrom(control_sock, &pkt, sizeof(pkt), 0,
                             (struct sockaddr *)&src, &slen);
        if (n < (ssize_t)HEADER_SIZE) continue;
        if (!verify_checksum(&pkt)) continue;

        uint16_t gid = ntohs(pkt.group_id);
        Group *g = find_or_create_group(gid);
        if (!g) continue;

        if (pkt.msg_type == MSG_JOIN) {
            add_subscriber(g, &src);
        } else if (pkt.msg_type == MSG_LEAVE) {
            remove_subscriber(g, &src);
        }
    }
    return NULL;
}

/* ─── Stats printer ────────────────────────────────────────────────────── */
static void print_stats(void) {
    printf("\n─── Stats ───────────────────────────────\n");
    printf("  Sent:         %ld\n", stat_sent);
    printf("  Delivered:    %ld\n", stat_delivered);
    printf("  Retransmits:  %ld\n", stat_retransmit);
    printf("  Expired:      %ld\n", stat_expired);
    if (stat_sent > 0)
        printf("  Delivery %%:   %.1f%%\n",
               100.0 * stat_delivered / stat_sent);
    printf("─────────────────────────────────────────\n\n");
}

/* ─── main ─────────────────────────────────────────────────────────────── */
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0); /* Disable buffering for instant printing */
    struct sockaddr_in addr;

    /* Notify (data) socket */
    notify_sock = socket(AF_INET, SOCK_DGRAM, 0);
    int opt = 1;
    setsockopt(notify_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* ACK socket */
    ack_sock = socket(AF_INET, SOCK_DGRAM, 0);
    setsockopt(ack_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)); /* Fix Address already in use */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(SERVER_PORT);
    if (bind(ack_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind ack_sock"); exit(1);
    }

    /* Control socket */
    control_sock = socket(AF_INET, SOCK_DGRAM, 0);
    setsockopt(control_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)); /* Fix Address already in use */
    addr.sin_port = htons(CONTROL_PORT);
    if (bind(control_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind control_sock"); exit(1);
    }

    printf("[SERVER] Listening — data/ACK on :%d, control on :%d\n",
           SERVER_PORT, CONTROL_PORT);

    pthread_t t_ctrl, t_ack, t_reap;
    pthread_create(&t_ctrl,  NULL, control_thread, NULL);
    pthread_create(&t_ack,   NULL, ack_thread,     NULL);
    pthread_create(&t_reap,  NULL, reaper_thread,  NULL);

    /* Main thread: read group:message from stdin */
    char line[512];
    printf("[SERVER] Enter notifications as  group_id:message  (or type 'stats')\n");
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\n")] = '\0';
        if (strcmp(line, "stats") == 0) {
            print_stats();
            continue;
        }
        char *colon = strchr(line, ':');
        if (!colon) { fprintf(stderr, "Format: group_id:message (or type 'stats')\n"); continue; }
        *colon = '\0';
        uint16_t gid = (uint16_t)atoi(line);
        notify_group(gid, colon + 1);
    }

    return 0;
}
