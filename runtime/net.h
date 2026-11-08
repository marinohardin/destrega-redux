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
void net_poll(NetSock *s);                              /* send due simulated packets */
void net_flush(NetSock *s, int max_ms);                 /* send everything still queued */
const char *net_peer_name(NetSock *s);
void net_lock_peer(NetSock *s);                         /* host: only the current peer from now on */
/* This machine's IPv4 addresses (no loopback), comma-separated; returns how many. */
int net_local_addresses(char *out, size_t n);
void net_sleep_us(u64 us);
#endif
