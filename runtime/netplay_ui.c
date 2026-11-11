/* Netplay presentation: status line, waiting panel, copy actions, lenient input parsing (see
 * netplay_ui.h). Presentation only; excluded from the netplay build identity. */
#include "netplay_ui.h"
#include "menu.h"
#include "platform.h"
#include "portmap.h"
#include "smooth.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define getcwd _getcwd
#else
#include <unistd.h>
#endif

void frontend_alert(const char *msg);          /* main.c: native alert (headless: log only) */
bool frontend_copy_text(const char *text);     /* main.c: clipboard (false headless: nothing copied) */

/* ---- small helpers ------------------------------------------------------------------------- */

/* A ping for people: one decimal below 10 ms (loopback / LAN), whole milliseconds above. */
static const char *ping_text(double ms) {
    static char t[2][24];
    static int k;
    k ^= 1;
    snprintf(t[k], sizeof t[k], ms < 10 ? "%.1f ms" : "%.0f ms", ms);
    return t[k];
}

/* Connection quality in one word, from the last second: round trip, rollbacks (corrections the
 * player may notice as a short jump) and time spent waiting for the opponent (stutter). */
static const char *quality(const NetplayUiInfo *i) {
    if (!i->quality_known) return NULL;
    if (i->ping_ms < 40 && i->rollbacks_per_s < 3 && i->waiting_pct < 2) return "excellent";
    if (i->ping_ms < 90 && i->rollbacks_per_s < 8 && i->waiting_pct < 5) return "good";
    if (i->ping_ms < 160 && i->waiting_pct < 15) return "fair";
    return "poor";
}

/* This computer's home-network address: a private one first (a VPN or tunnel address is listed
 * by the system too, and often first), else the first one. "" if there is none. */
static void home_address(char *out, size_t n) {
    char all[256], best[64] = "";
    out[0] = 0;
    if (plat_local_ipv4(all, sizeof all) <= 0) return;
    for (char *p = strtok(all, ", "); p; p = strtok(NULL, ", ")) {
        if (!best[0]) snprintf(best, sizeof best, "%s", p);
        if (portmap_classify_ipv4(p) == PM_ADDR_PRIVATE) { snprintf(best, sizeof best, "%s", p); break; }
    }
    snprintf(out, n, "%s", best);
}

static char s_copied[160];     /* "Copied ... to the clipboard." shown in the panel until it changes */
static int s_copied_port;

/* ---- status line ----------------------------------------------------------------------------- */

const char *netplay_status_text(void) {
    static char t[512];
    NetplayUiInfo i;
    netplay_ui_info(&i);
    if (i.lobby == NPUI_LOBBY_HOSTING) {
        char a[96];
        int r = portmap_address(a, sizeof a);
        if (r == PM_RES_PUBLIC) snprintf(t, sizeof t, "Waiting for an opponent on port %d (your address: %s)", i.port, a);
        else {
            const char *pm = portmap_status_short();
            snprintf(t, sizeof t, "Waiting for an opponent on port %d%s%s%s", i.port, *pm ? " (" : "", pm, *pm ? ")" : "");
        }
    } else if (i.lobby == NPUI_LOBBY_JOINING) {
        snprintf(t, sizeof t, "Connecting to %s:%d...", i.join_addr ? i.join_addr : "", i.port);
    } else if (i.session && i.desync_frame) {
        snprintf(t, sizeof t, "Out of sync since frame %llu (a report was saved)", i.desync_frame);
    } else if (i.session && i.peer_left) {
        snprintf(t, sizeof t, "Your opponent left");
    } else if (i.session && i.connected) {
        const char *q = quality(&i);
        int n = snprintf(t, sizeof t, "Connected to %s, ping %s%s%s", i.peer ? i.peer : "your opponent",
                         ping_text(i.ping_ms), q ? ", connection " : "", q ? q : "");
        if (n > 0 && (size_t)n < sizeof t) {
            if (i.peer_load >= 95) snprintf(t + n, sizeof t - (size_t)n, " (your opponent's computer is running slowly)");
            else if (smooth_load() >= 0.95) snprintf(t + n, sizeof t - (size_t)n, " (this computer is running slowly)");
        }
    } else if (i.session) {
        snprintf(t, sizeof t, "Connecting to your opponent...");
    } else {
        snprintf(t, sizeof t, "Not connected");
    }
    return t;
}

/* ---- the waiting panel ----------------------------------------------------------------------- */

void netplay_ui_panel_update(void) {
    static char last[1200];
    static bool shown;
    NetplayUiInfo i;
    netplay_ui_info(&i);
    char m[1200];
    bool copy = false;
    if (i.lobby == NPUI_LOBBY_HOSTING) {
        char a[96], home[64];
        int r = portmap_address(a, sizeof a);
        home_address(home, sizeof home);
        const char *pm = portmap_text();
        int n = snprintf(m, sizeof m, "Waiting for an opponent to join on port %d...\n\n", i.port);
        if (r == PM_RES_PUBLIC) {
            n += snprintf(m + n, sizeof m - (size_t)n, "Give your opponent this address: %s\n", a);
            if (*home) n += snprintf(m + n, sizeof m - (size_t)n, "On your home network: %s:%d\n", home, i.port);
        } else {
            if (*pm) n += snprintf(m + n, sizeof m - (size_t)n, "%s\n\n", pm);
            if (*home)
                n += snprintf(m + n, sizeof m - (size_t)n, "On your home network, your opponent joins with %s:%d.%s\n", home,
                              i.port, r == PM_RES_ASKING ? "" :
                              " Players outside your home network need your internet address instead.");
            else n += snprintf(m + n, sizeof m - (size_t)n, "This computer has no network address right now.\n");
        }
        if (s_copied[0] && s_copied_port == i.port && n > 0 && (size_t)n < sizeof m)
            snprintf(m + n, sizeof m - (size_t)n, "\n%s", s_copied);
        copy = r == PM_RES_PUBLIC || *home;
    } else if (i.lobby == NPUI_LOBBY_JOINING) {
        snprintf(m, sizeof m, "Connecting to %s:%d...\n\nWaiting for the host to answer. Both games restart from the "
                 "beginning when you connect.", i.join_addr ? i.join_addr : "", i.port);
    } else {
        s_copied[0] = 0;
        if (shown) { menu_netplay_progress(NULL, false); shown = false; last[0] = 0; }
        return;
    }
    if (shown && !strcmp(m, last)) return;
    snprintf(last, sizeof last, "%s", m);
    shown = true;
    char one[1200];   /* log on one line */
    snprintf(one, sizeof one, "%s", m);
    for (char *p = one; *p; p++) if (*p == '\n') *p = ' ';
    fprintf(stderr, "netplay: panel: %s\n", one);
    menu_netplay_progress(m, copy);
}

/* ---- parsing --------------------------------------------------------------------------------- */

/* Trim spaces, tabs, line breaks and no-break spaces (UTF-8 C2 A0) at both ends, and a pair of
 * quotes around the whole text; inner whitespace is kept for the caller. */
static void clean(const char *in, char *out, size_t n) {
    snprintf(out, n, "%s", in ? in : "");
    for (;;) {
        size_t l = strlen(out);
        char *p = out;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || ((unsigned char)p[0] == 0xc2 && (unsigned char)p[1] == 0xa0))
            p += (unsigned char)*p == 0xc2 ? 2 : 1;
        memmove(out, p, strlen(p) + 1);
        l = strlen(out);
        while (l && (out[l - 1] == ' ' || out[l - 1] == '\t' || out[l - 1] == '\r' || out[l - 1] == '\n' || out[l - 1] == '/'))
            out[--l] = 0;
        while (l >= 2 && (unsigned char)out[l - 2] == 0xc2 && (unsigned char)out[l - 1] == 0xa0) { l -= 2; out[l] = 0; }
        if (l >= 2 && ((out[0] == '"' && out[l - 1] == '"') || (out[0] == '\'' && out[l - 1] == '\''))) {
            out[l - 1] = 0;
            memmove(out, out + 1, l - 1);
            continue;
        }
        return;
    }
}

static bool all_digits(const char *s) {
    if (!*s) return false;
    for (; *s; s++) if (!isdigit((unsigned char)*s)) return false;
    return true;
}

static bool port_value(const char *s, int *port, char *err, size_t en) {
    long v = all_digits(s) && strlen(s) <= 5 ? strtol(s, NULL, 10) : -1;
    if (v < 1 || v > 65535) {
        snprintf(err, en, "\"%s\" is not a port. The port is a number from 1 to 65535, for example 7000.", s);
        return false;
    }
    *port = (int)v;
    return true;
}

bool netplay_ui_parse_port(const char *text, int *port, char *err, size_t en) {
    char s[256];
    clean(text, s, sizeof s);
    if (!*s) { snprintf(err, en, "Type a port, for example 7000."); return false; }
    const char *c = strrchr(s, ':');   /* ":7000" or a whole address pasted into the port field */
    if (c) { char t[256]; clean(c + 1, t, sizeof t); return port_value(t, port, err, en); }
    return port_value(s, port, err, en);
}

bool netplay_ui_parse_delay(const char *text, int *delay, char *err, size_t en) {
    char s[64];
    clean(text, s, sizeof s);
    long v = all_digits(s) && strlen(s) <= 3 ? strtol(s, NULL, 10) : -1;
    if (v < 1 || v > 30) {
        snprintf(err, en, "The input delay is a number of frames from 1 to 30 (2 suits most connections).");
        return false;
    }
    *delay = (int)v;
    return true;
}

bool netplay_ui_parse_address(const char *text, int default_port, char *host, size_t hn, int *port, char *err, size_t en) {
    char s[512];
    clean(text, s, sizeof s);
    host[0] = 0;
    if (!strncmp(s, "udp://", 6)) memmove(s, s + 6, strlen(s + 6) + 1);
    if (!*s) {
        snprintf(err, en, "Type the address your opponent gave you, for example 192.168.1.20:7000.");
        return false;
    }
    char h[512], p[64] = "";
    const char *c = strchr(s, ':');
    if (c && strchr(c + 1, ':')) {
        snprintf(err, en, "\"%s\" looks like an IPv6 address. Netplay needs an IPv4 address such as 192.168.1.20:7000.", s);
        return false;
    }
    if (c) {
        snprintf(h, sizeof h, "%.*s", (int)(c - s), s);
        clean(c + 1, p, sizeof p);
    } else {
        /* "192.168.1.20 7000": the port after a space */
        const char *sp = strrchr(s, ' ');
        if (!sp) sp = strrchr(s, '\t');
        if (sp && all_digits(sp + 1)) { snprintf(h, sizeof h, "%.*s", (int)(sp - s), s); snprintf(p, sizeof p, "%s", sp + 1); }
        else snprintf(h, sizeof h, "%s", s);
    }
    char hc[512];
    clean(h, hc, sizeof hc);
    if (!*hc) { snprintf(err, en, "The address is missing before the port. Type it as address:port, for example 192.168.1.20:7000."); return false; }
    for (const char *q = hc; *q; q++)
        if (!isalnum((unsigned char)*q) && *q != '.' && *q != '-') {
            snprintf(err, en, "\"%s\" is not an address. Type it as address:port, for example 192.168.1.20:7000.", hc);
            return false;
        }
    if (strlen(hc) >= hn) { snprintf(err, en, "The address is too long."); return false; }
    if (*p) { if (!port_value(p, port, err, en)) return false; }
    else if (default_port > 0) *port = default_port;
    else { snprintf(err, en, "Add the port after the address, for example %s:7000.", hc); return false; }
    snprintf(host, hn, "%s", hc);
    return true;
}

/* ---- actions --------------------------------------------------------------------------------- */

void netplay_ui_host_action(const char *arg) {   /* PORT[,DELAY]; empty = 7000 */
    char a[256], err[300];
    clean(arg, a, sizeof a);
    int port = 7000, delay = 2;
    char *comma = strrchr(a, ',');
    if (comma) {
        *comma = 0;
        if (!netplay_ui_parse_delay(comma + 1, &delay, err, sizeof err)) { frontend_alert(err); return; }
    }
    if (*a && !netplay_ui_parse_port(a, &port, err, sizeof err)) { frontend_alert(err); return; }
    netplay_menu_host(port, delay);
}

void netplay_ui_join_action(const char *arg) {   /* HOST:PORT[,DELAY] */
    char a[512], err[400], host[256];
    clean(arg, a, sizeof a);
    int port = 0, delay = 2;
    char *comma = strrchr(a, ',');
    if (comma) {
        *comma = 0;
        if (!netplay_ui_parse_delay(comma + 1, &delay, err, sizeof err)) { frontend_alert(err); return; }
    }
    if (!netplay_ui_parse_address(a, 0, host, sizeof host, &port, err, sizeof err)) { frontend_alert(err); return; }
    netplay_menu_join(host, port, delay);
}

bool netplay_ui_allowed(const char *name) {
    NetplayUiInfo i;
    netplay_ui_info(&i);
    if (!strcmp(name, "netplay-copy-address")) return i.lobby == NPUI_LOBBY_HOSTING;
    if (!strcmp(name, "netplay-copy-report")) return i.desync_report && *i.desync_report;
    return true;
}

const char *netplay_ui_refusal(const char *name) {
    if (!strcmp(name, "netplay-copy-address")) return "not hosting a game (choose Host Game first)";
    if (!strcmp(name, "netplay-copy-report")) return "no out-of-sync report was written";
    return "not available";
}

const char *netplay_ui_copy_title(void) {
    char a[96];
    NetplayUiInfo i;
    netplay_ui_info(&i);
    if (i.lobby != NPUI_LOBBY_HOSTING) return "Copy My Address";
    return portmap_address(a, sizeof a) == PM_RES_PUBLIC ? "Copy My Address" : "Copy My Home-Network Address";
}

static void put_clipboard(const char *what, const char *text, const char *note) {
    bool ok = frontend_copy_text(text);
    fprintf(stderr, "netplay: copy %s: %s%s%s\n", what, text, note, ok ? "" : " [headless: the clipboard is left alone]");
}

void netplay_ui_copy(const char *name) {
    NetplayUiInfo i;
    netplay_ui_info(&i);
    if (!strcmp(name, "netplay-copy-report")) {
        if (!i.desync_report || !*i.desync_report) return;
        char path[2048], cwd[1024];
        if (plat_is_absolute(i.desync_report) || !getcwd(cwd, sizeof cwd)) snprintf(path, sizeof path, "%s", i.desync_report);
        else {
            const char *r = i.desync_report;
            while (r[0] == '.' && plat_is_sep(r[1])) r += 2;
            snprintf(path, sizeof path, "%s/%s", cwd, r);
        }
        put_clipboard("report location", path, "");
        return;
    }
    if (strcmp(name, "netplay-copy-address") || i.lobby != NPUI_LOBBY_HOSTING) return;
    char a[96], home[64], text[128];
    int r = portmap_address(a, sizeof a);
    if (r == PM_RES_PUBLIC) {
        put_clipboard("address", a, " (your internet address, from the router)");
        snprintf(s_copied, sizeof s_copied, "Copied %s to the clipboard: send it to your opponent.", a);
    } else {
        home_address(home, sizeof home);
        if (!*home) {
            frontend_alert("This computer has no network address right now, so there is nothing to copy. Check that it "
                           "is connected to a network.");
            return;
        }
        snprintf(text, sizeof text, "%s:%d", home, i.port);
        const char *why = r == PM_RES_ASKING ? "the router has not answered yet"
                        : r == PM_RES_UNREACHABLE ? "your internet provider doesn't allow incoming connections"
                        : r == PM_RES_FAILED ? "the router did not open the port"
                        : "automatic port opening is off";
        char note[300];
        snprintf(note, sizeof note, " (your home-network address: it only works for players on your home network; %s, so "
                 "players outside it need your internet address)", why);
        put_clipboard("address", text, note);
        snprintf(s_copied, sizeof s_copied, "Copied your home-network address %s to the clipboard. It only works for "
                 "players on your home network.", text);
    }
    s_copied_port = i.port;
    netplay_ui_panel_update();
}
