/* Host platform shims for networking (see platform.h): kept apart from platform.c so the unit
 * tests that link platform.c need no socket libraries. Windows links ws2_32 + iphlpapi. */
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <net/route.h>
#endif
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

static int add_addr(char *out, size_t n, size_t *len, int k, const char *h)
{
    int w = snprintf(out + *len, n > *len ? n - *len : 0, "%s%s", k ? ", " : "", h);
    if (w > 0) *len += (size_t)w;
    if (*len >= n) { if (n) out[n - 1] = 0; return -1; }
    return 0;
}

int plat_local_ipv4(char *out, size_t n)
{
    int k = 0;
    size_t len = 0;
    if (n) out[0] = 0;
#ifdef _WIN32
    ULONG sz = 16 * 1024;
    IP_ADAPTER_ADDRESSES *aa = NULL;
    for (int tries = 0; tries < 3; tries++) {
        aa = malloc(sz);
        if (!aa) return 0;
        ULONG r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                       NULL, aa, &sz);
        if (r == NO_ERROR) break;
        free(aa);
        aa = NULL;
        if (r != ERROR_BUFFER_OVERFLOW) return 0;
    }
    for (IP_ADAPTER_ADDRESSES *a = aa; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            struct sockaddr_in *sa = (struct sockaddr_in *)u->Address.lpSockaddr;
            if ((ntohl(sa->sin_addr.s_addr) >> 24) == 127) continue;
            char h[64];
            if (!inet_ntop(AF_INET, &sa->sin_addr, h, sizeof h)) continue;
            if (add_addr(out, n, &len, k, h)) { free(aa); return k; }
            k++;
        }
    }
    free(aa);
#else
    struct ifaddrs *ifs = NULL;
    if (getifaddrs(&ifs)) return 0;
    for (struct ifaddrs *i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP))
            continue;
        char h[64];
        inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, h, sizeof h);
        if (add_addr(out, n, &len, k, h)) break;
        k++;
    }
    freeifaddrs(ifs);
#endif
    return k;
}

bool plat_default_gateway_ipv4(char *out, size_t n)
{
    if (n) out[0] = 0;
#if defined(_WIN32)
    ULONG sz = 0;
    if (GetIpForwardTable(NULL, &sz, FALSE) != ERROR_INSUFFICIENT_BUFFER || !sz) return false;
    MIB_IPFORWARDTABLE *t = malloc(sz);
    if (!t) return false;
    bool ok = false;
    DWORD best = 0xffffffff;
    if (GetIpForwardTable(t, &sz, TRUE) == NO_ERROR) {
        for (DWORD i = 0; i < t->dwNumEntries; i++) {
            MIB_IPFORWARDROW *r = &t->table[i];
            if (r->dwForwardDest != 0 || r->dwForwardMask != 0 || r->dwForwardNextHop == 0 || r->dwForwardMetric1 >= best) continue;
            struct in_addr a;
            a.s_addr = r->dwForwardNextHop;
            if (inet_ntop(AF_INET, &a, out, (socklen_t)n)) { ok = true; best = r->dwForwardMetric1; }
        }
    }
    free(t);
    return ok;
#elif defined(__APPLE__)
    int mib[6] = { CTL_NET, PF_ROUTE, 0, AF_INET, NET_RT_FLAGS, RTF_GATEWAY };
    size_t len = 0;
    if (sysctl(mib, 6, NULL, &len, NULL, 0) || !len) return false;
    char *buf = malloc(len);
    if (!buf) return false;
    bool ok = false;
    if (!sysctl(mib, 6, buf, &len, NULL, 0)) {
        for (char *p = buf; p + sizeof(struct rt_msghdr) <= buf + len && !ok;) {
            struct rt_msghdr *rt = (struct rt_msghdr *)p;
            if (!rt->rtm_msglen) break;
            struct sockaddr *sa = (struct sockaddr *)(rt + 1), *addr[RTAX_MAX] = { 0 };
            for (int i = 0; i < RTAX_MAX; i++) {
                if (!(rt->rtm_addrs & (1 << i))) continue;
                addr[i] = sa;
                size_t l = sa->sa_len ? ((sa->sa_len + sizeof(uint32_t) - 1) & ~(sizeof(uint32_t) - 1)) : sizeof(uint32_t);
                sa = (struct sockaddr *)((char *)sa + l);
            }
            struct sockaddr *dst = addr[RTAX_DST], *gw = addr[RTAX_GATEWAY];
            if ((rt->rtm_flags & RTF_UP) && dst && gw && dst->sa_family == AF_INET && gw->sa_family == AF_INET &&
                ((struct sockaddr_in *)dst)->sin_addr.s_addr == 0)
                ok = inet_ntop(AF_INET, &((struct sockaddr_in *)gw)->sin_addr, out, (socklen_t)n) != NULL;
            p += rt->rtm_msglen;
        }
    }
    free(buf);
    return ok;
#else
    FILE *f = fopen("/proc/net/route", "r");
    if (!f) return false;
    char line[512];
    bool ok = false;
    unsigned best = 0xffffffffu;
    if (fgets(line, sizeof line, f)) {   /* header */
        while (fgets(line, sizeof line, f)) {
            char ifn[64];
            unsigned dst, gw, flags, refcnt, use, metric, mask;
            if (sscanf(line, "%63s %x %x %x %u %u %u %x", ifn, &dst, &gw, &flags, &refcnt, &use, &metric, &mask) != 8) continue;
            if (dst != 0 || mask != 0 || !(flags & 0x2) || !(flags & 0x1) || metric >= best) continue;   /* RTF_GATEWAY, RTF_UP */
            struct in_addr a;
            a.s_addr = gw;   /* the file holds the address in network byte order, printed as a host integer */
            if (inet_ntop(AF_INET, &a, out, (socklen_t)n)) { ok = true; best = metric; }
        }
    }
    fclose(f);
    return ok;
#endif
}
