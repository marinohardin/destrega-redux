/* Netplay presentation (runtime/netplay_ui.c): the Netplay menu's status line, the waiting panel's
 * text, "Copy My Address" / "Copy Out-of-Sync Report Location", and the tolerant parsing of what a
 * player types or pastes into Host Game / Join Game. Shared by the macOS and Windows menus and the
 * in-window menu (menu_overlay.c, Linux) and the --menu-action hooks.
 *
 * Presentation only: it reads netplay.c's state through netplay_ui_info() and the router's answer
 * through portmap.c, writes the clipboard and the panel, and calls the same netplay_menu_* entry
 * points as before. It never touches emulated state or the protocol, so it is not part of the
 * netplay build identity (tools/source_ident.py). */
#ifndef NETPLAY_UI_H
#define NETPLAY_UI_H
#include <stdbool.h>
#include <stddef.h>

enum { NPUI_LOBBY_OFF, NPUI_LOBBY_HOSTING, NPUI_LOBBY_JOINING };

/* What netplay.c knows, for display (filled by netplay.c). */
typedef struct {
    int lobby;                    /* NPUI_LOBBY_*: Host Game / Join Game waiting for the other side */
    int port;                     /* the lobby's port */
    const char *join_addr;        /* joining: the host's address as typed */
    bool session, host;           /* a session runs; this side hosts it */
    bool connected;               /* the session's handshake is done */
    const char *peer;             /* "a.b.c.d:port" once connected */
    double ping_ms;               /* smoothed network round trip */
    bool quality_known;           /* the per-second figures below exist */
    double rollbacks_per_s;       /* last second */
    double waiting_pct;           /* % of the last second spent waiting for the opponent */
    int peer_load;                /* opponent's share of its frame budget in %, -1 = unknown */
    bool peer_left;
    unsigned long long desync_frame;   /* first out-of-sync frame of this session, 0 = none */
    const char *desync_report;    /* the last desync report this process wrote ("" = none) */
} NetplayUiInfo;
void netplay_ui_info(NetplayUiInfo *out);   /* netplay.c */

/* netplay.c entry points the actions call */
void netplay_menu_host(int port, int delay);
void netplay_menu_join(const char *addr, int port, int delay);
void netplay_menu_disconnect(void);
bool netplay_busy(void);

/* One line for the Netplay menu's status item (also the old netplay_status_text name, menu.h). */
const char *netplay_status_text(void);
/* The waiting panel (Host Game / Join Game in progress): rebuild its text and show / hide it. */
void netplay_ui_panel_update(void);

/* Menu actions netplay-host=PORT[,DELAY] and netplay-join=HOST:PORT[,DELAY]: parse leniently
 * (whitespace, pasted text), alert with a plain reason when the input is unusable. */
void netplay_ui_host_action(const char *arg);
void netplay_ui_join_action(const char *arg);
/* netplay-copy-address (while hosting) and netplay-copy-report (once a desync report exists). */
bool netplay_ui_allowed(const char *name);
const char *netplay_ui_refusal(const char *name);
void netplay_ui_copy(const char *name);
/* Title of the Copy item: "Copy My Address" or, without an internet address from the router,
 * "Copy My Home-Network Address". */
const char *netplay_ui_copy_title(void);

/* Parsing for the dialogs and the actions. Both trim spaces, tabs and line breaks (pasted text) and
 * write a plain-language reason to err on failure.
 *   address: "host:port", "host port", "host" (+ default_port; 0 = the port is required); host is an
 *            IPv4 address or a name (letters, digits, '.', '-').
 *   port:    "7000", ":7000" or "anything:7000" (an address pasted into the port field). */
bool netplay_ui_parse_address(const char *text, int default_port, char *host, size_t hn, int *port, char *err, size_t en);
bool netplay_ui_parse_port(const char *text, int *port, char *err, size_t en);
bool netplay_ui_parse_delay(const char *text, int *delay, char *err, size_t en);
#endif
