/* Automatic port forwarding for the netplay host: PCP / NAT-PMP / UPnP IGD (see portmap.h).
 *
 * Threading: one worker thread owns every socket and the mapping itself. The main thread only
 * changes the request (port wanted, exit, keep) and reads the published result under s_m; it
 * never waits for the network, except portmap_finish at exit (bounded). Every network wait is a
 * few seconds at most and checks the request between retries, so a cancel is seen quickly.
 *
 * UPnP: a small client written for this (SSDP M-SEARCH, HTTP/1.1 GET of the description, SOAP
 * POST), not miniupnpc: it needs four actions of one service, and keeping it here means no
 * third-party code with its own blocking timeouts on the four build targets. Real routers'
 * quirks it handles: URLBase or relative control URLs, chunked replies, namespace prefixes,
 * WANIPConnection:1/:2 and WANPPPConnection:1, error 725 (only permanent leases: retried with
 * lease 0 and deleted at the end) and 718 (port taken: AddAnyPortMapping on IGD v2, else the next
 * few ports). */
#include "portmap.h"
#include "platform.h"
#include "settings.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET sock_t;
#define SOCK_BAD INVALID_SOCKET
#define sock_close closesocket
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_BAD (-1)
#define sock_close close
#endif

#define PM_LOG(...) do { fprintf(stderr, "portmap: " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define KEEP_ENV "DESTREGA_PORTMAP_KEEP"
#define DESCRIPTION "Destrega netplay"

/* ---- options --------------------------------------------------------------------------------- */
static struct {
    int flag;              /* --net-portmap: -1 not given, 0 off, 1 on */
    bool headless;
    char gw[128];          /* --net-portmap-gateway HOST:PORT (PCP / NAT-PMP server) */
    char upnp[512];        /* --net-portmap-upnp URL (root device description) */
    unsigned lease;        /* seconds asked for */
} o = { -1, false, "", "", 3 * 3600 };

int portmap_parse_arg(int argc, char **argv, int i) {
    const char *a = argv[i];
    bool has = i + 1 < argc;
    static bool scanned;
    if (!scanned) {
        scanned = true;
        for (int k = 1; k < argc; k++) if (!strcmp(argv[k], "--headless")) o.headless = true;
    }
    if (!strcmp(a, "--net-portmap") && has) {
        o.flag = !strcmp(argv[i + 1], "on") || !strcmp(argv[i + 1], "1");
        settings_lock("netplay", "port_mapping");
        return 2;
    }
    if (!strcmp(a, "--net-portmap-gateway") && has) { snprintf(o.gw, sizeof o.gw, "%s", argv[i + 1]); return 2; }
    if (!strcmp(a, "--net-portmap-upnp") && has) { snprintf(o.upnp, sizeof o.upnp, "%s", argv[i + 1]); return 2; }
    if (!strcmp(a, "--net-portmap-lease") && has) {
        long v = atol(argv[i + 1]);
        o.lease = v < 2 ? 2 : v > 86400 ? 86400 : (unsigned)v;
        return 2;
    }
    return 0;
}

void portmap_usage(void) {
    fprintf(stderr,
        "  --net-portmap on|off  host: ask the router to forward the port (default: on in a window,\n"
        "                        [netplay] port_mapping; headless runs never map unless pointed at a\n"
        "                        test router with the options below)\n"
        "  --net-portmap-gateway HOST:PORT  test: PCP / NAT-PMP server to use instead of the gateway\n"
        "  --net-portmap-upnp URL  test: UPnP device description to use instead of SSDP discovery\n"
        "  --net-portmap-lease S lease to ask for in seconds (default 10800, renewed at half)\n");
}

static bool test_mode(void) { return o.gw[0] || o.upnp[0]; }

/* ---- small helpers --------------------------------------------------------------------------- */
static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static unsigned get16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

int portmap_classify_ipv4(const char *ip) {
    unsigned a, b, c, d;
    char tail;
    if (!ip || sscanf(ip, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
        return PM_ADDR_INVALID;
    if (a == 100 && (b & 0xc0) == 64) return PM_ADDR_CGNAT;                       /* 100.64/10 */
    if (a == 10 || (a == 172 && (b & 0xf0) == 16) || (a == 192 && b == 168)) return PM_ADDR_PRIVATE;
    if (a == 0 || a == 127 || (a == 169 && b == 254) || (a == 192 && b == 0 && c == 0) ||
        (a == 198 && (b & 0xfe) == 18) || a >= 224)
        return PM_ADDR_INVALID;
    return PM_ADDR_PUBLIC;
}

/* ---- NAT-PMP (RFC 6886) ---------------------------------------------------------------------- */
int portmap_natpmp_addr_req(uint8_t *b) { b[0] = 0; b[1] = 0; return 2; }

int portmap_natpmp_parse_addr(const uint8_t *b, int n, int *result, char *ip, size_t ipn) {
    if (n < 8 || b[0] != 0 || b[1] != 128) return -1;
    *result = (int)get16(b + 2);
    if (*result == 0) {
        if (n < 12) return -1;
        snprintf(ip, ipn, "%u.%u.%u.%u", b[8], b[9], b[10], b[11]);
    }
    return 0;
}

int portmap_natpmp_map_req(uint8_t *b, int port, int ext, uint32_t lifetime) {
    b[0] = 0; b[1] = 1;        /* version 0, map UDP */
    put16(b + 2, 0);
    put16(b + 4, (unsigned)port);
    put16(b + 6, (unsigned)ext);
    put32(b + 8, lifetime);
    return 12;
}

int portmap_natpmp_parse_map(const uint8_t *b, int n, int port, int *result, int *ext, uint32_t *lifetime) {
    if (n < 8 || b[0] != 0 || b[1] != 129) return -1;
    *result = (int)get16(b + 2);
    if (*result) return 0;
    if (n < 16 || (int)get16(b + 8) != port) return -1;
    *ext = (int)get16(b + 10);
    *lifetime = get32(b + 12);
    return 0;
}

/* ---- PCP (RFC 6887) -------------------------------------------------------------------------- */
static void v4mapped(uint8_t *p, const uint8_t ip[4]) {
    memset(p, 0, 10);
    p[10] = p[11] = 0xff;
    memcpy(p + 12, ip, 4);
}

int portmap_pcp_map_req(uint8_t *b, const uint8_t client[4], const uint8_t nonce[12], int port, int ext, uint32_t lifetime) {
    static const uint8_t any[4] = { 0, 0, 0, 0 };
    memset(b, 0, 60);
    b[0] = 2;                  /* version */
    b[1] = 1;                  /* request, MAP */
    put32(b + 4, lifetime);
    v4mapped(b + 8, client);
    memcpy(b + 24, nonce, 12);
    b[36] = 17;                /* UDP */
    put16(b + 40, (unsigned)port);
    put16(b + 42, (unsigned)ext);
    v4mapped(b + 44, any);
    return 60;
}

int portmap_pcp_parse_map(const uint8_t *b, int n, const uint8_t nonce[12], int port, int *result, int *ext,
                          uint32_t *lifetime, char *ip, size_t ipn) {
    if (n >= 4 && b[0] == 0) { *result = 1; return 0; }   /* NAT-PMP server: unsupported version */
    if (n < 24 || b[0] != 2 || b[1] != (0x80 | 1)) return -1;
    *result = b[3];
    if (*result == 1) return 0;                            /* UNSUPP_VERSION carries no opcode data */
    if (n < 60) return -1;
    if (memcmp(b + 24, nonce, 12) || b[36] != 17 || (int)get16(b + 40) != port) return -1;
    if (*result) return 0;
    *lifetime = get32(b + 4);
    *ext = (int)get16(b + 42);
    const uint8_t *a = b + 44;
    snprintf(ip, ipn, "%u.%u.%u.%u", a[12], a[13], a[14], a[15]);
    return 0;
}

/* ---- text / XML / HTTP parsing --------------------------------------------------------------- */
static int ci_ncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int x = tolower((unsigned char)a[i]), y = tolower((unsigned char)b[i]);
        if (x != y || !x) return x - y;
    }
    return 0;
}
static bool name_eq(const char *p, size_t n, const char *name) {
    return strlen(name) == n && !ci_ncmp(p, name, n);
}

/* Find the first element with local name `name` in [p, end): content range [*cs, *ce). */
static bool xml_find(const char *p, const char *end, const char *name, const char **cs, const char **ce) {
    while (p < end) {
        const char *lt = memchr(p, '<', (size_t)(end - p));
        if (!lt || lt + 1 >= end) return false;
        p = lt + 1;
        if (*p == '/' || *p == '?' || *p == '!') continue;
        const char *ns = p;
        while (p < end && !isspace((unsigned char)*p) && *p != '>' && *p != '/') p++;
        const char *ne = p, *loc = ns;
        for (const char *q = ns; q < ne; q++) if (*q == ':') loc = q + 1;
        if (!name_eq(loc, (size_t)(ne - loc), name)) continue;
        const char *gt = memchr(p, '>', (size_t)(end - p));
        if (!gt) return false;
        if (gt[-1] == '/') { *cs = *ce = gt + 1; return true; }   /* <x/> */
        const char *c = gt + 1;
        /* closing tag: </[prefix:]name> */
        for (const char *q = c; q + 2 < end; q++) {
            if (q[0] != '<' || q[1] != '/') continue;
            const char *t = q + 2, *te = t;
            while (te < end && *te != '>' && !isspace((unsigned char)*te)) te++;
            const char *tl = t;
            for (const char *r = t; r < te; r++) if (*r == ':') tl = r + 1;
            if (name_eq(tl, (size_t)(te - tl), name)) { *cs = c; *ce = q; return true; }
        }
        return false;
    }
    return false;
}

static void xml_text(const char *s, const char *e, char *out, size_t n) {
    while (s < e && isspace((unsigned char)*s)) s++;
    while (e > s && isspace((unsigned char)e[-1])) e--;
    size_t k = 0;
    static const char *const ent[][2] = { { "&amp;", "&" }, { "&lt;", "<" }, { "&gt;", ">" }, { "&quot;", "\"" }, { "&apos;", "'" } };
    while (s < e && k + 1 < n) {
        bool done = false;
        if (*s == '&')
            for (int i = 0; i < 5; i++) {
                size_t l = strlen(ent[i][0]);
                if ((size_t)(e - s) >= l && !strncmp(s, ent[i][0], l)) { out[k++] = ent[i][1][0]; s += l; done = true; break; }
            }
        if (!done) out[k++] = *s++;
    }
    if (n) out[k] = 0;
}

bool portmap_xml_value(const char *xml, const char *name, char *out, size_t n) {
    const char *cs, *ce;
    if (!xml || !xml_find(xml, xml + strlen(xml), name, &cs, &ce)) return false;
    xml_text(cs, ce, out, n);
    return true;
}

/* http://host[:port]/path -> parts */
static bool url_split(const char *url, char *host, size_t hn, int *port, char *path, size_t pn) {
    if (ci_ncmp(url, "http://", 7)) return false;
    const char *h = url + 7, *he = h;
    while (*he && *he != '/' && *he != ':') he++;
    if (he == h || (size_t)(he - h) >= hn) return false;
    memcpy(host, h, (size_t)(he - h));
    host[he - h] = 0;
    *port = 80;
    const char *p = he;
    if (*p == ':') { *port = atoi(p + 1); while (*p && *p != '/') p++; }
    if (*port <= 0 || *port > 65535) return false;
    snprintf(path, pn, "%s", *p ? p : "/");
    return true;
}

bool portmap_url_join(const char *base, const char *rel, char *out, size_t n) {
    if (!ci_ncmp(rel, "http://", 7)) { snprintf(out, n, "%s", rel); return true; }
    char host[256], path[1024];
    int port;
    if (!url_split(base, host, sizeof host, &port, path, sizeof path)) return false;
    if (rel[0] == '/') { snprintf(out, n, "http://%s:%d%s", host, port, rel); return true; }
    char *q = strchr(path, '?');
    if (q) *q = 0;
    char *slash = strrchr(path, '/');
    if (slash) slash[1] = 0;
    snprintf(out, n, "http://%s:%d%s%s", host, port, path, rel);
    return true;
}

bool portmap_upnp_parse_desc(const char *xml, const char *location, char *control, size_t cn, char *service, size_t sn) {
    static const char *const kinds[] = { "urn:schemas-upnp-org:service:WANIPConnection:2",
                                         "urn:schemas-upnp-org:service:WANIPConnection:1",
                                         "urn:schemas-upnp-org:service:WANPPPConnection:1" };
    const char *end = xml + strlen(xml);
    char base[512];
    if (!portmap_xml_value(xml, "URLBase", base, sizeof base) || !base[0]) snprintf(base, sizeof base, "%s", location);
    for (int k = 0; k < 3; k++) {
        const char *p = xml, *cs, *ce;
        while (xml_find(p, end, "service", &cs, &ce)) {
            char st[160], cu[512];
            const char *a, *b;
            st[0] = cu[0] = 0;
            if (xml_find(cs, ce, "serviceType", &a, &b)) xml_text(a, b, st, sizeof st);
            if (xml_find(cs, ce, "controlURL", &a, &b)) xml_text(a, b, cu, sizeof cu);
            if (!strcmp(st, kinds[k]) && cu[0] && portmap_url_join(base, cu, control, cn)) {
                snprintf(service, sn, "%s", st);
                return true;
            }
            p = ce;
        }
    }
    return false;
}

int portmap_http_parse(char *raw, int n, int *status, char **body, int *body_len) {
    raw[n] = 0;   /* the buffer holds n + 1 bytes */
    if (n < 12 || strncmp(raw, "HTTP/1.", 7)) return -1;
    *status = atoi(raw + 9);
    char *he = strstr(raw, "\r\n\r\n");
    if (!he) return -1;
    *he = 0;
    char *b = he + 4;
    int bl = (int)(raw + n - b);
    bool chunked = false;
    long clen = -1;
    for (char *l = strstr(raw, "\r\n"); l; l = strstr(l + 2, "\r\n")) {
        char *v = l + 2;
        if (!ci_ncmp(v, "transfer-encoding:", 18) && strstr(v, "chunked")) chunked = true;
        if (!ci_ncmp(v, "content-length:", 15)) clen = atol(v + 15);
    }
    if (chunked) {   /* decode in place */
        char *r = b, *w = b, *e = b + bl;
        for (;;) {
            char *ln = strstr(r, "\r\n");
            if (!ln || ln >= e) break;
            long sz = strtol(r, NULL, 16);
            r = ln + 2;
            if (sz <= 0 || r + sz > e) break;
            memmove(w, r, (size_t)sz);
            w += sz;
            r += sz + 2;
        }
        bl = (int)(w - b);
    } else if (clen >= 0 && clen < bl) bl = (int)clen;
    b[bl] = 0;
    *body = b;
    *body_len = bl;
    return 0;
}

/* ---- sockets --------------------------------------------------------------------------------- */
static bool net_init(void) {
#ifdef _WIN32
    static bool ok;
    if (!ok) { WSADATA d; if (WSAStartup(MAKEWORD(2, 2), &d)) return false; ok = true; }
#endif
    return true;
}

static void set_nonblock(sock_t s) {
#ifdef _WIN32
    u_long one = 1;
    ioctlsocket(s, FIONBIO, &one);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
}

/* wait until readable (r) or writable (!r); 1 ready, 0 timeout */
static int wait_sock(sock_t s, bool r, int ms) {
    fd_set fs;
    FD_ZERO(&fs);
    FD_SET(s, &fs);
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    int k = select((int)s + 1, r ? &fs : NULL, r ? NULL : &fs, NULL, &tv);
    return k > 0;
}

static bool resolve4(const char *host, int port, struct sockaddr_in *sa) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) || !res) return false;
    memcpy(sa, res->ai_addr, sizeof *sa);
    freeaddrinfo(res);
    return true;
}

/* This machine's address on the route to `to` (UDP connect + getsockname: nothing is sent). */
static bool local_addr_to(const struct sockaddr_in *to, uint8_t ip[4], char *s, size_t sn) {
    sock_t fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == SOCK_BAD) return false;
    struct sockaddr_in me;
    socklen_t ml = sizeof me;
    bool ok = !connect(fd, (const struct sockaddr *)to, sizeof *to) && !getsockname(fd, (struct sockaddr *)&me, &ml);
    sock_close(fd);
    if (!ok) return false;
    memcpy(ip, &me.sin_addr, 4);
    inet_ntop(AF_INET, &me.sin_addr, s, (socklen_t)sn);
    return true;
}

static int s_job_gen;          /* worker only: the request generation being served */
static bool s_nocancel;        /* worker only: deleting, which a pending exit must not cut short */
static bool cancelled(void);

#if defined(MSG_NOSIGNAL)
#define SEND_FLAGS MSG_NOSIGNAL   /* a router closing the connection must not raise SIGPIPE */
#else
#define SEND_FLAGS 0
#endif

/* Send req, wait for a reply that `parse` accepts (returns 0); 250 ms, doubled per try.
 * Returns 1 accepted, 0 no answer, -1 cancelled. */
static int udp_exchange(const struct sockaddr_in *to, const uint8_t *req, int n, int tries, uint8_t *resp, int *rn,
                        int (*parse)(const uint8_t *, int, void *), void *ctx) {
    sock_t fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == SOCK_BAD) return 0;
    if (connect(fd, (const struct sockaddr *)to, sizeof *to)) { sock_close(fd); return 0; }
    set_nonblock(fd);
    int wait_ms = 250, rc = 0;
    for (int t = 0; t < tries && !rc; t++, wait_ms *= 2) {
        send(fd, (const char *)req, n, 0);
        uint64_t until = plat_mono_ns() + (uint64_t)wait_ms * 1000000u;
        while (!rc) {
            if (cancelled()) { rc = -1; break; }
            uint64_t now = plat_mono_ns();
            if (now >= until) break;
            int left = (int)((until - now) / 1000000u) + 1;
            if (!wait_sock(fd, true, left > 50 ? 50 : left)) continue;
            int k = (int)recv(fd, (char *)resp, 1100, 0);
            if (k <= 0) { plat_sleep_us(10000); continue; }   /* ICMP unreachable etc.: keep waiting */
            if (parse(resp, k, ctx) == 0) { *rn = k; rc = 1; }
        }
    }
    sock_close(fd);
    return rc;
}

/* One HTTP request; *out = malloc'd body (caller frees). Returns the status code, 0 if no answer. */
static int http_request(const char *url, const char *soap_action, const char *body, char **out) {
    *out = NULL;
    char host[256], path[1024];
    int port;
    struct sockaddr_in sa;
    if (!url_split(url, host, sizeof host, &port, path, sizeof path) || !resolve4(host, port, &sa)) return 0;
    sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == SOCK_BAD) return 0;
#ifdef SO_NOSIGPIPE
    { int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); }
#endif
    set_nonblock(fd);
    int status = 0;
    char *buf = NULL;
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa)) {
#ifdef _WIN32
        bool pending = WSAGetLastError() == WSAEWOULDBLOCK;
#else
        bool pending = errno == EINPROGRESS;
#endif
        if (!pending || !wait_sock(fd, false, 2000)) goto done;
        int err = 0;
        socklen_t el = sizeof err;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&err, &el);
        if (err) goto done;
    }
    {
        size_t bl = body ? strlen(body) : 0;
        size_t rl = bl + strlen(path) + strlen(host) + (soap_action ? strlen(soap_action) : 0) + 300;
        char *req = malloc(rl);
        if (!req) goto done;
        int k = body ? snprintf(req, rl, "POST %s HTTP/1.1\r\nHost: %s:%d\r\nContent-Type: text/xml; charset=\"utf-8\"\r\n"
                                "SOAPAction: \"%s\"\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                                path, host, port, soap_action, bl, body)
                     : snprintf(req, rl, "GET %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n\r\n", path, host, port);
        int sent = 0;
        uint64_t until = plat_mono_ns() + 3000000000ull;
        while (sent < k && plat_mono_ns() < until && !cancelled()) {
            if (!wait_sock(fd, false, 50)) continue;
            int w = (int)send(fd, req + sent, k - sent, SEND_FLAGS);
            if (w > 0) sent += w;
        }
        free(req);
        if (sent < k) goto done;
        int cap = 128 * 1024, n = 0;
        buf = malloc((size_t)cap + 1);
        if (!buf) goto done;
        until = plat_mono_ns() + 3000000000ull;
        bool eof = false;
        while (!eof && n < cap && plat_mono_ns() < until && !cancelled()) {
            if (!wait_sock(fd, true, 50)) continue;
            int r = (int)recv(fd, buf + n, cap - n, 0);
            if (r > 0) n += r;
            else if (r == 0) eof = true;
            else {
#ifdef _WIN32
                if (WSAGetLastError() != WSAEWOULDBLOCK) eof = true;
#else
                if (errno != EAGAIN && errno != EWOULDBLOCK) eof = true;
#endif
            }
            /* a complete Content-Length reply ends the wait even without the close */
            buf[n] = 0;
            char *he = strstr(buf, "\r\n\r\n"), *cl;
            if (he && (cl = strstr(buf, "Content-Length:")) && cl < he && n - (int)(he + 4 - buf) >= atoi(cl + 15)) eof = true;
        }
        char *b;
        int blen;
        if (n > 0 && portmap_http_parse(buf, n, &status, &b, &blen) == 0) {
            *out = malloc((size_t)blen + 1);
            if (*out) { memcpy(*out, b, (size_t)blen); (*out)[blen] = 0; }
        }
    }
done:
    free(buf);
    sock_close(fd);
    return status;
}

/* ---- the mapping ----------------------------------------------------------------------------- */
enum { P_NONE, P_PCP, P_NATPMP, P_UPNP };
static const char *const k_proto_name[] = { "none", "PCP", "NAT-PMP", "UPnP" };

typedef struct {
    int proto, port, ext;
    uint32_t life;              /* granted lifetime, 0 = permanent (UPnP) */
    uint64_t renew_at;
    uint8_t nonce[12];
    char ext_ip[64];
    char control[512], service[160];   /* UPnP */
    char client[64];                   /* UPnP NewInternalClient */
} Mapping;

typedef struct {             /* outcome of one attempt */
    bool refused, answered;
    char why[160];
} Attempt;

static bool gateway_addr(struct sockaddr_in *gw) {
    if (o.gw[0]) {
        char h[128];
        snprintf(h, sizeof h, "%s", o.gw);
        char *c = strrchr(h, ':');
        int port = 5351;
        if (c) { *c = 0; port = atoi(c + 1); }
        return resolve4(h, port, gw);
    }
    if (test_mode()) return false;    /* a UPnP override alone: no PCP / NAT-PMP at all */
    char ip[64];
    if (!plat_default_gateway_ipv4(ip, sizeof ip)) return false;
    return resolve4(ip, 5351, gw);
}

/* PCP */
typedef struct { const uint8_t *nonce; int port, result, ext; uint32_t life; char ip[64]; } PcpCtx;
static int pcp_parse(const uint8_t *b, int n, void *c) {
    PcpCtx *x = c;
    return portmap_pcp_parse_map(b, n, x->nonce, x->port, &x->result, &x->ext, &x->life, x->ip, sizeof x->ip);
}

static const char *pcp_result_name(int r) {
    static const char *const n[] = { "SUCCESS", "UNSUPP_VERSION", "NOT_AUTHORIZED", "MALFORMED_REQUEST", "UNSUPP_OPCODE",
                                     "UNSUPP_OPTION", "MALFORMED_OPTION", "NETWORK_FAILURE", "NO_RESOURCES",
                                     "UNSUPP_PROTOCOL", "USER_EX_QUOTA", "CANNOT_PROVIDE_EXTERNAL", "ADDRESS_MISMATCH",
                                     "EXCESSIVE_REMOTE_PEERS" };
    return r >= 0 && r < 14 ? n[r] : "?";
}

/* 1 mapped, 0 failed (a), -1 cancelled */
static int pcp_map(const struct sockaddr_in *gw, Mapping *m, uint32_t life, Attempt *a, int tries) {
    uint8_t me[4], req[60], resp[1100];
    char mes[64];
    if (!local_addr_to(gw, me, mes, sizeof mes)) return 0;
    PcpCtx x = { m->nonce, m->port, 0, 0, 0, "" };
    int n = portmap_pcp_map_req(req, me, m->nonce, m->port, m->ext ? m->ext : m->port, life), rn;
    int r = udp_exchange(gw, req, n, tries, resp, &rn, pcp_parse, &x);
    if (r <= 0) return r;
    a->answered = true;
    if (x.result == 1) { snprintf(a->why, sizeof a->why, "PCP not supported by the router"); return 0; }
    if (x.result) {
        a->refused = x.result == 2 || x.result == 7 || x.result == 8 || x.result == 10 || x.result == 11;
        snprintf(a->why, sizeof a->why, "PCP refused: %s", pcp_result_name(x.result));
        return 0;
    }
    if (life == 0) return 1;   /* deleted */
    m->proto = P_PCP; m->ext = x.ext; m->life = x.life;
    snprintf(m->ext_ip, sizeof m->ext_ip, "%s", x.ip);
    return 1;
}

/* NAT-PMP */
typedef struct { int port, result, ext; uint32_t life; char ip[64]; } NpCtx;
static int np_parse_map(const uint8_t *b, int n, void *c) { NpCtx *x = c; return portmap_natpmp_parse_map(b, n, x->port, &x->result, &x->ext, &x->life); }
static int np_parse_addr(const uint8_t *b, int n, void *c) { NpCtx *x = c; return portmap_natpmp_parse_addr(b, n, &x->result, x->ip, sizeof x->ip); }

static int natpmp_map(const struct sockaddr_in *gw, Mapping *m, uint32_t life, Attempt *a, int tries) {
    uint8_t req[12], resp[1100];
    NpCtx x = { m->port, 0, 0, 0, "" };
    int rn;
    if (life) {
        int r = udp_exchange(gw, req, portmap_natpmp_addr_req(req), tries, resp, &rn, np_parse_addr, &x);
        if (r <= 0) return r;
        a->answered = true;
        if (x.result) {
            a->refused = x.result == 2 || x.result == 3;
            snprintf(a->why, sizeof a->why, "NAT-PMP external address: result %d", x.result);
            return 0;
        }
        snprintf(m->ext_ip, sizeof m->ext_ip, "%s", x.ip);
    }
    int n = portmap_natpmp_map_req(req, m->port, life ? (m->ext ? m->ext : m->port) : 0, life);
    int r = udp_exchange(gw, req, n, tries, resp, &rn, np_parse_map, &x);
    if (r <= 0) return r;
    a->answered = true;
    if (x.result) {
        a->refused = x.result == 2 || x.result == 3 || x.result == 4;
        snprintf(a->why, sizeof a->why, "NAT-PMP refused: result %d", x.result);
        return 0;
    }
    if (life == 0) return 1;
    m->proto = P_NATPMP; m->ext = x.ext; m->life = x.life;
    return 1;
}

/* UPnP */
static int soap(const Mapping *m, const char *action, const char *args, char **out) {
    size_t n = strlen(args) + strlen(m->service) + 2 * strlen(action) + 400;
    char *body = malloc(n), sa[320];
    if (!body) return 0;
    snprintf(body, n, "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:%s xmlns:u=\"%s\">%s</u:%s>"
             "</s:Body></s:Envelope>\r\n", action, m->service, args, action);
    snprintf(sa, sizeof sa, "%s#%s", m->service, action);
    int st = http_request(m->control, sa, body, out);
    free(body);
    return st;
}

static int upnp_error(const char *body) {
    char v[32];
    return body && portmap_xml_value(body, "errorCode", v, sizeof v) ? atoi(v) : -1;
}

/* AddPortMapping (or AddAnyPortMapping); returns the HTTP status, *err the UPnP error code */
static int upnp_add(Mapping *m, int ext, uint32_t lease, bool any, int *err, int *granted) {
    char args[1024], *out = NULL;
    snprintf(args, sizeof args, "<NewRemoteHost></NewRemoteHost><NewExternalPort>%d</NewExternalPort>"
             "<NewProtocol>UDP</NewProtocol><NewInternalPort>%d</NewInternalPort><NewInternalClient>%s</NewInternalClient>"
             "<NewEnabled>1</NewEnabled><NewPortMappingDescription>" DESCRIPTION "</NewPortMappingDescription>"
             "<NewLeaseDuration>%u</NewLeaseDuration>", ext, m->port, m->client, lease);
    int st = soap(m, any ? "AddAnyPortMapping" : "AddPortMapping", args, &out);
    *err = st == 200 ? 0 : upnp_error(out);
    *granted = ext;
    char v[32];
    if (st == 200 && any && portmap_xml_value(out, "NewReservedPort", v, sizeof v)) *granted = atoi(v);
    free(out);
    return st;
}

static bool upnp_find(const char *location, Mapping *m) {
    char *desc = NULL;
    int st = http_request(location, NULL, NULL, &desc);
    bool ok = st == 200 && desc && portmap_upnp_parse_desc(desc, location, m->control, sizeof m->control, m->service, sizeof m->service);
    free(desc);
    return ok;
}

/* SSDP M-SEARCH on the local network (never in test mode): up to 8 description URLs. */
static int ssdp_discover(char loc[][512], int max) {
    if (test_mode()) return 0;
    sock_t fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == SOCK_BAD) return 0;
    set_nonblock(fd);
    struct sockaddr_in mc;
    memset(&mc, 0, sizeof mc);
    mc.sin_family = AF_INET;
    mc.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &mc.sin_addr);
    static const char *const st[] = { "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
                                      "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                                      "urn:schemas-upnp-org:service:WANIPConnection:1" };
    int n = 0;
    uint64_t t0 = plat_mono_ns();
    for (int round = 0; round < 2 && !n; round++) {
        for (int i = 0; i < 3; i++) {
            char q[300];
            int k = snprintf(q, sizeof q, "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\n"
                             "MX: 2\r\nST: %s\r\n\r\n", st[i]);
            sendto(fd, q, k, 0, (struct sockaddr *)&mc, sizeof mc);
        }
        uint64_t until = plat_mono_ns() + 1200000000ull;
        while (plat_mono_ns() < until && n < max && !cancelled()) {
            if (!wait_sock(fd, true, 50)) continue;
            char b[1600];
            int k = (int)recv(fd, b, sizeof b - 1, 0);
            if (k <= 0) continue;
            b[k] = 0;
            for (char *l = b; l; l = strstr(l, "\r\n")) {
                if (*l == '\r') l += 2;
                if (ci_ncmp(l, "location:", 9)) continue;
                char *v = l + 9, *e = strstr(v, "\r\n");
                while (*v == ' ') v++;
                if (!e) e = v + strlen(v);
                char u[512];
                snprintf(u, sizeof u, "%.*s", (int)(e - v), v);
                bool dup = false;
                for (int j = 0; j < n; j++) dup |= !strcmp(loc[j], u);
                if (!dup && n < max) snprintf(loc[n++], 512, "%s", u);
                break;
            }
        }
    }
    sock_close(fd);
    PM_LOG("UPnP discovery: %d device(s) in %.1f s", n, (double)(plat_mono_ns() - t0) / 1e9);
    return n;
}

/* 1 mapped, 0 failed, -1 cancelled. m->control set means "use this device" (adopted mapping). */
static int upnp_map(Mapping *m, Attempt *a, bool adopt) {
    char loc[8][512];
    int nloc = 0;
    if (!adopt) {
        if (o.upnp[0]) snprintf(loc[nloc++], 512, "%s", o.upnp);
        else nloc = ssdp_discover(loc, 8);
        if (cancelled()) return -1;
        if (!nloc) return 0;
    }
    for (int i = 0; i < (adopt ? 1 : nloc); i++) {
        if (!adopt && !upnp_find(loc[i], m)) continue;
        if (cancelled()) return -1;
        a->answered = true;
        char host[256], path[16];
        int port;
        struct sockaddr_in ca;
        uint8_t me[4];
        if (!url_split(m->control, host, sizeof host, &port, path, sizeof path)) continue;
        if (!resolve4(host, port, &ca) || !local_addr_to(&ca, me, m->client, sizeof m->client)) continue;
        char *out = NULL;
        int st = soap(m, "GetExternalIPAddress", "", &out);
        m->ext_ip[0] = 0;
        if (st == 200) portmap_xml_value(out, "NewExternalIPAddress", m->ext_ip, sizeof m->ext_ip);
        free(out);
        if (st != 200) { snprintf(a->why, sizeof a->why, "UPnP GetExternalIPAddress: HTTP %d", st); continue; }
        int want = m->ext ? m->ext : m->port, err = 0, got = want;
        uint32_t lease = o.lease;
        st = upnp_add(m, want, lease, false, &err, &got);
        if (err == 725) {   /* OnlyPermanentLeasesSupported */
            PM_LOG("the router only accepts permanent UPnP mappings; asking for one (it is deleted when hosting ends)");
            lease = 0;
            st = upnp_add(m, want, lease, false, &err, &got);
        }
        if (err == 718 && !adopt) {   /* ConflictInMappingEntry: the port is taken by another computer */
            if (strstr(m->service, "WANIPConnection:2")) st = upnp_add(m, want, lease, true, &err, &got);
            for (int k = 1; k <= 4 && err == 718; k++) st = upnp_add(m, want + k, lease, false, &err, &got);
        }
        if (st == 200 && !err) {
            m->proto = P_UPNP; m->ext = got; m->life = lease;
            return 1;
        }
        a->refused = true;
        snprintf(a->why, sizeof a->why, "UPnP AddPortMapping: HTTP %d, error %d", st, err);
        if (st == 0) { a->refused = false; snprintf(a->why, sizeof a->why, "UPnP: no answer from %s", m->control); }
    }
    return 0;
}

static bool upnp_delete(const Mapping *m) {
    char args[300], *out = NULL;
    snprintf(args, sizeof args, "<NewRemoteHost></NewRemoteHost><NewExternalPort>%d</NewExternalPort><NewProtocol>UDP</NewProtocol>", m->ext);
    int st = soap(m, "DeletePortMapping", args, &out);
    free(out);
    return st == 200;
}

/* ---- worker / shared state ------------------------------------------------------------------- */
enum { ST_OFF, ST_BUSY, ST_OK, ST_CGNAT, ST_NOANSWER, ST_REFUSED, ST_DISABLED };

static plat_mutex s_m = PLAT_MUTEX_INIT;
static struct {
    bool running, done;
    bool exit_req, keep;
    int want, gen;                 /* port wanted (0 = none), request generation */
    int state, text_gen;
    char text[400], shortt[200];
    Mapping cur;                   /* the mapping in place (cur.proto != 0), for the restart hand-off */
    char adopt[1024];              /* DESTREGA_PORTMAP_KEEP read at the first start */
} S;
static plat_thread s_thread;

static bool cancelled(void) {
    if (s_nocancel) return false;
    plat_mutex_lock(&s_m);
    bool c = S.exit_req || S.keep || S.gen != s_job_gen;
    plat_mutex_unlock(&s_m);
    return c;
}

static void publish(int state, const char *text, const char *shortt) {
    plat_mutex_lock(&s_m);
    S.state = state;
    snprintf(S.text, sizeof S.text, "%s", text);
    snprintf(S.shortt, sizeof S.shortt, "%s", shortt);
    S.text_gen++;
    plat_mutex_unlock(&s_m);
    if (*text) PM_LOG("%s", text);
}

static void publish_mapping(const Mapping *m) {
    plat_mutex_lock(&s_m);
    S.cur = *m;
    plat_mutex_unlock(&s_m);
}

static void publish_ok(const Mapping *m) {
    char t[400], s[200];
    int cls = portmap_classify_ipv4(m->ext_ip);
    if (cls == PM_ADDR_PUBLIC) {
        snprintf(t, sizeof t, "Your address for the opponent: %s:%d", m->ext_ip, m->ext);
        snprintf(s, sizeof s, "your address: %s:%d", m->ext_ip, m->ext);
        publish(ST_OK, t, s);
    } else {
        snprintf(t, sizeof t, "Your internet provider doesn't allow incoming connections, so players outside your home "
                 "network probably can't join (the router's internet address, %s, is not a public one).",
                 m->ext_ip[0] ? m->ext_ip : "unknown");
        snprintf(s, sizeof s, "no incoming connections from the internet");
        publish(ST_CGNAT, t, s);
    }
}

static void delete_mapping(Mapping *m) {
    if (!m->proto) return;
    Attempt a = { 0 };
    bool ok = false;
    struct sockaddr_in gw;
    Mapping d = *m;
    s_nocancel = true;   /* the exit request that led here must not cut the delete short */
    if (m->proto == P_UPNP) ok = upnp_delete(m);
    else if (gateway_addr(&gw)) ok = (m->proto == P_PCP ? pcp_map(&gw, &d, 0, &a, 3) : natpmp_map(&gw, &d, 0, &a, 3)) == 1;
    s_nocancel = false;
    PM_LOG("%s mapping of port %d %s", k_proto_name[m->proto], m->ext, ok ? "deleted" : "could not be deleted (it expires by itself)");
    if (ok && m->life == 0) PM_LOG("(it was a permanent mapping)");
    m->proto = P_NONE;
    publish_mapping(m);
}

/* parse DESTREGA_PORTMAP_KEEP: proto|port|ext|life|nonce hex|ext ip|control|service|client */
static bool adopt_parse(const char *s, int port, Mapping *m) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", s);
    char *f[9];
    int n = 0;
    for (char *p = buf; n < 9; n++) {
        f[n] = p;
        char *b = strchr(p, '|');
        if (!b) { n++; break; }
        *b = 0;
        p = b + 1;
    }
    if (n != 9 || atoi(f[1]) != port) return false;
    memset(m, 0, sizeof *m);
    m->proto = atoi(f[0]); m->port = port; m->ext = atoi(f[2]); m->life = (uint32_t)strtoul(f[3], NULL, 10);
    for (int i = 0; i < 12 && f[4][2 * i] && f[4][2 * i + 1]; i++) {
        unsigned v;
        sscanf(f[4] + 2 * i, "%2x", &v);
        m->nonce[i] = (uint8_t)v;
    }
    snprintf(m->ext_ip, sizeof m->ext_ip, "%s", f[5]);
    snprintf(m->control, sizeof m->control, "%s", f[6]);
    snprintf(m->service, sizeof m->service, "%s", f[7]);
    snprintf(m->client, sizeof m->client, "%s", f[8]);
    return m->proto >= P_PCP && m->proto <= P_UPNP;
}

/* Map `port` (renewing an adopted mapping first if there is one). 1 ok, 0 failed, -1 cancelled. */
static int map_port(int port, Mapping *m, const char *adopt) {
    Attempt pcp = { 0 }, np = { 0 }, up = { 0 };
    struct sockaddr_in gw;
    bool have_gw = gateway_addr(&gw);
    memset(m, 0, sizeof *m);
    m->port = port;
    uint64_t seed = plat_mono_ns() ^ (uint64_t)time(NULL) << 20 ^ (uint64_t)(uintptr_t)m;
    for (int i = 0; i < 12; i++) {   /* PCP mapping nonce: only has to differ from other clients' */
        seed ^= seed >> 12; seed ^= seed << 25; seed ^= seed >> 27;
        m->nonce[i] = (uint8_t)((seed * 0x2545f4914f6cdd1dull) >> 56);
    }
    if (adopt && *adopt) {
        Mapping a;
        if (adopt_parse(adopt, port, &a)) {
            PM_LOG("renewing the %s mapping of port %d made before the restart", k_proto_name[a.proto], a.ext);
            int r = 0;
            Attempt x = { 0 };
            if (a.proto == P_UPNP) r = upnp_map(&a, &x, true);
            else if (have_gw) r = (a.proto == P_PCP ? pcp_map(&gw, &a, o.lease, &x, 3) : natpmp_map(&gw, &a, o.lease, &x, 3));
            if (r) { *m = a; return r; }
            PM_LOG("could not renew it (%s); asking again", x.why[0] ? x.why : "no answer");
        }
    }
    if (have_gw) {
        int r = pcp_map(&gw, m, o.lease, &pcp, 3);
        if (r) return r;
        if (pcp.why[0]) PM_LOG("%s", pcp.why);
        r = natpmp_map(&gw, m, o.lease, &np, pcp.answered ? 3 : 2);
        if (r) return r;
        if (np.why[0]) PM_LOG("%s", np.why);
    }
    int r = upnp_map(m, &up, false);
    if (r) return r;
    if (up.why[0]) PM_LOG("%s", up.why);
    char t[400];
    if (pcp.refused || np.refused || up.refused) {
        snprintf(t, sizeof t, "Your router refused to open port %d. Turn on automatic port opening (UPnP or NAT-PMP) in "
                 "the router's settings, or forward UDP port %d to this computer by hand.", port, port);
        publish(ST_REFUSED, t, "the router refused to open the port");
    } else {
        snprintf(t, sizeof t, "Your router didn't answer; you may need to forward UDP port %d to this computer by hand "
                 "to play over the internet.", port);
        publish(ST_NOANSWER, t, "the router didn't answer");
    }
    return 0;
}

static void worker(void *arg) {
    (void)arg;
    Mapping m;
    memset(&m, 0, sizeof m);
    int failed_gen = -1, mapped_gen = -1;
    for (;;) {
        plat_mutex_lock(&s_m);
        int want = S.want, gen = S.gen;
        bool ex = S.exit_req, keep = S.keep;
        char adopt[1024];
        snprintf(adopt, sizeof adopt, "%s", S.adopt);
        S.adopt[0] = 0;
        s_job_gen = gen;
        plat_mutex_unlock(&s_m);
        if (keep) break;                               /* the restarted process takes the mapping over */
        if (m.proto && (want != m.port || ex)) { delete_mapping(&m); continue; }
        if (ex) break;
        if (want && !m.proto && failed_gen != gen) {
            char t[200];
            snprintf(t, sizeof t, "Asking your router to open port %d for the internet...", want);
            publish(ST_BUSY, t, "asking the router to open the port");
            int r = map_port(want, &m, adopt);
            if (r == 1) {
                mapped_gen = gen;
                m.renew_at = m.life ? plat_mono_ns() + (uint64_t)m.life * 500000000ull : 0;
                PM_LOG("%s: UDP port %d -> external %s:%d, lease %u s%s", k_proto_name[m.proto], m.port, m.ext_ip, m.ext,
                       (unsigned)m.life, m.life ? "" : " (permanent)");
                publish_mapping(&m);
                publish_ok(&m);
            } else if (r == 0) failed_gen = gen;
            continue;
        }
        if (want && m.proto && gen != mapped_gen) { mapped_gen = gen; publish_ok(&m); }   /* hosting again: same mapping */
        if (m.proto && m.life && plat_mono_ns() >= m.renew_at) {
            Attempt a = { 0 };
            struct sockaddr_in gw;
            Mapping n = m;
            int r = 0, old_ext = m.ext;
            if (m.proto == P_UPNP) {
                int err = 0, got = 0;
                r = upnp_add(&n, m.ext, m.life, false, &err, &got) == 200 && !err;
            } else if (gateway_addr(&gw)) r = (m.proto == P_PCP ? pcp_map(&gw, &n, o.lease, &a, 3) : natpmp_map(&gw, &n, o.lease, &a, 3)) == 1;
            if (r == 1) {
                if (n.ext != m.ext) PM_LOG("renewal moved the external port %d -> %d", m.ext, n.ext);
                m = n;
                m.renew_at = plat_mono_ns() + (uint64_t)(m.life ? m.life : o.lease) * 500000000ull;
                PM_LOG("%s mapping of port %d renewed (lease %u s)", k_proto_name[m.proto], m.ext, (unsigned)m.life);
                publish_mapping(&m);
                if (m.ext != old_ext) publish_ok(&m);
            } else if (r == 0) {
                PM_LOG("renewal failed (%s); trying again in 15 s", a.why[0] ? a.why : "no answer");
                m.renew_at = plat_mono_ns() + 15000000000ull;
            }
            continue;
        }
        plat_sleep_us(50000);
    }
    plat_mutex_lock(&s_m);
    S.done = true;
    S.running = false;
    plat_mutex_unlock(&s_m);
}

/* ---- main-thread API ------------------------------------------------------------------------- */
static uint64_t s_busy_last_ns, s_busy_max_ns, s_busy_frames;
static int s_seen_gen;

static bool enabled(void) {
    if (o.headless && !test_mode()) return false;   /* tests never touch the real network */
    if (o.flag >= 0) return o.flag == 1;
    return settings_get_bool("netplay", "port_mapping", true);
}

void portmap_start(int port) {
    if (!enabled()) {
        if (!o.headless) {
            char t[300];
            snprintf(t, sizeof t, "Automatic port opening is off: to play over the internet, forward UDP port %d to this "
                     "computer in your router.", port);
            publish(ST_DISABLED, t, "");
        }
        return;
    }
    if (!net_init()) return;
    plat_mutex_lock(&s_m);
    static bool env_read;
    if (!env_read) {
        env_read = true;
        const char *e = getenv(KEEP_ENV);
        if (e) snprintf(S.adopt, sizeof S.adopt, "%s", e);
        plat_unsetenv(KEEP_ENV);
    }
    S.want = port;
    S.gen++;
    S.exit_req = S.keep = false;
    bool start = !S.running;
    if (start) { S.running = true; S.done = false; }
    plat_mutex_unlock(&s_m);
    if (start && !plat_thread_start(&s_thread, worker, NULL)) {
        plat_mutex_lock(&s_m); S.running = false; S.done = true; plat_mutex_unlock(&s_m);
        PM_LOG("cannot start the worker thread");
        return;
    }
    static bool registered;
    if (!registered) { registered = true; atexit(portmap_finish); }
}

void portmap_stop(void) {
    plat_mutex_lock(&s_m);
    S.want = 0;
    S.gen++;
    if (S.state != ST_OFF) { S.state = ST_OFF; S.text[0] = S.shortt[0] = 0; S.text_gen++; }
    plat_mutex_unlock(&s_m);
}

void portmap_keep_for_restart(void) {
    plat_mutex_lock(&s_m);
    S.keep = true;
    Mapping m = S.cur;
    plat_mutex_unlock(&s_m);
    if (!m.proto) return;
    char nonce[25], v[1200];
    for (int i = 0; i < 12; i++) snprintf(nonce + 2 * i, 3, "%02x", m.nonce[i]);
    snprintf(v, sizeof v, "%d|%d|%d|%u|%s|%s|%s|%s|%s", m.proto, m.port, m.ext, (unsigned)m.life, nonce, m.ext_ip,
             m.control, m.service, m.client);
    plat_setenv(KEEP_ENV, v);
    PM_LOG("keeping the %s mapping of port %d for the restarted game", k_proto_name[m.proto], m.ext);
}

void portmap_finish(void) {
    static bool finished;
    if (finished) return;
    finished = true;
    plat_mutex_lock(&s_m);
    bool running = S.running, keep = S.keep;
    S.want = 0;
    S.exit_req = true;
    S.gen++;
    plat_mutex_unlock(&s_m);
    if (s_busy_frames)
        PM_LOG("frame loop: %llu frames ran while the router was being asked, longest gap between frames %.1f ms",
               (unsigned long long)s_busy_frames, (double)s_busy_max_ns / 1e6);
    if (!running || keep) return;
    uint64_t until = plat_mono_ns() + 3000000000ull;
    for (;;) {
        plat_mutex_lock(&s_m);
        bool done = S.done;
        plat_mutex_unlock(&s_m);
        if (done) { plat_thread_join(s_thread); break; }
        if (plat_mono_ns() > until) { PM_LOG("gave up waiting for the router at exit"); break; }
        plat_sleep_us(10000);
    }
}

bool portmap_tick(void) {
    plat_mutex_lock(&s_m);
    int g = S.text_gen, st = S.state;
    plat_mutex_unlock(&s_m);
    uint64_t now = plat_mono_ns();
    if (st == ST_BUSY) {   /* how smoothly frames keep coming while the worker waits for the router */
        if (s_busy_last_ns && now - s_busy_last_ns > s_busy_max_ns) s_busy_max_ns = now - s_busy_last_ns;
        s_busy_last_ns = now;
        s_busy_frames++;
    } else s_busy_last_ns = 0;
    if (g == s_seen_gen) return false;
    s_seen_gen = g;
    return true;
}

const char *portmap_text(void) {
    static char t[400];
    plat_mutex_lock(&s_m);
    snprintf(t, sizeof t, "%s", S.text);
    plat_mutex_unlock(&s_m);
    return t;
}

const char *portmap_status_short(void) {
    static char t[200];
    plat_mutex_lock(&s_m);
    snprintf(t, sizeof t, "%s", S.shortt);
    plat_mutex_unlock(&s_m);
    return t;
}
