/* Unit tests for portmap.c's packet encoding / parsing (PCP, NAT-PMP, UPnP description, SOAP,
 * HTTP) and address classification. Nothing here touches the network; the default-gateway lookup
 * is a local routing-table read and only printed. */
#include "portmap.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void test_classify(void) {
    CHECK(portmap_classify_ipv4("8.8.8.8") == PM_ADDR_PUBLIC);
    CHECK(portmap_classify_ipv4("203.0.113.45") == PM_ADDR_PUBLIC);
    CHECK(portmap_classify_ipv4("100.64.0.1") == PM_ADDR_CGNAT);
    CHECK(portmap_classify_ipv4("100.127.255.254") == PM_ADDR_CGNAT);
    CHECK(portmap_classify_ipv4("100.128.0.1") == PM_ADDR_PUBLIC);
    CHECK(portmap_classify_ipv4("100.63.255.255") == PM_ADDR_PUBLIC);
    CHECK(portmap_classify_ipv4("10.1.2.3") == PM_ADDR_PRIVATE);
    CHECK(portmap_classify_ipv4("172.16.0.1") == PM_ADDR_PRIVATE);
    CHECK(portmap_classify_ipv4("172.31.255.1") == PM_ADDR_PRIVATE);
    CHECK(portmap_classify_ipv4("172.32.0.1") == PM_ADDR_PUBLIC);
    CHECK(portmap_classify_ipv4("192.168.1.1") == PM_ADDR_PRIVATE);
    CHECK(portmap_classify_ipv4("127.0.0.1") == PM_ADDR_INVALID);
    CHECK(portmap_classify_ipv4("0.0.0.0") == PM_ADDR_INVALID);
    CHECK(portmap_classify_ipv4("169.254.3.4") == PM_ADDR_INVALID);
    CHECK(portmap_classify_ipv4("239.255.255.250") == PM_ADDR_INVALID);
    CHECK(portmap_classify_ipv4("1.2.3") == PM_ADDR_INVALID);
    CHECK(portmap_classify_ipv4("1.2.3.4x") == PM_ADDR_INVALID);
    CHECK(portmap_classify_ipv4("256.1.1.1") == PM_ADDR_INVALID);
    CHECK(portmap_classify_ipv4("") == PM_ADDR_INVALID);
}

static void test_natpmp(void) {
    uint8_t b[64];
    CHECK(portmap_natpmp_addr_req(b) == 2 && b[0] == 0 && b[1] == 0);
    CHECK(portmap_natpmp_map_req(b, 7000, 7001, 10800) == 12);
    static const uint8_t want[12] = { 0, 1, 0, 0, 0x1b, 0x58, 0x1b, 0x59, 0, 0, 0x2a, 0x30 };
    CHECK(!memcmp(b, want, 12));
    /* address response: 203.0.113.45 */
    uint8_t ar[12] = { 0, 128, 0, 0, 0, 0, 1, 0, 203, 0, 113, 45 };
    int res = -1;
    char ip[64] = "";
    CHECK(portmap_natpmp_parse_addr(ar, 12, &res, ip, sizeof ip) == 0 && res == 0 && !strcmp(ip, "203.0.113.45"));
    ar[1] = 129;
    CHECK(portmap_natpmp_parse_addr(ar, 12, &res, ip, sizeof ip) == -1);
    /* map response: internal 7000 -> external 7002, 3600 s */
    uint8_t mr[16] = { 0, 129, 0, 0, 0, 0, 0, 9, 0x1b, 0x58, 0x1b, 0x5a, 0, 0, 0x0e, 0x10 };
    int ext = 0;
    uint32_t life = 0;
    CHECK(portmap_natpmp_parse_map(mr, 16, 7000, &res, &ext, &life) == 0 && res == 0 && ext == 7002 && life == 3600);
    CHECK(portmap_natpmp_parse_map(mr, 16, 7001, &res, &ext, &life) == -1);   /* another internal port */
    CHECK(portmap_natpmp_parse_map(mr, 12, 7000, &res, &ext, &life) == -1);   /* short */
    mr[3] = 2;   /* refused */
    CHECK(portmap_natpmp_parse_map(mr, 8, 7000, &res, &ext, &life) == 0 && res == 2);
}

static void test_pcp(void) {
    uint8_t b[64];
    const uint8_t me[4] = { 192, 168, 1, 20 };
    uint8_t nonce[12];
    for (int i = 0; i < 12; i++) nonce[i] = (uint8_t)(0xa0 + i);
    CHECK(portmap_pcp_map_req(b, me, nonce, 7000, 7000, 10800) == 60);
    CHECK(b[0] == 2 && b[1] == 1 && b[2] == 0 && b[3] == 0);
    CHECK(b[4] == 0 && b[5] == 0 && b[6] == 0x2a && b[7] == 0x30);
    CHECK(b[18] == 0xff && b[19] == 0xff && !memcmp(b + 20, me, 4));
    CHECK(!memcmp(b + 24, nonce, 12) && b[36] == 17);
    CHECK(b[40] == 0x1b && b[41] == 0x58 && b[42] == 0x1b && b[43] == 0x58);
    CHECK(b[54] == 0xff && b[55] == 0xff && b[56] == 0 && b[59] == 0);
    /* success response */
    uint8_t r[60];
    memcpy(r, b, 60);
    r[1] = 0x81; r[2] = 0; r[3] = 0;
    r[4] = 0; r[5] = 0; r[6] = 0x1c; r[7] = 0x20;   /* 7200 s */
    memset(r + 8, 0, 16);
    r[42] = 0x1f; r[43] = 0x40;                      /* external 8000 */
    r[56] = 198; r[57] = 51; r[58] = 100; r[59] = 7;
    int res = -1, ext = 0;
    uint32_t life = 0;
    char ip[64] = "";
    CHECK(portmap_pcp_parse_map(r, 60, nonce, 7000, &res, &ext, &life, ip, sizeof ip) == 0);
    CHECK(res == 0 && ext == 8000 && life == 7200 && !strcmp(ip, "198.51.100.7"));
    uint8_t other[12];
    memcpy(other, nonce, 12);
    other[5] ^= 1;
    CHECK(portmap_pcp_parse_map(r, 60, other, 7000, &res, &ext, &life, ip, sizeof ip) == -1);
    CHECK(portmap_pcp_parse_map(r, 60, nonce, 7001, &res, &ext, &life, ip, sizeof ip) == -1);
    r[3] = 2;   /* NOT_AUTHORIZED */
    CHECK(portmap_pcp_parse_map(r, 60, nonce, 7000, &res, &ext, &life, ip, sizeof ip) == 0 && res == 2);
    /* a NAT-PMP-only server answers version 0 / unsupported version */
    uint8_t v0[8] = { 0, 129, 0, 1, 0, 0, 0, 5 };
    CHECK(portmap_pcp_parse_map(v0, 8, nonce, 7000, &res, &ext, &life, ip, sizeof ip) == 0 && res == 1);
    uint8_t uv[24] = { 2, 0x81, 0, 1 };
    CHECK(portmap_pcp_parse_map(uv, 24, nonce, 7000, &res, &ext, &life, ip, sizeof ip) == 0 && res == 1);
    uint8_t junk[60] = { 2, 0x82 };   /* PEER response, not MAP */
    CHECK(portmap_pcp_parse_map(junk, 60, nonce, 7000, &res, &ext, &life, ip, sizeof ip) == -1);
}

static void test_upnp(void) {
    char c[512], st[160];
    const char *d1 =
        "<?xml version=\"1.0\"?>\n<root xmlns=\"urn:schemas-upnp-org:device-1-0\"><specVersion><major>1</major></specVersion>"
        "<device><deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType>"
        "<serviceList><service><serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1</serviceType>"
        "<controlURL>/l3f</controlURL></service></serviceList>"
        "<deviceList><device><deviceType>urn:schemas-upnp-org:device:WANDevice:1</deviceType><deviceList><device>"
        "<serviceList><service>\n  <serviceType> urn:schemas-upnp-org:service:WANIPConnection:1 </serviceType>\n"
        "  <serviceId>urn:upnp-org:serviceId:WANIPConn1</serviceId>\n  <controlURL>ctl/IPConn?a=1&amp;b=2</controlURL>"
        "</service></serviceList></device></deviceList></device></deviceList></device></root>";
    CHECK(portmap_upnp_parse_desc(d1, "http://192.168.1.1:5000/desc/root.xml", c, sizeof c, st, sizeof st));
    CHECK(!strcmp(c, "http://192.168.1.1:5000/desc/ctl/IPConn?a=1&b=2"));
    CHECK(!strcmp(st, "urn:schemas-upnp-org:service:WANIPConnection:1"));
    /* URLBase, a namespace prefix, PPP only */
    const char *d2 =
        "<r:root xmlns:r=\"urn:schemas-upnp-org:device-1-0\"><r:URLBase>http://10.0.0.1:49000/</r:URLBase>"
        "<r:device><r:serviceList><r:service><r:serviceType>urn:schemas-upnp-org:service:WANPPPConnection:1</r:serviceType>"
        "<r:controlURL>upnp/control/WANPPPConn1</r:controlURL></r:service></r:serviceList></r:device></r:root>";
    CHECK(portmap_upnp_parse_desc(d2, "http://10.0.0.1:49000/igddesc.xml", c, sizeof c, st, sizeof st));
    CHECK(!strcmp(c, "http://10.0.0.1:49000/upnp/control/WANPPPConn1"));
    CHECK(!strcmp(st, "urn:schemas-upnp-org:service:WANPPPConnection:1"));
    /* IGD v2 preferred over v1 when both are listed; absolute control URL */
    const char *d3 =
        "<root><device><serviceList>"
        "<service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType><controlURL>/v1</controlURL></service>"
        "<service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:2</serviceType>"
        "<controlURL>http://192.168.0.1:1900/v2</controlURL></service></serviceList></device></root>";
    CHECK(portmap_upnp_parse_desc(d3, "http://192.168.0.1:1900/d.xml", c, sizeof c, st, sizeof st));
    CHECK(!strcmp(c, "http://192.168.0.1:1900/v2") && strstr(st, "WANIPConnection:2"));
    CHECK(!portmap_upnp_parse_desc("<root><device></device></root>", "http://1.2.3.4/", c, sizeof c, st, sizeof st));

    char v[64];
    const char *ok = "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
                     "<u:GetExternalIPAddressResponse xmlns:u=\"urn:schemas-upnp-org:service:WANIPConnection:1\">"
                     "<NewExternalIPAddress>203.0.113.9</NewExternalIPAddress></u:GetExternalIPAddressResponse></s:Body></s:Envelope>";
    CHECK(portmap_xml_value(ok, "NewExternalIPAddress", v, sizeof v) && !strcmp(v, "203.0.113.9"));
    const char *fault = "<s:Envelope><s:Body><s:Fault><faultcode>s:Client</faultcode><detail><UPnPError "
                        "xmlns=\"urn:schemas-upnp-org:control-1-0\"><errorCode>725</errorCode><errorDescription>"
                        "OnlyPermanentLeasesSupported</errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>";
    CHECK(portmap_xml_value(fault, "errorCode", v, sizeof v) && !strcmp(v, "725"));
    CHECK(portmap_xml_value("<a><b/><c>x</c></a>", "b", v, sizeof v) && !strcmp(v, ""));
    CHECK(!portmap_xml_value("<a><c>x</c></a>", "b", v, sizeof v));

    char u[256];
    CHECK(portmap_url_join("http://1.2.3.4:80/a/b.xml", "/c", u, sizeof u) && !strcmp(u, "http://1.2.3.4:80/c"));
    CHECK(portmap_url_join("http://1.2.3.4/a/b.xml", "c/d", u, sizeof u) && !strcmp(u, "http://1.2.3.4:80/a/c/d"));
    CHECK(portmap_url_join("http://1.2.3.4/a/", "http://5.6.7.8:9/x", u, sizeof u) && !strcmp(u, "http://5.6.7.8:9/x"));
    CHECK(!portmap_url_join("ftp://x/", "c", u, sizeof u));
}

static void test_http(void) {
    char a[256] = "HTTP/1.1 200 OK\r\nContent-Type: text/xml\r\nContent-Length: 5\r\n\r\nhelloEXTRA";
    int st = 0, bl = 0;
    char *body;
    CHECK(portmap_http_parse(a, (int)strlen(a), &st, &body, &bl) == 0 && st == 200 && bl == 5 && !strcmp(body, "hello"));
    char b[256] = "HTTP/1.1 500 Internal Server Error\r\nTRANSFER-ENCODING: chunked\r\n\r\n4\r\nabcd\r\nA\r\n0123456789\r\n0\r\n\r\n";
    CHECK(portmap_http_parse(b, (int)strlen(b), &st, &body, &bl) == 0 && st == 500 && bl == 14 && !strcmp(body, "abcd0123456789"));
    char c[64] = "HTTP/1.0 404 Not Found\r\n\r\n";
    CHECK(portmap_http_parse(c, (int)strlen(c), &st, &body, &bl) == 0 && st == 404 && bl == 0);
    char d[64] = "garbage\r\n\r\n";
    CHECK(portmap_http_parse(d, (int)strlen(d), &st, &body, &bl) == -1);
    char e[64] = "HTTP/1.1 200 OK\r\nX: y";
    CHECK(portmap_http_parse(e, (int)strlen(e), &st, &body, &bl) == -1);
}

int main(void) {
    test_classify();
    test_natpmp();
    test_pcp();
    test_upnp();
    test_http();
    char gw[64];
    if (plat_default_gateway_ipv4(gw, sizeof gw)) {
        printf("default gateway (local routing table, nothing sent): %s\n", gw);
    } else printf("no IPv4 default gateway on this machine\n");
    if (fails) { printf("test_portmap: %d FAILED\n", fails); return 1; }
    printf("test_portmap: all passed\n");
    return 0;
}
