/* UDP transport for netplay (see net.h). */
#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define SOCK_BAD INVALID_SOCKET
#define sock_close closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_BAD (-1)
#define sock_close close
#endif

#define QMAX 1024
typedef struct { u64 due_ns; int len; u8 data[1500]; } QPkt;

struct NetSock {
    sock_t fd;
    struct sockaddr_storage peer;
    socklen_t peer_len;
    bool have_peer;
    NetSim sim;
    bool simulate;
    QPkt *q;
    int qn;
    u64 rng;
    char name[128];
};

static u64 mono_ns(void) {
#if defined(_WIN32)
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (u64)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
#endif
}

void net_sleep_us(u64 us) {
#if defined(_WIN32)
    Sleep((DWORD)((us + 999) / 1000));
#else
    struct timespec ts = { (time_t)(us / 1000000), (long)(us % 1000000) * 1000 };
    nanosleep(&ts, NULL);
#endif
}

static double rnd01(NetSock *s) {   /* xorshift64*: the simulated network is reproducible per seed */
    s->rng ^= s->rng >> 12; s->rng ^= s->rng << 25; s->rng ^= s->rng >> 27;
    return (double)((s->rng * 0x2545f4914f6cdd1dull) >> 11) / 9007199254740992.0;
}

static void set_name(NetSock *s) {
    char h[64] = "?";
    int port = 0;
    if (s->peer.ss_family == AF_INET) {
        struct sockaddr_in *a = (struct sockaddr_in *)&s->peer;
        inet_ntop(AF_INET, &a->sin_addr, h, sizeof h);
        port = ntohs(a->sin_port);
    } else if (s->peer.ss_family == AF_INET6) {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)&s->peer;
        inet_ntop(AF_INET6, &a->sin6_addr, h, sizeof h);
        port = ntohs(a->sin6_port);
    }
    snprintf(s->name, sizeof s->name, "%s:%d", h, port);
}

NetSock *net_open(const char *host, int port, int bind_port, const NetSim *sim) {
#if defined(_WIN32)
    static bool wsa;
    if (!wsa) { WSADATA d; if (WSAStartup(MAKEWORD(2, 2), &d)) return NULL; wsa = true; }
#endif
    NetSock *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    int family = AF_INET;
    if (host) {
        struct addrinfo hints = {0}, *res = NULL;
        hints.ai_family = strchr(host, ':') ? AF_INET6 : AF_INET;   /* the host listens on IPv4 */
        hints.ai_socktype = SOCK_DGRAM;
        char ps[16];
        snprintf(ps, sizeof ps, "%d", port);
        if (getaddrinfo(host, ps, &hints, &res) || !res) { fprintf(stderr, "net: cannot resolve %s\n", host); free(s); return NULL; }
        memcpy(&s->peer, res->ai_addr, res->ai_addrlen);
        s->peer_len = (socklen_t)res->ai_addrlen;
        family = res->ai_family;
        s->have_peer = true;
        freeaddrinfo(res);
        set_name(s);
    }
    s->fd = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (s->fd == SOCK_BAD) { free(s); return NULL; }
    int bp = host ? bind_port : port;
    if (family == AF_INET6) {
        struct sockaddr_in6 a = {0};
        a.sin6_family = AF_INET6; a.sin6_port = htons((u16)bp); a.sin6_addr = in6addr_any;
        if (bind(s->fd, (struct sockaddr *)&a, sizeof a)) { perror("net: bind"); sock_close(s->fd); free(s); return NULL; }
    } else {
        struct sockaddr_in a = {0};
        a.sin_family = AF_INET; a.sin_port = htons((u16)bp); a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(s->fd, (struct sockaddr *)&a, sizeof a)) { perror("net: bind"); sock_close(s->fd); free(s); return NULL; }
    }
#if defined(_WIN32)
    u_long nb = 1;
    ioctlsocket(s->fd, FIONBIO, &nb);
#else
    fcntl(s->fd, F_SETFL, fcntl(s->fd, F_GETFL) | O_NONBLOCK);
#endif
    if (sim) s->sim = *sim;
    s->simulate = s->sim.latency_ms > 0 || s->sim.jitter_ms > 0 || s->sim.loss_pct > 0 || s->sim.reorder_pct > 0;
    s->rng = 0x9e3779b97f4a7c15ull ^ ((u64)s->sim.seed * 0xbf58476d1ce4e5b9ull) ^ (host ? 1 : 2);
    if (s->simulate) s->q = calloc(QMAX, sizeof *s->q);
    return s;
}

void net_close(NetSock *s) {
    if (!s) return;
    sock_close(s->fd);
    free(s->q);
    free(s);
}

static void raw_send(NetSock *s, const void *buf, int len) {
    if (!s->have_peer) return;
    sendto(s->fd, buf, (size_t)len, 0, (struct sockaddr *)&s->peer, s->peer_len);
}

void net_send(NetSock *s, const void *buf, int len) {
    if (len <= 0 || len > 1500) return;
    if (!s->simulate) { raw_send(s, buf, len); return; }
    if (rnd01(s) * 100 < s->sim.loss_pct) return;
    double d = s->sim.latency_ms + rnd01(s) * s->sim.jitter_ms;
    if (rnd01(s) * 100 < s->sim.reorder_pct) d += s->sim.latency_ms + 10;
    if (s->qn == QMAX) return;   /* a full queue is a lost packet */
    QPkt *p = &s->q[s->qn++];
    p->due_ns = mono_ns() + (u64)(d * 1e6);
    p->len = len;
    memcpy(p->data, buf, (size_t)len);
    net_poll(s);
}

void net_poll(NetSock *s) {
    if (!s->simulate || !s->qn) return;
    u64 now = mono_ns();
    int k = 0;
    for (int i = 0; i < s->qn; i++) {
        if (s->q[i].due_ns <= now) raw_send(s, s->q[i].data, s->q[i].len);
        else { if (k != i) s->q[k] = s->q[i]; k++; }
    }
    s->qn = k;
}

void net_flush(NetSock *s, int max_ms) {
    u64 end = mono_ns() + (u64)max_ms * 1000000ull;
    while (s->qn && mono_ns() < end) { net_poll(s); net_sleep_us(1000); }
}

static bool same_addr(const struct sockaddr_storage *a, const struct sockaddr_storage *b) {
    if (a->ss_family != b->ss_family) return false;
    if (a->ss_family == AF_INET) {
        const struct sockaddr_in *x = (const void *)a, *y = (const void *)b;
        return x->sin_port == y->sin_port && x->sin_addr.s_addr == y->sin_addr.s_addr;
    }
    const struct sockaddr_in6 *x = (const void *)a, *y = (const void *)b;
    return x->sin6_port == y->sin6_port && !memcmp(&x->sin6_addr, &y->sin6_addr, sizeof x->sin6_addr);
}

int net_recv(NetSock *s, void *buf, int cap) {
    net_poll(s);
    for (;;) {
        struct sockaddr_storage from;
        socklen_t fl = sizeof from;
        long n = (long)recvfrom(s->fd, buf, (size_t)cap, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) return 0;
        if (!s->have_peer) {   /* host: the first sender becomes the peer */
            memcpy(&s->peer, &from, fl);
            s->peer_len = fl;
            s->have_peer = true;
            set_name(s);
        } else if (!same_addr(&from, &s->peer)) {
            continue;          /* not our peer */
        }
        return (int)n;
    }
}

const char *net_peer_name(NetSock *s) { return s->have_peer ? s->name : "(none)"; }
