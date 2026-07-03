#ifndef REBORNOS_NET_H
#define REBORNOS_NET_H

#include <stdint.h>

/* The thinnest possible network stack: Ethernet framing, ARP, and
 * ICMP echo (ping) -- enough to prove RebornOS can actually talk to
 * the outside world over e1000.c, not a general-purpose stack. No
 * DHCP: our own IP is a fixed constant matching the address QEMU's
 * SLIRP user-mode networking expects a single guest at (see
 * net_init()'s comment). No UDP/TCP yet -- that's future work once
 * there's a reason to need it. */

/* Our own IP address, hardcoded rather than learned via DHCP: QEMU's
 * `-netdev user` (SLIRP) NATs a private 10.0.2.0/24 network with the
 * gateway at 10.0.2.2 and (when its own DHCP server is used) hands out
 * 10.0.2.15 to the first client. Since this stack only ever runs
 * against that one specific, well-known test network, hardcoding the
 * address it would have gotten anyway is simpler than implementing a
 * DHCP client purely to arrive at the same fixed value. */
extern const uint8_t NET_OUR_IP[4];
extern const uint8_t NET_GATEWAY_IP[4];

/* Reads our own MAC from e1000.c and does no protocol work of its own
 * -- e1000_init() must already have run. */
void net_init(void);

/* Resolves `ip`'s MAC address via ARP, retrying the request a bounded
 * number of times while polling for a reply. Returns 1 and fills
 * out_mac on success, 0 if nothing answered. */
int net_arp_resolve(const uint8_t ip[4], uint8_t out_mac[6]);

/* Sends one ICMP echo request to `ip` (whose MAC must already be
 * known -- see net_arp_resolve()) with the given identifier/sequence,
 * then polls for a matching echo reply. Returns 1 if a reply with the
 * same identifier/sequence and payload came back, 0 on timeout. */
int net_ping(const uint8_t ip[4], const uint8_t mac[6], uint16_t identifier, uint16_t sequence);

#endif /* REBORNOS_NET_H */
