#ifndef NETPING_H
#define NETPING_H
// =====================================================================================
//  Network probes using the ESP32's built-in lwIP sockets - no extra library needed.
//  Works on ESP32 Arduino core 2.x and 3.x.
//
//    pingOnceUs()   real ICMP ping (same as the ping command on your PC)
//    probeHopUs()   ping with a limited TTL: the router that many hops away answers with
//                   "time exceeded" - used to find and time the ISP's first router
//    dnsQueryUs()   one DNS lookup sent straight to your DNS server (no local cache)
// =====================================================================================
#include <Arduino.h>
#include <lwip/sockets.h>
#include <lwip/inet_chksum.h>
#include <lwip/prot/ip4.h>
#include <lwip/prot/icmp.h>
#include <esp_system.h>
#if __has_include(<esp_random.h>)
  #include <esp_random.h>
#endif

#ifndef IP_PROTO_ICMP
  #define IP_PROTO_ICMP 1
#endif
#ifndef ICMP_TE
  #define ICMP_TE 11
#endif

static int  pingSock = -1;
static int  pingTtl = 64;
static const uint16_t PING_ID = 0x494D;   // "IM"
static uint16_t pingSeq = 0;

// Sends one echo request with the given TTL. Returns the round trip in microseconds, or -1.
//   ttl 64: only an echo reply from ip4 counts (normal ping)
//   small ttl: a "time exceeded" from the router at that hop counts too; its address -> *from
long probeHopUs(uint32_t ip4, uint32_t timeoutMs, int ttl, uint32_t *from = nullptr) {
  if (pingSock < 0) {
    pingSock = lwip_socket(AF_INET, SOCK_RAW, IP_PROTO_ICMP);
    if (pingSock < 0) return -1;
    pingTtl = -1;
  }
  if (ttl != pingTtl) {
    lwip_setsockopt(pingSock, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl));
    pingTtl = ttl;
  }
  // throw away any late replies from an earlier probe
  {
    uint8_t junk[128];
    struct sockaddr_in f; socklen_t fl = sizeof(f);
    while (lwip_recvfrom(pingSock, junk, sizeof(junk), MSG_DONTWAIT, (struct sockaddr *)&f, &fl) > 0) fl = sizeof(f);
  }

  uint8_t pkt[sizeof(struct icmp_echo_hdr) + 32];
  struct icmp_echo_hdr *h = (struct icmp_echo_hdr *)pkt;
  const uint16_t seq = ++pingSeq;
  ICMPH_TYPE_SET(h, ICMP_ECHO);
  ICMPH_CODE_SET(h, 0);
  h->id = lwip_htons(PING_ID);
  h->seqno = lwip_htons(seq);
  h->chksum = 0;
  for (size_t i = sizeof(struct icmp_echo_hdr); i < sizeof(pkt); i++) pkt[i] = 'a' + (i % 26);
  h->chksum = inet_chksum(pkt, sizeof(pkt));

  struct sockaddr_in to;
  memset(&to, 0, sizeof(to));
  to.sin_family = AF_INET;
  to.sin_addr.s_addr = ip4;

  const uint32_t t0 = micros();
  if (lwip_sendto(pingSock, pkt, sizeof(pkt), 0, (struct sockaddr *)&to, sizeof(to)) <= 0) return -1;

  uint8_t buf[160];
  while (true) {
    uint32_t elapsedMs = (micros() - t0) / 1000;
    if (elapsedMs >= timeoutMs) return -1;
    uint32_t left = timeoutMs - elapsedMs;
    struct timeval tv = { (long)(left / 1000), (long)((left % 1000) * 1000) };
    lwip_setsockopt(pingSock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in src;
    socklen_t fl = sizeof(src);
    int n = lwip_recvfrom(pingSock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &fl);
    if (n <= 0) return -1;                              // timed out
    const uint32_t rtt = micros() - t0;
    if (n < 20) continue;
    int ihl = (buf[0] & 0x0F) * 4;                      // raw sockets include the IP header
    if (n < ihl + 8) continue;
    uint8_t type = buf[ihl];
    if (type == ICMP_ER) {                              // echo reply from the target itself
      struct icmp_echo_hdr *r = (struct icmp_echo_hdr *)(buf + ihl);
      if (src.sin_addr.s_addr != ip4) continue;
      if (r->id != lwip_htons(PING_ID) || r->seqno != lwip_htons(seq)) continue;
      if (from) *from = ip4;
      return (long)rtt;
    }
    if (type == ICMP_TE && ttl < 64) {                  // "time exceeded" from a router on the way
      // payload = our original IP header + the first 8 bytes of our echo request
      const uint8_t *in = buf + ihl + 8;
      int inLen = n - ihl - 8;
      if (inLen < 20) continue;
      int ihl2 = (in[0] & 0x0F) * 4;
      if (inLen < ihl2 + 8) continue;
      const uint8_t *ie = in + ihl2;
      uint16_t id  = (uint16_t)(ie[4] << 8 | ie[5]);
      uint16_t sq  = (uint16_t)(ie[6] << 8 | ie[7]);
      if (ie[0] != ICMP_ECHO || id != PING_ID || sq != seq) continue;
      if (from) *from = src.sin_addr.s_addr;
      return (long)rtt;
    }
  }
}

// Normal ping: microseconds, or -1 if no reply within timeoutMs
long pingOnceUs(uint32_t ip4, uint32_t timeoutMs) { return probeHopUs(ip4, timeoutMs, 64); }

// True for addresses inside a home network (10/8, 172.16/12, 192.168/16, 169.254/16).
// 100.64/10 (carrier-grade NAT) counts as the ISP's side.
bool isPrivateIp(uint32_t ip4) {
  const uint8_t *b = (const uint8_t *)&ip4;          // network order: b[0] is the first number
  return b[0] == 10 || (b[0] == 172 && (b[1] & 0xF0) == 16) || (b[0] == 192 && b[1] == 168) ||
         (b[0] == 169 && b[1] == 254) || b[0] == 0;
}

// One DNS lookup (A record) sent straight to `server` over UDP. Microseconds, or -1 on
// timeout / error. Goes past the ESP32's own DNS cache, so it really times your DNS server.
long dnsQueryUs(uint32_t server, const char *name, uint32_t timeoutMs) {
  int s = lwip_socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0) return -1;
  uint8_t q[300];
  uint16_t id = (uint16_t)esp_random();
  int n = 0;
  q[n++] = id >> 8; q[n++] = id & 0xFF;
  q[n++] = 0x01; q[n++] = 0x00;                       // standard query, recursion desired
  q[n++] = 0; q[n++] = 1;                             // 1 question
  for (int i = 0; i < 6; i++) q[n++] = 0;
  const char *p = name;
  while (*p && n < 280) {
    const char *dot = strchr(p, '.');
    int len = dot ? (int)(dot - p) : (int)strlen(p);
    if (len <= 0 || len > 63) break;
    q[n++] = (uint8_t)len;
    memcpy(q + n, p, len); n += len;
    p += len + (dot ? 1 : 0);
    if (!dot) break;
  }
  q[n++] = 0;
  q[n++] = 0; q[n++] = 1;                             // type A
  q[n++] = 0; q[n++] = 1;                             // class IN

  struct sockaddr_in to;
  memset(&to, 0, sizeof(to));
  to.sin_family = AF_INET;
  to.sin_port = lwip_htons(53);
  to.sin_addr.s_addr = server;
  long result = -1;
  const uint32_t t0 = micros();
  if (lwip_sendto(s, q, n, 0, (struct sockaddr *)&to, sizeof(to)) == n) {
    uint8_t r[512];
    while (true) {
      uint32_t el = (micros() - t0) / 1000;
      if (el >= timeoutMs) break;
      uint32_t left = timeoutMs - el;
      struct timeval tv = { (long)(left / 1000), (long)((left % 1000) * 1000) };
      lwip_setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      int m = lwip_recv(s, r, sizeof(r), 0);
      if (m <= 0) break;
      if (m < 12 || r[0] != (id >> 8) || r[1] != (id & 0xFF) || !(r[2] & 0x80)) continue;
      if ((r[3] & 0x0F) == 0 && (r[6] || r[7])) result = (long)(micros() - t0);   // NOERROR + answers
      break;
    }
  }
  lwip_close(s);
  return result;
}
#endif
