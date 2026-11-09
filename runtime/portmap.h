/* Automatic port forwarding for the netplay host (runtime/portmap.c).
 *
 * When a player hosts, the game asks the router to forward the host's UDP port for a while and
 * shows the address the opponent should type (the router's internet address and the forwarded
 * port). The joiner needs nothing: it sends first, so its own router lets the replies in.
 *
 * Protocols, tried in order: PCP (RFC 6887) and NAT-PMP (RFC 6886), both UDP to the default
 * gateway's port 5351, then UPnP IGD (SSDP discovery, the device description, SOAP
 * AddPortMapping on WANIPConnection / WANPPPConnection). All of it runs on one worker thread with
 * timeouts of a few seconds, so the frame loop never waits for the router. The mapping is leased
 * (3 hours), renewed at half its lifetime while hosting, and deleted when hosting ends (cancel,
 * Disconnect, the opponent leaving, exit). Across the synced restart into a session
 * (frontend_restart_with) the mapping is kept: the old process hands its parameters (PCP nonce,
 * external port, UPnP control URL) to the new one in DESTREGA_PORTMAP_KEEP, and the new process
 * renews the same mapping and deletes it when the session ends.
 *
 * Host-side plumbing only: nothing here can reach emulated state, so it is not part of the netplay
 * build identity (tools/source_ident.py).
 *
 * Safety for tests: only a windowed game talks to the real router (main.c calls
 * portmap_set_windowed(true) after creating the window; the default is off, so this fails closed
 * and does not depend on how the command line is parsed). Headless runs, i.e. every test, map
 * nothing unless --net-portmap-gateway or --net-portmap-upnp points them at a fake router
 * (tools/fake_router.py), and with either override only the overridden protocols are tried (no
 * gateway lookup, no SSDP). */
#ifndef PORTMAP_H
#define PORTMAP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Options (--net-portmap on|off, --net-portmap-gateway HOST:PORT, --net-portmap-upnp URL,
 * --net-portmap-lease S). Returns the number of arguments used, 0 if not ours. */
int portmap_parse_arg(int argc, char **argv, int i);
void portmap_usage(void);
/* main.c, once the window exists: this run may ask the real router (never called headless). */
void portmap_set_windowed(bool on);

/* Host: ask the router to forward UDP `port` (asynchronous; no-op when port mapping is off). */
void portmap_start(int port);
/* Hosting ended: delete the mapping (asynchronous). */
void portmap_stop(void);
/* The process is about to re-execute itself into the session: leave the mapping in place and hand
 * it to the new process (environment), never delete it here. */
void portmap_keep_for_restart(void);
/* Exit: delete the mapping and wait for it (at most ~3 s). Idempotent. */
void portmap_finish(void);
/* Main thread, once per vblank: true when portmap_text() changed. */
bool portmap_tick(void);
/* What the host should know, one sentence ("" when port mapping is not in use). */
const char *portmap_text(void);
/* Short form for the Netplay menu's status line ("" when not in use). */
const char *portmap_status_short(void);

/* ---- internals, exposed for runtime/tests/test_portmap.c ---------------------------------- */
enum { PM_ADDR_PUBLIC, PM_ADDR_CGNAT, PM_ADDR_PRIVATE, PM_ADDR_INVALID };
int portmap_classify_ipv4(const char *ip);
/* NAT-PMP: request bytes (returns the length) and response parsing (0 ok, -1 not a matching
 * response; *result = the protocol's result code). */
int portmap_natpmp_addr_req(uint8_t *b);
int portmap_natpmp_parse_addr(const uint8_t *b, int n, int *result, char *ip, size_t ipn);
int portmap_natpmp_map_req(uint8_t *b, int port, int ext, uint32_t lifetime);
int portmap_natpmp_parse_map(const uint8_t *b, int n, int port, int *result, int *ext, uint32_t *lifetime);
/* PCP MAP for UDP. client = the host's own IPv4 address (4 bytes). A NAT-PMP-only server answers
 * a PCP request with version 0: *result is then set to 1 (unsupported version). */
int portmap_pcp_map_req(uint8_t *b, const uint8_t client[4], const uint8_t nonce[12], int port, int ext, uint32_t lifetime);
int portmap_pcp_parse_map(const uint8_t *b, int n, const uint8_t nonce[12], int port, int *result, int *ext,
                          uint32_t *lifetime, char *ip, size_t ipn);
/* UPnP: the WAN connection service in a device description (absolute control URL resolved
 * against URLBase or the description's own URL). */
bool portmap_upnp_parse_desc(const char *xml, const char *location, char *control, size_t cn, char *service, size_t sn);
/* Text of the first element with this local name (namespace prefix ignored), entities decoded. */
bool portmap_xml_value(const char *xml, const char *name, char *out, size_t n);
/* Split an HTTP response in place: status code, body (chunked transfer decoded). -1 if malformed. */
int portmap_http_parse(char *raw, int n, int *status, char **body, int *body_len);
/* Resolve a (possibly relative) URL against a base URL. */
bool portmap_url_join(const char *base, const char *rel, char *out, size_t n);
#endif
