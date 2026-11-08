/* Host platform shims for networking (see platform.h): kept apart from platform.c so the unit
 * tests that link platform.c need no socket libraries. Windows links ws2_32 + iphlpapi. */
#include "platform.h"
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
