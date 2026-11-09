/* Minimal UDP peer-to-peer transport for netplay, with an optional simulated network on the
 * send side (latency, jitter, loss, reordering) for offline tests over loopback. */
#ifndef NET_H
#define NET_H
#include "psx.h"

typedef struct {
    double latency_ms, jitter_ms;   /* one-way delay: latency + uniform [0, jitter) */
    double loss_pct, reorder_pct;   /* dropped packets; packets held back an extra latency + 10 ms */
    u32 seed;
} NetSim;

typedef struct NetSock NetSock;

/* host == NULL: listen on bind_port, the peer is whoever sends first.
 * Otherwise send to host:port, bound to bind_port (0 = any). */
NetSock *net_open(const char *host, int port, int bind_port, const NetSim *sim);
void net_close(NetSock *s);
void net_send(NetSock *s, const void *buf, int len);    /* queued if a simulated delay applies */
int net_recv(NetSock *s, void *buf, int cap);           /* > 0 bytes, 0 nothing, never blocks */
/* The same, plus when the datagram arrived (plat_mono_ns clock). A receiver thread per socket
 * reads each datagram as soon as it arrives and queues it with that time; the caller still gets
 * the packets only when it asks, in arrival order, so where they are processed does not change.
 * (If the thread cannot start, the caller's thread reads the socket: net_drain / net_wait.) */
int net_recv_ts(NetSock *s, void *buf, int cap, u64 *arrived_ns);
void net_drain(NetSock *s);                             /* no thread: queue what is there now */
bool net_pending(NetSock *s);                           /* a received datagram is waiting */
/* Sleep until deadline_ns (plat_mono_ns clock); until_packet: return as soon as a datagram is
 * waiting (checked every 0.5 ms). Simulated packets (NetSim) are sent when they are due by the
 * receiver thread (without it, by this wait). */
void net_wait(NetSock *s, u64 deadline_ns, bool until_packet);
void net_poll(NetSock *s);                              /* send due simulated packets */
void net_flush(NetSock *s, int max_ms);                 /* send everything still queued */
const char *net_peer_name(NetSock *s);
void net_lock_peer(NetSock *s);                         /* host: only the current peer from now on */
/* This machine's IPv4 addresses (no loopback), comma-separated; returns how many. */
int net_local_addresses(char *out, size_t n);
void net_sleep_us(u64 us);
#endif
