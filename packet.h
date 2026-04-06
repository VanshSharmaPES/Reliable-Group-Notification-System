#ifndef PACKET_H
#define PACKET_H

#include <stdint.h>
#include <time.h>

/* ─── Message Types ─────────────────────────────────────────────────── */
#define MSG_JOIN     0x01   /* Client -> Server: subscribe to group     */
#define MSG_LEAVE    0x02   /* Client -> Server: unsubscribe            */
#define MSG_NOTIFY   0x03   /* Server -> Client: deliver notification   */
#define MSG_ACK      0x04   /* Client -> Server: acknowledge receipt    */
#define MSG_PING     0x05   /* Server -> Client: keepalive              */
#define MSG_PONG     0x06   /* Client -> Server: keepalive reply        */

/* ─── Constants ─────────────────────────────────────────────────────── */
#define MAX_PAYLOAD      256
#define MAX_SUBSCRIBERS  64
#define MAX_GROUPS       16
#define TIMEOUT_MS       500    /* Retransmit timeout in milliseconds   */
#define MAX_RETRIES      5      /* Maximum retransmission attempts      */
#define SERVER_PORT      9000
#define CONTROL_PORT     9001   /* JOIN/LEAVE control channel           */

/* ─── Packet Header ──────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  msg_type;          /* One of MSG_* above                  */
    uint16_t seq_num;           /* Sequence number (network byte order)*/
    uint16_t group_id;          /* Multicast group identifier          */
    uint32_t timestamp_sec;     /* Sender timestamp (seconds)          */
    uint32_t timestamp_usec;    /* Sender timestamp (microseconds)     */
    uint16_t payload_len;       /* Length of payload in bytes          */
    uint8_t  checksum;          /* XOR checksum of header bytes        */
    char     payload[MAX_PAYLOAD];
} Packet;

#define HEADER_SIZE  (sizeof(Packet) - MAX_PAYLOAD)
#define PACKET_SIZE  sizeof(Packet)

/* ─── Checksum helpers ───────────────────────────────────────────────── */
static inline uint8_t compute_checksum(const Packet *p) {
    const uint8_t *b = (const uint8_t *)p;
    uint8_t cs = 0;
    /* XOR all header bytes except the checksum field itself (offset 15) */
    for (size_t i = 0; i < HEADER_SIZE; i++) {
        if (i != 15) cs ^= b[i];
    }
    return cs;
}

static inline int verify_checksum(const Packet *p) {
    return compute_checksum(p) == p->checksum;
}

/* ─── Timestamp helpers ──────────────────────────────────────────────── */
static inline void packet_stamp(Packet *p) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    p->timestamp_sec  = htonl((uint32_t)tv.tv_sec);
    p->timestamp_usec = htonl((uint32_t)tv.tv_usec);
}

static inline double packet_age_ms(const Packet *p) {
    struct timeval now;
    gettimeofday(&now, NULL);
    uint32_t s  = ntohl(p->timestamp_sec);
    uint32_t us = ntohl(p->timestamp_usec);
    return (now.tv_sec - s) * 1000.0 + (now.tv_usec - us) / 1000.0;
}

#endif /* PACKET_H */
