#include <stdint.h>
#include "net.h"
#include "e1000.h"
#include "scheduler.h"
#include "minilib.h"

const uint8_t NET_OUR_IP[4] = { 10, 0, 2, 15 };
const uint8_t NET_GATEWAY_IP[4] = { 10, 0, 2, 2 };

#define ETHERTYPE_ARP  0x0806u
#define ETHERTYPE_IPV4 0x0800u
#define IP_PROTO_ICMP  1u
#define ARP_OPER_REQUEST 1u
#define ARP_OPER_REPLY   2u
#define ICMP_TYPE_ECHO_REQUEST 8u
#define ICMP_TYPE_ECHO_REPLY   0u

#define MAX_FRAME_SIZE 1518u
#define ICMP_PAYLOAD_SIZE 32u

/* This driver never needs anything past ARP/ICMP, so both requests are
 * retried a bounded number of times rather than sent once and hoped
 * for -- a dropped frame (or the gateway just not having answered yet)
 * is a normal, expected outcome here, not a bug, so these functions
 * return 0 on timeout rather than panicking; the caller decides
 * whether that failure matters. */
#define RETRY_COUNT 20u
#define SPINS_PER_RETRY 2000000ULL

static uint16_t htons16(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}

static uint16_t checksum16(const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += (uint16_t)((p[0] << 8) | p[1]);
        p += 2;
        len -= 2;
    }
    if (len == 1) {
        sum += (uint16_t)(p[0] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

typedef struct __attribute__((packed)) {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t ethertype;
} eth_header_t;

typedef struct __attribute__((packed)) {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t oper;
    uint8_t  sha[6];
    uint8_t  spa[4];
    uint8_t  tha[6];
    uint8_t  tpa[4];
} arp_packet_t;

typedef struct __attribute__((packed)) {
    uint8_t  version_ihl;
    uint8_t  dscp_ecn;
    uint16_t total_length;
    uint16_t id;
    uint16_t flags_fragoffset;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint8_t  src_ip[4];
    uint8_t  dst_ip[4];
} ipv4_header_t;

typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t identifier;
    uint16_t sequence;
} icmp_header_t;

static uint8_t our_mac[6];
static const uint8_t BROADCAST_MAC[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

void net_init(void) {
    e1000_get_mac(our_mac);
}

int net_arp_resolve(const uint8_t ip[4], uint8_t out_mac[6]) {
    uint8_t request[sizeof(eth_header_t) + sizeof(arp_packet_t)];
    eth_header_t *eth = (eth_header_t *)request;
    arp_packet_t *arp = (arp_packet_t *)(request + sizeof(eth_header_t));

    memcpy(eth->dst, BROADCAST_MAC, 6);
    memcpy(eth->src, our_mac, 6);
    eth->ethertype = htons16(ETHERTYPE_ARP);

    arp->htype = htons16(1);
    arp->ptype = htons16(ETHERTYPE_IPV4);
    arp->hlen = 6;
    arp->plen = 4;
    arp->oper = htons16(ARP_OPER_REQUEST);
    memcpy(arp->sha, our_mac, 6);
    memcpy(arp->spa, NET_OUR_IP, 4);
    memset(arp->tha, 0, 6);
    memcpy(arp->tpa, ip, 4);

    uint8_t reply_buf[MAX_FRAME_SIZE];
    for (uint32_t retry = 0; retry < RETRY_COUNT; retry++) {
        e1000_send(request, sizeof(request));

        for (uint64_t spins = 0; spins < SPINS_PER_RETRY; spins++) {
            uint16_t len = e1000_poll_receive(reply_buf, sizeof(reply_buf));
            if (len >= sizeof(eth_header_t) + sizeof(arp_packet_t)) {
                const eth_header_t *reth = (const eth_header_t *)reply_buf;
                const arp_packet_t *rarp = (const arp_packet_t *)(reply_buf + sizeof(eth_header_t));
                if (reth->ethertype == htons16(ETHERTYPE_ARP) &&
                    rarp->oper == htons16(ARP_OPER_REPLY) &&
                    memcmp(rarp->spa, ip, 4) == 0) {
                    memcpy(out_mac, rarp->sha, 6);
                    return 1;
                }
            }
            schedule();
        }
    }
    return 0;
}

int net_ping(const uint8_t ip[4], const uint8_t mac[6], uint16_t identifier, uint16_t sequence) {
    uint8_t packet[sizeof(eth_header_t) + sizeof(ipv4_header_t) + sizeof(icmp_header_t) + ICMP_PAYLOAD_SIZE];
    eth_header_t *eth = (eth_header_t *)packet;
    ipv4_header_t *ip4 = (ipv4_header_t *)(packet + sizeof(eth_header_t));
    icmp_header_t *icmp = (icmp_header_t *)(packet + sizeof(eth_header_t) + sizeof(ipv4_header_t));
    uint8_t *payload = packet + sizeof(eth_header_t) + sizeof(ipv4_header_t) + sizeof(icmp_header_t);

    memcpy(eth->dst, mac, 6);
    memcpy(eth->src, our_mac, 6);
    eth->ethertype = htons16(ETHERTYPE_IPV4);

    ip4->version_ihl = 0x45;
    ip4->dscp_ecn = 0;
    ip4->total_length = htons16((uint16_t)(sizeof(ipv4_header_t) + sizeof(icmp_header_t) + ICMP_PAYLOAD_SIZE));
    ip4->id = htons16(1);
    ip4->flags_fragoffset = 0;
    ip4->ttl = 64;
    ip4->protocol = IP_PROTO_ICMP;
    ip4->checksum = 0;
    memcpy(ip4->src_ip, NET_OUR_IP, 4);
    memcpy(ip4->dst_ip, ip, 4);
    ip4->checksum = htons16(checksum16(ip4, sizeof(ipv4_header_t)));

    icmp->type = ICMP_TYPE_ECHO_REQUEST;
    icmp->code = 0;
    icmp->checksum = 0;
    icmp->identifier = htons16(identifier);
    icmp->sequence = htons16(sequence);
    for (uint32_t i = 0; i < ICMP_PAYLOAD_SIZE; i++) {
        payload[i] = (uint8_t)('A' + (i % 26));
    }
    icmp->checksum = htons16(checksum16(icmp, sizeof(icmp_header_t) + ICMP_PAYLOAD_SIZE));

    uint8_t reply_buf[MAX_FRAME_SIZE];
    for (uint32_t retry = 0; retry < RETRY_COUNT; retry++) {
        e1000_send(packet, sizeof(packet));

        for (uint64_t spins = 0; spins < SPINS_PER_RETRY; spins++) {
            uint16_t len = e1000_poll_receive(reply_buf, sizeof(reply_buf));
            if (len >= sizeof(eth_header_t) + sizeof(ipv4_header_t) + sizeof(icmp_header_t) + ICMP_PAYLOAD_SIZE) {
                const eth_header_t *reth = (const eth_header_t *)reply_buf;
                const ipv4_header_t *rip4 = (const ipv4_header_t *)(reply_buf + sizeof(eth_header_t));
                const icmp_header_t *ricmp =
                    (const icmp_header_t *)(reply_buf + sizeof(eth_header_t) + sizeof(ipv4_header_t));
                const uint8_t *rpayload =
                    reply_buf + sizeof(eth_header_t) + sizeof(ipv4_header_t) + sizeof(icmp_header_t);

                if (reth->ethertype == htons16(ETHERTYPE_IPV4) &&
                    rip4->protocol == IP_PROTO_ICMP &&
                    memcmp(rip4->src_ip, ip, 4) == 0 &&
                    ricmp->type == ICMP_TYPE_ECHO_REPLY &&
                    ricmp->identifier == htons16(identifier) &&
                    ricmp->sequence == htons16(sequence) &&
                    memcmp(rpayload, payload, ICMP_PAYLOAD_SIZE) == 0) {
                    return 1;
                }
            }
            schedule();
        }
    }
    return 0;
}
