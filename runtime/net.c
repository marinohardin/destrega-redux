/* UDP transport for netplay (see net.h). */
#include "net.h"
#include "platform.h"
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
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_BAD (-1)
#define sock_close close
#endif

#define QMAX 1024
typedef struct { u64 due_ns; int len; u8 data[1500]; } QPkt;
/* Received datagrams waiting to be handed out, with their arrival time. A receiver thread per socket
 * waits on it (select; Windows: an event, see wait_readable) and queues each datagram the moment it arrives, so the timestamp does not
 * depend on what the main thread is doing (emulating, presenting, sleeping). The main thread still
 * takes them out only where it processes packets (net_recv / net_recv_ts), exactly as before. */
#define RMAX 256
typedef struct { u64 at_ns; struct sockaddr_storage from; socklen_t fl; int len; u8 data[1500]; } RPkt;

struct NetSock {
    sock_t fd;
    struct sockaddr_storage peer;
    socklen_t peer_len;
    bool have_peer;
    bool locked;           /* host: peer fixed (after a valid hello); before, the last sender */
    NetSim sim;
    bool simulate;
    QPkt *q;
    int qn;
    u64 rng;
    char name[128];
    RPkt *rq;              /* ring of received datagrams */
    int rq_head, rq_n;
    plat_mutex mu;         /* rq_head, rq_n, the slots in use, rx_stop */
    plat_thread rx;
    bool rx_on, rx_stop;   /* receiver thread running / asked to stop */
#if defined(_WIN32)
    WSAEVENT ev;           /* FD_READ (WSAEventSelect) */
    HANDLE timer;          /* high-resolution waitable timer: the receiver thread's timeout */
#endif
};

static u64 mono_ns(void) { return plat_mono_ns(); }
static void rx_thread(void *arg);
void net_sleep_us(u64 us) { plat_sleep_us(us); }

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
        s->locked = true;
        freeaddrinfo(res);
        set_name(s);
    }
#if defined(_WIN32)
    /* not inheritable: a menu-started session restarts by CreateProcess with inherited handles
     * (plat_exec), and the child binds the same port. (SetHandleInformation on the socket instead
     * stopped a bound socket from receiving anything under Wine.) */
    s->fd = WSASocketW(family, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
#else
    s->fd = socket(family, SOCK_DGRAM, IPPROTO_UDP);
#endif
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
    fcntl(s->fd, F_SETFD, FD_CLOEXEC);   /* a re-exec (menu netplay start) must free the port */
    int on = 1;
    setsockopt(s->fd, SOL_SOCKET, SO_TIMESTAMP, &on, sizeof on);   /* kernel arrival times (read_one) */
#endif
    if (sim) s->sim = *sim;
    s->simulate = s->sim.latency_ms > 0 || s->sim.jitter_ms > 0 || s->sim.loss_pct > 0 || s->sim.reorder_pct > 0;
    s->rng = 0x9e3779b97f4a7c15ull ^ ((u64)s->sim.seed * 0xbf58476d1ce4e5b9ull) ^ (host ? 1 : 2);
    if (s->simulate) s->q = calloc(QMAX, sizeof *s->q);
    s->mu = (plat_mutex)PLAT_MUTEX_INIT;
    s->rq = calloc(RMAX, sizeof *s->rq);
#if defined(_WIN32)
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    s->ev = WSACreateEvent();
    if (s->ev != WSA_INVALID_EVENT && WSAEventSelect(s->fd, s->ev, FD_READ)) { WSACloseEvent(s->ev); s->ev = WSA_INVALID_EVENT; }
    s->timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!s->timer) s->timer = CreateWaitableTimerW(NULL, FALSE, NULL);   /* older Windows: coarser */
#endif
    /* without the thread (cannot start) the main thread reads the socket itself, as before:
     * packets are then timestamped when it looks (net_drain at a few points of each frame) */
    if (s->rq) s->rx_on = plat_thread_start(&s->rx, rx_thread, s);
    return s;
}

void net_close(NetSock *s) {
    if (!s) return;
    if (s->rx_on) {            /* the thread notices within one wait timeout (20 ms) */
        plat_mutex_lock(&s->mu); s->rx_stop = true; plat_mutex_unlock(&s->mu);
        plat_thread_join(s->rx);
    }
    sock_close(s->fd);
#if defined(_WIN32)
    if (s->ev != WSA_INVALID_EVENT) WSACloseEvent(s->ev);
    if (s->timer) CloseHandle(s->timer);
#endif
    free(s->q);
    free(s->rq);
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
    plat_mutex_lock(&s->mu);
    if (s->qn < QMAX) {          /* a full queue is a lost packet */
        QPkt *p = &s->q[s->qn++];
        p->due_ns = mono_ns() + (u64)(d * 1e6);
        p->len = len;
        memcpy(p->data, buf, (size_t)len);
    }
    plat_mutex_unlock(&s->mu);
    net_poll(s);
}

/* Simulated packets leave when they are due: the receiver thread looks every 0.5 ms while a
 * simulated link is in use, so the latency does not depend on when the main thread gets here. */
void net_poll(NetSock *s) {
    if (!s->simulate) return;
    plat_mutex_lock(&s->mu);
    if (s->qn) {
        u64 now = mono_ns();
        int k = 0;
        for (int i = 0; i < s->qn; i++) {
            if (s->q[i].due_ns <= now) raw_send(s, s->q[i].data, s->q[i].len);
            else { if (k != i) s->q[k] = s->q[i]; k++; }
        }
        s->qn = k;
    }
    plat_mutex_unlock(&s->mu);
}

static int queued(NetSock *s) {
    plat_mutex_lock(&s->mu);
    int n = s->qn;
    plat_mutex_unlock(&s->mu);
    return n;
}

void net_flush(NetSock *s, int max_ms) {
    u64 end = mono_ns() + (u64)max_ms * 1000000ull;
    while (queued(s) && mono_ns() < end) { net_poll(s); net_sleep_us(1000); }
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

/* Read one datagram that is ready into the queue (caller made sure there is room). Its arrival
 * time is the kernel's receive timestamp where there is one (SO_TIMESTAMP, macOS / Linux: immune
 * to the receiver thread waking late on a loaded machine), else the time it was read. */
static bool read_one(NetSock *s, RPkt *p) {
    p->fl = sizeof p->from;
#if defined(_WIN32)
    long n = (long)recvfrom(s->fd, (void *)p->data, sizeof p->data, 0, (struct sockaddr *)&p->from, &p->fl);
    if (n <= 0) return false;  /* nothing, or an error such as WSAECONNRESET for an earlier send */
    p->len = (int)n;
    p->at_ns = mono_ns();
#else
    struct iovec iov = { p->data, sizeof p->data };
    union { struct cmsghdr h; u8 b[CMSG_SPACE(sizeof(struct timeval)) + 64]; } ctl;
    struct msghdr m = {0};
    m.msg_name = &p->from; m.msg_namelen = p->fl;
    m.msg_iov = &iov; m.msg_iovlen = 1;
    m.msg_control = ctl.b; m.msg_controllen = sizeof ctl.b;
    long n = (long)recvmsg(s->fd, &m, 0);
    if (n <= 0) return false;
    u64 now = mono_ns();
    p->fl = m.msg_namelen;
    p->len = (int)n;
    p->at_ns = now;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_TIMESTAMP) continue;
        struct timeval tv;
        memcpy(&tv, CMSG_DATA(c), sizeof tv);
        struct timespec rt;
        clock_gettime(CLOCK_REALTIME, &rt);
        s64 age = ((s64)rt.tv_sec - (s64)tv.tv_sec) * 1000000000ll + (s64)rt.tv_nsec - (s64)tv.tv_usec * 1000ll;
        if (age >= 0 && age < 1000000000ll) p->at_ns = now - (u64)age;   /* the wall clock only measures the age */
    }
#endif
    return true;
}

/* Wait up to `us` for a datagram. Windows: the socket's FD_READ event or a high-resolution timer.
 * Not select(): with a select() loop here, Mac <-> Wine sessions fell to ~1 fps with multi-second
 * round trips after a few seconds (cause not pinned down; likely Wine serialising the pending
 * select with the main thread's calls on the same socket). The event wait is not a socket call
 * and does not show it. */
static bool wait_readable(NetSock *s, u64 us) {
#if defined(_WIN32)
    if (s->ev != WSA_INVALID_EVENT && s->timer) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(us * 10);   /* relative, 100 ns units */
        if (SetWaitableTimer(s->timer, &due, 0, NULL, NULL, FALSE)) {
            HANDLE h[2] = { s->ev, s->timer };
            DWORD r = WaitForMultipleObjects(2, h, FALSE, INFINITE);
            if (r != WAIT_OBJECT_0) return false;
            WSAResetEvent(s->ev);   /* before reading: a datagram arriving after the reads sets it again */
            CancelWaitableTimer(s->timer);
            return true;
        }
    }
    plat_sleep_us(us < 1000 ? us : 1000);   /* no event / timer: poll */
    return true;
#else
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(s->fd, &rd);
    struct timeval tv = { (time_t)(us / 1000000), (int)(us % 1000000) };
    return select(s->fd + 1, &rd, NULL, NULL, &tv) > 0;
#endif
}

static void rx_thread(void *arg) {
    NetSock *s = arg;
    plat_thread_boost();       /* timestamps should not wait for a busy core */
    RPkt *tmp = malloc(sizeof *tmp);
    if (!tmp) return;
    for (;;) {
        plat_mutex_lock(&s->mu);
        bool stop = s->rx_stop, full = s->rq_n >= RMAX;
        plat_mutex_unlock(&s->mu);
        if (stop) break;
        net_poll(s);           /* --net-sim: send what is due */
        if (full) { plat_sleep_us(500); continue; }   /* the main thread is not taking them: leave them in the socket */
        /* the timeout only bounds how long net_close waits, or how late a simulated send can be */
        if (!wait_readable(s, s->simulate ? 500 : 20000)) continue;
        int got = 0;
        while (read_one(s, tmp)) {   /* everything that is there (non-blocking socket) */
            got++;
            plat_mutex_lock(&s->mu);
            s->rq[(s->rq_head + s->rq_n) % RMAX] = *tmp;
            s->rq_n++;
            bool more = s->rq_n < RMAX;
            plat_mutex_unlock(&s->mu);
            if (!more) break;
        }
        if (!got) plat_sleep_us(100);   /* readable but nothing read (an error): do not spin on it */
    }
    free(tmp);
}

void net_drain(NetSock *s) {
    if (s->rx_on || !s->rq) return;   /* the receiver thread does it as they arrive */
    while (s->rq_n < RMAX && read_one(s, &s->rq[(s->rq_head + s->rq_n) % RMAX])) s->rq_n++;
}

/* host: answer the last sender until a valid hello locks the peer; false = not our peer */
static bool accept_from(NetSock *s, const struct sockaddr_storage *from, socklen_t fl) {
    if (!s->locked) {          /* under the lock: the thread's simulated sends read the address */
        plat_mutex_lock(&s->mu);
        memcpy(&s->peer, from, fl);
        s->peer_len = fl;
        s->have_peer = true;
        set_name(s);
        plat_mutex_unlock(&s->mu);
        return true;
    }
    return same_addr(from, &s->peer);
}

int net_recv_ts(NetSock *s, void *buf, int cap, u64 *arrived_ns) {
    net_poll(s);
    net_drain(s);
    if (!s->rq) return 0;
    for (;;) {
        RPkt *p;
        plat_mutex_lock(&s->mu);
        if (!s->rq_n) { plat_mutex_unlock(&s->mu); return 0; }
        p = &s->rq[s->rq_head];
        plat_mutex_unlock(&s->mu);
        /* the slot is ours until rq_head moves past it: the thread only writes behind rq_n */
        bool ok = accept_from(s, &p->from, p->fl);
        int n = p->len < cap ? p->len : cap;
        if (ok) { memcpy(buf, p->data, (size_t)n); if (arrived_ns) *arrived_ns = p->at_ns; }
        plat_mutex_lock(&s->mu);
        s->rq_head = (s->rq_head + 1) % RMAX;
        s->rq_n--;
        plat_mutex_unlock(&s->mu);
        if (ok) return n;      /* else: not our peer, next */
    }
}

int net_recv(NetSock *s, void *buf, int cap) { return net_recv_ts(s, buf, cap, NULL); }

bool net_pending(NetSock *s) {
    plat_mutex_lock(&s->mu);
    bool r = s->rq_n > 0;
    plat_mutex_unlock(&s->mu);
    return r;
}

void net_wait(NetSock *s, u64 deadline_ns, bool until_packet) {
    for (;;) {
        net_poll(s);
        net_drain(s);
        if (until_packet && net_pending(s)) return;
        u64 now = mono_ns();
        if (now >= deadline_ns) return;
        u64 wake = deadline_ns;
        /* look again every 0.5 ms for a packet, or without the thread for simulated sends too */
        if ((until_packet || (!s->rx_on && s->simulate)) && wake > now + 500000) wake = now + 500000;
        plat_sleep_us((wake - now + 999) / 1000);
    }
}

const char *net_peer_name(NetSock *s) { return s->have_peer ? s->name : "(none)"; }
void net_lock_peer(NetSock *s) { if (s->have_peer) s->locked = true; }

int net_local_addresses(char *out, size_t n) { return plat_local_ipv4(out, n); }
