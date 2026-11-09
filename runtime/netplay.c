/* Rollback netplay (GGPO-style) on top of save states (savestate.c) and UDP (net.c).
 *
 * Frames are the frontend's vblank frames (60 Hz). Each peer owns one pad: the host is player 1,
 * the joining peer player 2; each peer's local input is whatever its frontend put into pad 1
 * (keyboard / gamepad / --press). The local input sampled at the end of frame F is used for frame
 * F + delay on both peers. A remote input not yet received is predicted (its last known value).
 *
 * Rollback: when a remote input arrives that differs from the one used for frame M, the newest
 * snapshot taken at the start of a frame K <= M whose host context matches the current one is
 * restored and frames K .. now-1 are re-simulated with the corrected inputs (emulation only:
 * no presentation, no audio output, hi-res rendering only for the last few re-simulated frames).
 * Snapshots are restorable only where the host call chain matches (savestate.c): in fights that is
 * every second vblank (the VSync wait of each 30 Hz game update), so a rollback restores at most
 * window + 1 vblanks back and may wait one vblank for a matching point.
 *
 * Window: counted in vblanks (inputs, packets and frame numbers are per vblank). The default 8
 * vblanks = 4 game updates = 133 ms. Prediction beyond it stalls.
 *
 * Prediction is only allowed while a recent snapshot with an older-or-equal frame than the first
 * unconfirmed one has the current (or previous) vblank's host context: in loops that recur
 * (fights, menus waiting in VSync). Elsewhere (loads, scene changes) peers run in lockstep with
 * the input delay. A misprediction found after the game left such a loop cannot be rolled back
 * ("rollback impossible"); it is counted and desync detection (milestone 4) would catch any effect.
 *
 * Final ("confirmed") frames: the state at the start of frame F is final once every input of
 * frames < F is confirmed and was used. Their gameplay hash (and every --net-hash-every frames the
 * complete state hash) can be logged (--net-hash-log) and is exchanged for desync detection.
 */
#include "savestate.h"
#include "net.h"
#include "hires.h"
#include "memcard.h"
#include "smooth.h"
#include "portmap.h"   /* host: automatic port forwarding (not part of the build identity) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "source_ident.h"   /* DESTREGA_SOURCE_IDENT (tools/source_ident.py, generated) */

u64 gameplay_hash(void);                    /* replay.c */
u32 replay_build_id(void);
void replay_disc_id(u32 *crc, u32 *sectors);
void ss_set_resim(bool on);
bool ss_restore_ex(const Snapshot *s, bool reseed_hires);
u64 ss_now_ns(void);
void frontend_set_max_frames(u64 n);       /* main.c */
void frontend_alert(const char *msg);      /* main.c: native alert (logged; headless: log only) */
void frontend_pump_events(void);           /* main.c: keep the window responsive while waiting */
void frontend_restart_with(const char *const *args, int n);   /* main.c: clean re-exec + args */
void menu_netplay_progress(const char *text);                  /* menu_mac.m / menu_stub.c */
void replay_record_in_memory(void);        /* replay.c: desync reports are replay files */
bool replay_write(const char *path, u64 last);
u64 replay_last_frame(void);
int replay_field_count(void);
const char *replay_field_name(int k);
void replay_fields_now(u32 *val, u16 *hash);
void replay_field_str(int k, u32 v, char *b, size_t n);
#define NF 16                               /* gameplay fields (replay.c) */

#define RING 256                            /* frames of input / hash history (power of two) */
#define SNAPS 24                            /* snapshot ring */

enum { PKT_HELLO = 1, PKT_WELCOME = 2, PKT_INPUT = 3, PKT_BYE = 4,
       PKT_LOBBY_HELLO = 5, PKT_LOBBY_WELCOME = 6, PKT_LOBBY_REFUSE = 7 };   /* menu sessions (below) */
#define NET_MAGIC 0x504e5344u               /* "DSNP" */
#define NET_VERSION 2                       /* 2: the hello carries the emulation source identity */

typedef struct {
    bool on, host;
    char addr[256];
    int port, local_port;
    int delay, window;
    double pace_hz;                          /* headless pacing */
    bool unpaced;                            /* headless without --net-pace: no time sync waits */
    NetSim sim;
    const char *hash_log;
    const char *desync_dir;
    u64 inject_frame;                        /* test: corrupt p1 hp at the start of this frame */
    u32 fake_build, fake_source;             /* test: pretend to be different game code / sources */
    bool desync_stop;
    bool lockstep;                           /* never predict (debugging / comparison) */
    bool from_menu;                          /* started from the Netplay menu: alerts instead of exits */
    bool full_boot;                          /* --net-full-boot: the session boots through the intro */
    bool sync_hard;                          /* --net-sync-hard: time sync by stopping (old, comparisons) */
    bool boot_to_vs_offline;                 /* --boot-to-vs: the same shortcut without netplay (tests) */
    int hash_every;
    double timeout_s;
} Opts;
static Opts o = { .delay = 2, .window = 8, .hash_every = 30, .timeout_s = 15 };

static NetSock *s_sock;
static bool s_started;
static int s_local_idx, s_remote_idx;       /* pad index of each player (0 = pad 1) */

static u16 s_local_in[RING];                /* local input by frame (already delayed) */
static u16 s_remote_in[RING];               /* remote input by frame, when received */
static bool s_remote_have[RING];
static u64 s_remote_frame[RING];            /* which frame the slot holds (ring aliasing) */
static u16 s_used_remote[RING];             /* remote input actually used for frame k */
static bool s_used_pred[RING];
static u64 s_applied;                       /* last frame whose inputs were applied (end hook) */
static u64 s_remote_conf;                   /* all remote inputs <= this are known */
static u64 s_peer_ack;                      /* peer has all our inputs <= this */
static u64 s_local_through;                 /* our inputs are known up to this frame */
static u64 s_first_bad;                     /* earliest mispredicted frame (0 = none) */
static u16 s_last_remote;                   /* prediction source */
static bool s_peer_left, s_pred_prev;
static char s_end_msg[512];                 /* session end requested (peer left, desync, timeout) */
static bool s_end_pending;
static void end_later(const char *msg) {
    if (s_end_pending) return;
    s_end_pending = true;
    snprintf(s_end_msg, sizeof s_end_msg, "%s", msg ? msg : "");
}
static char s_card_path[2][2048];          /* cards detached for the session, re-attached after */
static u64 s_cur_frame, s_peer_frame;       /* time sync (GGPO-style frame advantage) */
static double s_adv_avg, s_peer_adv;
static double s_sync_debt_ns;               /* time-sync slowdown still to spread over the next frames */
static u64 s_sync_next;                     /* first frame the next slowdown may be decided */
static u64 s_fe_extra_ns;                   /* windowed: slowdown for the frontend's pacing to add */
static int s_peer_load = -1;                /* peer's share of its frame budget in use (%), -1 = unknown */
static u32 s_echo_ts;                       /* round-trip time */
static u64 s_echo_at;
static double s_rtt;

static Snapshot s_snap[SNAPS];
static u64 s_resim_until;                   /* re-simulating until begin(this frame) */
static u64 s_resim_from;
static u64 s_rb_depth, s_rb_shown;          /* current rollback: depth, frames shown before the fix */
static const HiresBackend *s_hires_saved;
static bool s_hires_off;

static u64 s_gh[RING], s_fh[RING];          /* hashes at the start of frame k (latest simulation) */
static u64 s_hframe[RING];
static u16 s_fld[RING][NF];                 /* 16-bit field hashes (replay.c) at the start of frame k */
static u32 s_fval[RING][NF];                /* and the values (local reports only, never sent) */
static u64 s_final_through;
static FILE *s_hash_log;

static struct {
    u64 frames, predicted, mispredicted, rollbacks, resim_frames, deferred, impossible;
    u64 sync_waits, sync_frames;
    u64 stalls, stall_ns, resim_ns, max_resim_ns, max_depth, pkts_in, pkts_out;
} st;
static u64 s_t0_ns;

/* ---- helpers --------------------------------------------------------------------------- */

static inline u32 slot(u64 f) { return (u32)(f & (RING - 1)); }

static bool remote_known(u64 f) { return s_remote_have[slot(f)] && s_remote_frame[slot(f)] == f; }

static void put_u32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static u32 get_u32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static void put_u64(u8 *p, u64 v) { put_u32(p, (u32)v); put_u32(p + 4, (u32)(v >> 32)); }
static u64 get_u64(const u8 *p) { return get_u32(p) | (u64)get_u32(p + 4) << 32; }

static void fatal(const char *msg) {
    fprintf(stderr, "netplay: %s\n", msg);
    exit(8);
}

/* ---- handshake ---------------------------------------------------------------------------- */

/* What both peers must share (the rule is explained in tools/source_ident.py):
 *   disc_crc, sectors: the same game disc;
 *   build: the recompiled-code table (replay_build_id: every recompiled function's address,
 *          overlay, bounds and the hash of its original MIPS bytes), i.e. the same game code;
 *   source: the emulation sources (runtime + recompiler + its analysis inputs), line endings
 *           normalised, platform-only and presentation-only files left out; no compiler or OS;
 *   window (and delay for the session handshake): the same netplay settings. */
typedef struct { u32 disc_crc, sectors, build, delay, window, source; } Ident;

static void ident_local(Ident *id) {
    replay_disc_id(&id->disc_crc, &id->sectors);
    id->build = replay_build_id();
    id->delay = (u32)o.delay;
    id->window = (u32)o.window | (o.full_boot ? 0x80000000u : 0);   /* the boot shortcut must match too */
    id->source = DESTREGA_SOURCE_IDENT;
    if (o.fake_build) id->build = o.fake_build;
    if (o.fake_source) id->source = o.fake_source;
}

static int pkt_ident(u8 *b, int type, const Ident *id) {
    put_u32(b, NET_MAGIC); b[4] = (u8)type; b[5] = NET_VERSION;
    put_u32(b + 8, id->disc_crc); put_u32(b + 12, id->sectors); put_u32(b + 16, id->build);
    put_u32(b + 20, id->delay); put_u32(b + 24, id->window); put_u32(b + 28, id->source);
    return 32;
}

/* NULL: compatible; "": not a packet of that type; else a plain-language reason. */
static const char *ident_check_ex(const u8 *b, int n, int want_type, bool check_delay) {
    static char err[256];
    if (n < 28 || get_u32(b) != NET_MAGIC || b[4] != want_type) return "";
    if (b[5] != NET_VERSION) {   /* checked before the length: older versions send shorter hellos */
        snprintf(err, sizeof err, "the two players run different versions of Destrega (netplay version %d and %d); "
                 "both need the same version", b[5], NET_VERSION);
        return err;
    }
    if (n < 32) return "";
    Ident me, peer = { get_u32(b + 8), get_u32(b + 12), get_u32(b + 16), get_u32(b + 20), get_u32(b + 24), get_u32(b + 28) };
    ident_local(&me);
    if (peer.disc_crc != me.disc_crc || peer.sectors != me.sectors)
        snprintf(err, sizeof err, "the two players imported different game discs (%08x/%u and %08x/%u); both need the "
                 "North American release", peer.disc_crc, peer.sectors, me.disc_crc, me.sectors);
    else if (peer.build != me.build || peer.source != me.source)
        snprintf(err, sizeof err, "the two players run different versions of Destrega (%08x-%08x and %08x-%08x); both "
                 "need the same version", peer.build, peer.source, me.build, me.source);
    else if (peer.window != me.window || (check_delay && peer.delay != me.delay))
        snprintf(err, sizeof err, "the two players chose different netplay settings (input delay %u / %u, rollback window "
                 "%u / %u, intro skip %s / %s)", peer.delay, me.delay, peer.window & 0xffff, me.window & 0xffff,
                 peer.window >> 31 ? "off" : "on", me.window >> 31 ? "off" : "on");
    else return NULL;
    return err;
}
static const char *ident_check(const u8 *b, int n, int want_type) { return ident_check_ex(b, n, want_type, true); }

/* Returns NULL when connected, else a message for the player. */
static const char *handshake(void) {
    static char err[320];
    u8 b[1500];
    Ident me;
    ident_local(&me);
    u64 t0 = ss_now_ns(), last = 0;
    double limit = o.from_menu ? 30 : o.timeout_s * 4;
    fprintf(stderr, "netplay: %s, waiting for the peer...\n", o.host ? "hosting" : "joining");
    for (;;) {
        u64 now = ss_now_ns();
        if ((double)(now - t0) / 1e9 > limit) {
            snprintf(err, sizeof err, "%s", o.host ? "Your opponent did not connect in time." :
                     "No answer from the host. Check the address and that your opponent is hosting.");
            return err;
        }
        if (!o.host && now - last > 100000000ull) {     /* join: hello every 100 ms */
            int n = pkt_ident(b, PKT_HELLO, &me);
            net_send(s_sock, b, n);
            last = now;
        }
        int n;
        while ((n = net_recv(s_sock, b, sizeof b)) > 0) {
            if (o.host) {
                const char *e = ident_check(b, n, PKT_HELLO);
                if (e && !*e) continue;
                if (e) { fprintf(stderr, "netplay: refusing peer: %s\n", e); continue; }
                net_lock_peer(s_sock);
                int m = pkt_ident(b, PKT_WELCOME, &me);
                for (int k = 0; k < 5; k++) net_send(s_sock, b, m);   /* the first input packets re-ack anyway */
                fprintf(stderr, "netplay: peer %s joined\n", net_peer_name(s_sock));
                return NULL;
            } else {
                const char *e = ident_check(b, n, PKT_WELCOME);
                if (e && !*e) continue;
                if (e) { snprintf(err, sizeof err, "The host refused the connection: %s.", e); return err; }
                fprintf(stderr, "netplay: joined %s\n", net_peer_name(s_sock));
                return NULL;
            }
        }
        net_poll(s_sock);
        frontend_pump_events();
        net_sleep_us(1000);
    }
}

/* ---- packets ------------------------------------------------------------------------------ */

/* input packet: magic, type, ack(u32), first(u32), count(u8), inputs u16[count],
 *               hash frame(u32), gameplay hash(u64), full hash(u64) */
static void send_inputs(void) {
    u8 b[1500];
    put_u32(b, NET_MAGIC); b[4] = PKT_INPUT; b[5] = 0;
    put_u32(b + 8, (u32)s_remote_conf);
    u64 first = s_peer_ack + 1;
    if (s_local_through < first) first = s_local_through + 1;
    u64 cnt = s_local_through + 1 - first;
    if (cnt > 120) { first = s_local_through + 1 - 120; cnt = 120; }
    put_u32(b + 12, (u32)first);
    b[16] = (u8)cnt;
    int n = 17;
    for (u64 k = 0; k < cnt; k++) { u16 v = s_local_in[slot(first + k)]; b[n++] = (u8)v; b[n++] = (u8)(v >> 8); }
    /* newest final frame's hashes (desync detection) */
    u64 hf = s_final_through;
    put_u32(b + n, (u32)hf);
    put_u64(b + n + 4, hf && s_hframe[slot(hf)] == hf ? s_gh[slot(hf)] : 0);
    put_u64(b + n + 12, hf && s_hframe[slot(hf)] == hf ? s_fh[slot(hf)] : 0);
    n += 20;
    /* time sync: our current frame and our frame advantage over the peer (x16) */
    put_u32(b + n, (u32)s_cur_frame);
    s16 adv = (s16)(s_adv_avg * 16 > 32000 ? 32000 : s_adv_avg * 16 < -32000 ? -32000 : s_adv_avg * 16);
    b[n + 4] = (u8)adv; b[n + 5] = (u8)((u16)adv >> 8);
    n += 6;
    /* field hashes of the hash frame: a desync report can name the differing fields */
    for (int k = 0; k < NF; k++) {
        u16 v = hf && s_hframe[slot(hf)] == hf ? s_fld[slot(hf)][k] : 0;
        b[n++] = (u8)v; b[n++] = (u8)(v >> 8);
    }
    /* round-trip time: our clock, the peer's last clock we saw and how long we held it (ms) */
    u64 now = ss_now_ns();
    put_u32(b + n, (u32)(now / 1000000));
    put_u32(b + n + 4, s_echo_ts);
    u64 hold = s_echo_at ? (now - s_echo_at) / 1000000 : 0;
    b[n + 8] = (u8)(hold > 65535 ? 65535 : hold); b[n + 9] = (u8)((hold > 65535 ? 65535 : hold) >> 8);
    n += 10;
    /* our load: % of the frame budget used by our own work (255 = unknown, e.g. headless) */
    double ld = smooth_load();
    b[n++] = ld <= 0 ? 255 : ld * 100 > 250 ? 250 : (u8)(ld * 100);
    net_send(s_sock, b, n);
    st.pkts_out++;
}

void netplay_on_peer_hash(u64 frame, u64 gh, u64 fh, const u16 *fld);   /* desync check (below) */
static void status(u64 f);                             /* connection-quality readout (below) */

static void receive(void) {
    u8 b[1500];
    int n;
    net_poll(s_sock);
    while ((n = net_recv(s_sock, b, sizeof b)) > 0) {
        if (n < 8 || get_u32(b) != NET_MAGIC) continue;
        if (b[4] == PKT_BYE) {   /* ends the session at the next safe moment (end_pending) */
            if (!s_peer_left) fprintf(stderr, "netplay: peer left at our frame %llu\n", (unsigned long long)s_applied);
            s_peer_left = true;
            end_later("Your opponent left the game. Continuing offline.");
            continue;
        }
        if (b[4] != PKT_INPUT || n < 17) continue;
        st.pkts_in++;
        u64 ack = get_u32(b + 8), first = get_u32(b + 12);
        int cnt = b[16];
        if (n < 17 + cnt * 2) continue;
        if (ack > s_peer_ack) s_peer_ack = ack;
        for (int k = 0; k < cnt; k++) {
            u64 f = first + (u64)k;
            if (f + RING / 2 < s_applied) continue;           /* ancient */
            u16 v = (u16)(b[17 + 2 * k] | b[18 + 2 * k] << 8);
            u32 s = slot(f);
            if (remote_known(f)) continue;
            s_remote_in[s] = v; s_remote_have[s] = true; s_remote_frame[s] = f;
            if (f <= s_applied) {                              /* used already: was the guess right? */
                if (s_used_remote[s] != v) {
                    st.mispredicted++;
                    if (!s_first_bad || f < s_first_bad) s_first_bad = f;
                }
            }
        }
        while (remote_known(s_remote_conf + 1)) s_remote_conf++;
        if (s_remote_conf && remote_known(s_remote_conf)) s_last_remote = s_remote_in[slot(s_remote_conf)];
        int h = 17 + cnt * 2;
        if (n >= h + 26 + 2 * NF) {
            u64 hf = get_u32(b + h);
            u16 fl[NF];
            for (int k = 0; k < NF; k++) fl[k] = (u16)(b[h + 26 + 2 * k] | b[h + 27 + 2 * k] << 8);
            if (hf) netplay_on_peer_hash(hf, get_u64(b + h + 4), get_u64(b + h + 12), fl);
        }
        int r = h + 26 + 2 * NF;
        if (n >= r + 10) {
            u32 ts = get_u32(b + r), echo = get_u32(b + r + 4);
            u32 hold = b[r + 8] | b[r + 9] << 8;
            u64 now = ss_now_ns();
            s_echo_ts = ts; s_echo_at = now;
            if (echo) {
                double rtt = (double)((u32)(now / 1000000) - echo) - hold;
                if (rtt >= 0 && rtt < 5000) s_rtt = s_rtt ? s_rtt + (rtt - s_rtt) / 8 : rtt;
            }
            if (n >= r + 11) s_peer_load = b[r + 10] == 255 ? -1 : b[r + 10];
        }
        if (n >= h + 26) {
            u64 pf = get_u32(b + h + 20);
            if (pf > s_peer_frame) s_peer_frame = pf;
            s_peer_adv = (s16)(b[h + 24] | b[h + 25] << 8) / 16.0;
        }
    }
}

/* ---- snapshots ---------------------------------------------------------------------------- */

static Snapshot *snap_for(u64 f) {
    Snapshot *best = &s_snap[0];
    for (int i = 0; i < SNAPS; i++) {
        if (s_snap[i].valid && s_snap[i].frame == f) return &s_snap[i];
        if (!s_snap[i].valid || s_snap[i].frame < best->frame) best = &s_snap[i];
    }
    return best;   /* oldest / free slot to reuse */
}

/* newest snapshot with frame <= f that can be restored right now */
static Snapshot *restorable(u64 f) {
    Snapshot *best = NULL;
    for (int i = 0; i < SNAPS; i++) {
        Snapshot *s = &s_snap[i];
        if (!s->valid || s->frame > f) continue;
        if (best && s->frame <= best->frame) continue;
        if (ss_context_matches(s)) best = s;
    }
    return best;
}

/* would a misprediction from frame f on still be fixable later? */
static bool prediction_ok(u64 f) {
    for (int i = 0; i < SNAPS; i++) {
        Snapshot *s = &s_snap[i];
        if (s->valid && s->frame <= f && f - s->frame <= (u64)o.window + 2 && ss_context_matches(s)) return true;
    }
    return false;
}

/* Prediction only while a round is being fought: round_time (800e379c, symbols.txt) advances by 2
 * every game update during a round and is frozen from the KO / time-out until the next round.
 * Everything else (menus, the round intro, KO and win animations, loads) runs in lockstep: there
 * the game leaves its loop at some point (a menu choice, the end of an animation), and a
 * misprediction found after the game left the loop of the snapshot could not be rolled back (the
 * host call chain differs, savestate.c). From the KO on, the fight loop keeps running for well
 * over the rollback window, so any prediction made before it is still correctable. */
#define ROUND_TIME 0x800e379cu
static u32 s_rt_last;
static u64 s_rt_changed;
static bool in_round(u64 f) {
    u32 t = mem_r32(ROUND_TIME);
    if (t != s_rt_last) { s_rt_last = t; s_rt_changed = f; }
    return s_rt_changed && f - s_rt_changed <= 4 && !o.lockstep;
}

/* ---- hashes (final frames) ---------------------------------------------------------------- */

static u64 s_peer_hash_frame[RING], s_peer_gh[RING], s_peer_fh[RING];
static u16 s_peer_fld[RING][NF];
static u64 s_desyncs, s_hash_checked, s_first_desync;
static void desync_report(u64 frame);   /* below */
static u64 s_report_deferred;           /* a report waiting for the recording to reach its frame */

static void check_hash(u64 f) {
    u32 s = slot(f);
    if (s_peer_hash_frame[s] != f || s_hframe[s] != f) return;
    s_hash_checked++;
    bool bad = s_peer_gh[s] != s_gh[s] || (s_peer_fh[s] && s_fh[s] && s_peer_fh[s] != s_fh[s]);
    if (bad) desync_report(f);
}

void netplay_on_peer_hash(u64 frame, u64 gh, u64 fh, const u16 *fld) {
    u32 s = slot(frame);
    if (s_peer_hash_frame[s] == frame) return;
    s_peer_hash_frame[s] = frame; s_peer_gh[s] = gh; s_peer_fh[s] = fh;
    memcpy(s_peer_fld[s], fld, sizeof s_peer_fld[s]);
    if (frame <= s_final_through) check_hash(frame);
}

static void emit_final(u64 upto) {
    while (s_final_through < upto) {
        u64 f = s_final_through + 1;
        if (s_hframe[slot(f)] != f) break;              /* not simulated yet */
        s_final_through = f;
        if (s_hash_log) {
            if (s_fh[slot(f)]) fprintf(s_hash_log, "%llu %016llx %016llx\n", (unsigned long long)f,
                                       (unsigned long long)s_gh[slot(f)], (unsigned long long)s_fh[slot(f)]);
            else fprintf(s_hash_log, "%llu %016llx\n", (unsigned long long)f, (unsigned long long)s_gh[slot(f)]);
        }
        check_hash(f);
    }
}

/* ---- hooks -------------------------------------------------------------------------------- */

bool netplay_active(void) { return o.on; }

static void print_stats(void);
static void session_end(const char *msg);
static void lobby_poll(void);
static void lobby_portmap_changed(void);

static bool start(void) {
    s_started = true;
    s_sock = net_open(o.host ? NULL : o.addr, o.port, o.host ? o.port : o.local_port, &o.sim);
    if (!s_sock) {
        if (!o.from_menu) fatal("cannot open the UDP socket");
        char m[160];
        snprintf(m, sizeof m, "Could not use network port %d (another program may be using it).", o.port);
        frontend_alert(m);
        o.on = false;
        return false;
    }
    s_local_idx = o.host ? 0 : 1;
    s_remote_idx = 1 - s_local_idx;
    if (o.host) portmap_start(o.port);   /* after a menu restart: renews the mapping the lobby made */
    const char *err = handshake();
    if (err) {
        portmap_stop();
        if (!o.from_menu) fatal(err);
        net_close(s_sock); s_sock = NULL;
        o.on = false; s_started = false;
        char m[400];
        snprintf(m, sizeof m, "Netplay could not start. %s The game continues offline.", err);
        frontend_alert(m);
        return false;
    }
    /* both peers must emulate the same machine: no memory cards, and nothing is ever written */
    memcard_flush();
    for (int p = 0; p < 2; p++) {
        const char *cp = memcard_path(p);
        snprintf(s_card_path[p], sizeof s_card_path[p], "%s", cp ? cp : "");
        memcard_attach(p, NULL);
    }
    memcard_set_write_protect(true);
    ss_set_deterministic_audio(true);   /* the SPU must advance the same on both peers */
    if (o.hash_log) s_hash_log = fopen(o.hash_log, "w");
    for (int k = 0; k <= o.delay; k++) { s_local_in[slot((u64)k)] = 0; }
    s_local_through = (u64)o.delay;    /* frames 1..delay: no input */
    for (u64 k = 1; k <= (u64)o.delay; k++) {
        s_remote_in[slot(k)] = 0; s_remote_have[slot(k)] = true; s_remote_frame[slot(k)] = k;
    }
    s_remote_conf = (u64)o.delay;
    s_t0_ns = ss_now_ns();
    s_sync_debt_ns = 0; s_sync_next = 0; s_fe_extra_ns = 0; s_peer_load = -1;
    smooth_governor(true);
    return true;
}

/* End the session and keep playing offline from the current state: pads go back to local input,
 * cards come back, nothing more is logged. Only between frames (never while re-simulating). */
static void session_end(const char *msg) {
    if (!o.on) return;
    fprintf(stderr, "netplay: session ended at frame %llu%s%s\n", (unsigned long long)s_cur_frame,
            msg && *msg ? ": " : "", msg ? msg : "");
    if (s_sock) {
        u8 b[8]; put_u32(b, NET_MAGIC); b[4] = PKT_BYE; b[5] = 0; b[6] = b[7] = 0;
        for (int k = 0; k < 3; k++) net_send(s_sock, b, 8);
        net_flush(s_sock, 200);
        net_close(s_sock);
        s_sock = NULL;
    }
    print_stats();
    portmap_stop();
    if (s_hash_log) { fclose(s_hash_log); s_hash_log = NULL; }
    o.hash_log = NULL;
    if (s_hires_off) { g_hires = s_hires_saved; s_hires_off = false; }
    smooth_governor(false);
    s_sync_debt_ns = 0; s_fe_extra_ns = 0;
    s_resim_until = 0;
    ss_set_resim(false);
    memcard_set_write_protect(false);
    for (int p = 0; p < 2; p++) if (s_card_path[p][0]) memcard_attach(p, s_card_path[p]);
    g_pad_buttons[0] = g_pad_buttons[1] = 0xffff;   /* released; the frontend's own input takes over */
    o.on = false;
    s_started = false;
    menu_netplay_progress(NULL);
    if (msg && *msg) frontend_alert(msg);
}

static void hires_for_resim(u64 f) {
    /* hi-res is presentation only: draw just the last few re-simulated frames */
    bool want_off = s_resim_until && f + 3 < s_resim_until;
    if (want_off && !s_hires_off) { s_hires_saved = g_hires; g_hires = NULL; s_hires_off = true; }
    else if (!want_off && s_hires_off) { g_hires = s_hires_saved; s_hires_off = false; }
}

void netplay_vblank_begin(u64 f) {
    if (portmap_tick()) lobby_portmap_changed();
    if (!o.on) {   /* --net-hash-log without netplay: the same log for an offline run (tests) */
        lobby_poll();
        if (!o.hash_log) return;
        if (!s_hash_log && !(s_hash_log = fopen(o.hash_log, "w"))) { o.hash_log = NULL; return; }
        if (o.hash_every > 0 && f % (u64)o.hash_every == 0)
            fprintf(s_hash_log, "%llu %016llx %016llx\n", (unsigned long long)f, (unsigned long long)gameplay_hash(),
                    (unsigned long long)ss_hash_all());
        else fprintf(s_hash_log, "%llu %016llx\n", (unsigned long long)f, (unsigned long long)gameplay_hash());
        return;
    }
    if (!s_started && !start()) return;
    bool resim = s_resim_until && f < s_resim_until;
    if (s_resim_until && f >= s_resim_until) {          /* caught up */
        u64 dt = ss_now_ns() - s_resim_from;
        st.resim_ns += dt;
        if (dt > st.max_resim_ns) st.max_resim_ns = dt;
        smooth_rollback(s_rb_depth, dt, s_rb_shown);
        s_resim_until = 0;
        ss_set_resim(false);
        hires_for_resim(f);
    }
    if (!resim) {
        s_cur_frame = f;
        /* time sync: a peer that runs ahead of the other (it started first, its clock is faster, or
         * the other one lost time on a slow frame) predicts more and makes the other side roll back
         * more. Both sides measure their frame advantage over the last frame reported by the peer
         * (latency cancels out in the difference). The one ahead by >= 1 frame on average slows down
         * by that much, spread over the next frames (1 ms per frame, 2 ms when far ahead: ~6-12%
         * slower for a moment) instead of stopping, so nothing visibly halts; then it waits ~0.75 s
         * for both averages to show the result before deciding again. */
        if (s_peer_frame) {
            double adv = (double)f - (double)s_peer_frame;
            s_adv_avg += (adv - s_adv_avg) / 32;
            double d = (s_adv_avg - s_peer_adv) / 2;
            if (o.sync_hard) {             /* the old way, for comparisons: stop for the whole amount */
                if (f % 60 == 0 && d >= 1.0) {
                    int k = d > 8 ? 8 : (int)d;
                    st.sync_waits++; st.sync_frames += (u64)k;
                    smooth_sync((u64)k * 16666667ull);
                    if (o.pace_hz > 0) s_t0_ns += (u64)(k * 1e9 / o.pace_hz);
                    else if (!o.unpaced) net_sleep_us((u64)k * 16667);
                }
            } else if (s_sync_debt_ns <= 0 && f >= s_sync_next && d >= 1.0) {
                double k = d > 8 ? 8 : d;
                st.sync_waits++; st.sync_frames += (u64)k;
                s_sync_debt_ns = k * 1e9 / 60;
            }
            if (s_sync_debt_ns > 0) {
                double pay = s_sync_debt_ns > 2.5e9 / 60 ? 2e6 : 1e6;
                if (pay > s_sync_debt_ns) pay = s_sync_debt_ns;
                s_sync_debt_ns -= pay;
                if (s_sync_debt_ns <= 0) s_sync_next = f + 45;
                smooth_sync((u64)pay);
                if (o.pace_hz > 0) s_t0_ns += (u64)pay;
                else if (!o.unpaced) s_fe_extra_ns += (u64)pay;
            }
        }
        /* pacing (headless; windowed runs are paced by the frontend) */
        if (o.pace_hz > 0) {
            u64 due = s_t0_ns + (u64)((double)f * 1e9 / o.pace_hz);
            u64 now = ss_now_ns();
            if (due > now && due - now < 1000000000ull) net_sleep_us((due - now) / 1000);
        }
        receive();
        if (s_end_pending) { s_end_pending = false; session_end(s_end_msg); return; }
        /* stall while the remote input of frame f is unknown and may not be predicted */
        bool round = in_round(f);
        u64 t0 = 0;
        for (;;) {
            if (remote_known(f) || s_peer_left) break;
            bool ahead_ok = f <= s_remote_conf + (u64)o.window;
            /* restorable points alternate with other contexts in fights: one at this or the
             * previous vblank is enough (a rollback found at a non-matching vblank waits one) */
            bool now_ok = prediction_ok(s_remote_conf + 1);
            if (ahead_ok && round && (now_ok || s_pred_prev)) { s_pred_prev = now_ok; break; }
            if (!t0) { t0 = ss_now_ns(); st.stalls++; }
            if ((double)(ss_now_ns() - t0) / 1e9 > o.timeout_s) {
                end_later("Lost the connection to your opponent. Continuing offline.");
                break;
            }
            frontend_pump_events();
            static u64 last_send;
            if (ss_now_ns() - last_send > 4000000ull) {    /* keep acks / resends flowing (lost packets) */
                send_inputs();
                last_send = ss_now_ns();
            }
            net_sleep_us(500);
            receive();
        }
        if (t0) { st.stall_ns += ss_now_ns() - t0; smooth_stall(ss_now_ns() - t0); }
        if (s_end_pending) { s_end_pending = false; session_end(s_end_msg); return; }
        if (remote_known(f)) s_pred_prev = prediction_ok(s_remote_conf + 1);
        /* rollback */
        if (s_first_bad) {
            Snapshot *sn = restorable(s_first_bad);
            /* the newest snapshot with this vblank's host context may lie far before the bad frame
             * (rare mid-update contexts); the VSync-wait context of the next vblank or two usually
             * has one within 2 frames, which re-simulates much less */
            static int defer_n;
            if (sn && s_first_bad - sn->frame > 2 && defer_n < 2) { sn = NULL; defer_n++; }
            else if (sn) defer_n = 0;
            if (sn && ss_safe_point()) {
                u64 depth = f - sn->frame;
                if (getenv("NETPLAY_DEBUG") && depth > (u64)o.window + 2)
                    fprintf(stderr, "netplay-debug: deep rollback at %llu: first bad %llu, snapshot %llu, remote conf %llu\n",
                            (unsigned long long)f, (unsigned long long)s_first_bad, (unsigned long long)sn->frame,
                            (unsigned long long)s_remote_conf);
                if (depth > st.max_depth) st.max_depth = depth;
                st.rollbacks++;
                s_rb_depth = depth;
                s_rb_shown = f - s_first_bad;   /* frames f-1 .. first_bad + 1 were presented wrong */
                s_first_bad = 0;
                s_resim_from = ss_now_ns();
                s_resim_until = f;
                if (!ss_restore_ex(sn, false)) fatal("restore failed");
                ss_set_resim(true);
                f = sn->frame;                             /* we are now at the start of frame f */
                resim = true;
            } else {
                st.deferred++;
                bool any = false;
                for (int i = 0; i < SNAPS; i++) if (s_snap[i].valid && s_snap[i].frame <= s_first_bad) any = true;
                if (!any || f > s_first_bad + (u64)o.window * 3) {
                    st.impossible++;
                    fprintf(stderr, "netplay: rollback impossible for frame %llu at frame %llu (no snapshot with the "
                            "current host context); continuing\n", (unsigned long long)s_first_bad, (unsigned long long)f);
                    s_first_bad = 0;
                }
            }
        }
    }
    if (resim) { st.resim_frames++; hires_for_resim(f); }
    else st.frames++;
    if (o.inject_frame && f == o.inject_frame) {   /* test fault, applied on every pass through f */
        mem_w16(0x800c9938u + 0xc, (u16)(mem_r16(0x800c9938u + 0xc) - 1));
        if (!resim) fprintf(stderr, "netplay: injected a desync at frame %llu (p1.hp - 1)\n", (unsigned long long)f);
    }
    /* snapshot + hashes for the start of frame f */
    if (ss_safe_point()) {
        Snapshot *sn = snap_for(f);
        sn->valid = false;
        ss_save(sn);
    }
    u32 s = slot(f);
    s_gh[s] = gameplay_hash();
    replay_fields_now(s_fval[s], s_fld[s]);
    s_fh[s] = (o.hash_every > 0 && f % (u64)o.hash_every == 0) ? ss_hash_all() : 0;
    s_hframe[s] = f;
    if (!resim) {
        /* final: every input < f known and used (no misprediction pending) */
        u64 lim = s_remote_conf + 1;
        if (lim > f) lim = f;
        if (s_first_bad && s_first_bad < lim) lim = s_first_bad;
        emit_final(lim);
    }
}

/* Straight to VS mode's character select (P1 = host, P2 = joiner): the inputs a player would give
 * from power-on (Start skips the KOEI logo and the intro FMV into the title menu, Down Down moves its
 * cursor from 1P BATTLE to VS, Start), played on pad 1 on both peers at the same frames of the
 * session's fresh boot, whatever the players press meanwhile. Pure emulation of the real path: both
 * machines stay identical and nothing is loaded from outside the disc. The character select is up at
 * about frame 380 instead of ~1560, and its cursors start on Gradd (P1) and Celia (P2) as usual.
 * Each press is held 6 vblanks like --press. Found by stepping through dumps (analysis/NOTES.md). */
static const struct { u16 frame, mask; } k_boot_vs[] = {
    { 210, 0x0008 },   /* Start: KOEI logo -> title (the intro FMV never starts) */
    { 240, 0x0040 },   /* Down: 1P BATTLE -> STORY */
    { 270, 0x0040 },   /* Down: STORY -> VS */
    { 300, 0x0008 },   /* Start: VS -> character select (up by frame ~380) */
};
#define BOOT_VS_END 306u   /* frames 1 .. this are driven by the script */
static bool boot_vs_pads(u64 f) {
    if (f < 1 || f > BOOT_VS_END) return false;
    u16 m = 0;
    for (size_t k = 0; k < sizeof k_boot_vs / sizeof k_boot_vs[0]; k++)
        if (f >= k_boot_vs[k].frame && f < k_boot_vs[k].frame + 6u) m |= k_boot_vs[k].mask;
    g_pad_buttons[0] = (u16)~m;
    g_pad_buttons[1] = 0xffff;
    if (f == BOOT_VS_END) fprintf(stderr, "netplay: boot shortcut done at frame %llu (VS character select next)\n",
                                  (unsigned long long)f);
    return true;
}

void netplay_vblank_end(u64 f) {
    if (!o.on && o.boot_to_vs_offline) { boot_vs_pads(f); return; }
    if (!o.on || !s_started) return;
    if (s_report_deferred && replay_last_frame() >= s_report_deferred) { u64 d = s_report_deferred; s_report_deferred = 0; desync_report(d); }
    bool resim = s_resim_until && f < s_resim_until;
    if (!resim) {
        /* local input sampled now is for frame f + delay */
        u16 local = (u16)~g_pad_buttons[0];
        u64 t = f + (u64)o.delay;
        s_local_in[slot(t)] = local;
        if (t > s_local_through) s_local_through = t;
    }
    u32 s = slot(f);
    u16 rem;
    if (remote_known(f)) { rem = s_remote_in[s]; s_used_pred[s] = false; }
    else { rem = s_last_remote; s_used_pred[s] = true; if (!resim) st.predicted++; }
    s_used_remote[s] = rem;
    if (f > s_applied || resim) s_applied = f > s_applied ? f : s_applied;
    u16 local_now = s_local_in[s];
    g_pad_buttons[s_local_idx] = (u16)~local_now;
    g_pad_buttons[s_remote_idx] = (u16)~rem;
    if (!o.full_boot) boot_vs_pads(f);
    if (!resim) { send_inputs(); status(f); }
}

/* Connection-quality readout, once a second: window title (windowed) and every 10 s on stderr. */
void frontend_set_status(const char *s);   /* main.c */
static void status(u64 f) {
    static u64 last_f, last_rb, last_pred, last_stall_ns, last_ns;
    u64 now = ss_now_ns();
    if (!last_ns) { last_ns = now; last_f = f; return; }
    if (now - last_ns < 1000000000ull) return;
    double sec = (double)(now - last_ns) / 1e9;
    char b[256];
    snprintf(b, sizeof b, "netplay %s | ping %.0f ms | delay %d | %.0f fps | rollbacks %.1f/s | predicted %.0f%% | waiting %.0f%%%s",
             o.host ? "host P1" : "P2", s_rtt, o.delay, (double)(f - last_f) / sec,
             (double)(st.rollbacks - last_rb) / sec,
             f > last_f ? 100.0 * (double)(st.predicted - last_pred) / (double)(f - last_f) : 0.0,
             100.0 * (double)(st.stall_ns - last_stall_ns) / (double)(now - last_ns),
             s_desyncs ? " | DESYNC" : s_peer_left ? " | peer left" :
             s_peer_load >= 95 ? " | Opponent's computer is running slowly" :
             smooth_load() >= 0.95 ? " | This computer is running slowly" : "");
    frontend_set_status(b);
    static int n;
    if (++n % 10 == 0) fprintf(stderr, "%s\n", b);
    last_ns = now; last_f = f; last_rb = st.rollbacks; last_pred = st.predicted; last_stall_ns = st.stall_ns;
}

/* ---- desync handling (milestone 4) --------------------------------------------------------- */

/* A final frame whose hashes differ from the peer's: the emulated machines are no longer the same
 * (non-determinism, a bug, or a different build that passed the handshake). Logged, counted (exit
 * status 7), and the first one writes a report next to an input recording of this peer's timeline:
 *   desync-FRAME-host.txt   frame, both hashes, the field-by-field comparison, link statistics
 *   desync-FRAME-host.dtr   both pads for frames 1..FRAME + this peer's per-frame field hashes
 * `destrega --headless --replay desync-FRAME-host.dtr` re-runs the inputs offline: on the peer
 * that went wrong it reports the first frame where its own timeline differs from a clean run. */
static void desync_report(u64 frame) {
    /* the hash check can run before this vblank's replay_vblank_begin recorded `frame` (depends on when
     * the peer's packet arrived): wait one vblank so the report's replay file includes it */
    if (frame > replay_last_frame()) { if (!s_report_deferred) s_report_deferred = frame; return; }
    s_desyncs++;
    u32 s = slot(frame);
    if (s_desyncs <= 3)
        fprintf(stderr, "netplay: DESYNC at frame %llu: gameplay hash %016llx here, %016llx on the peer; "
                "state hash %016llx / %016llx\n", (unsigned long long)frame, (unsigned long long)s_gh[s],
                (unsigned long long)s_peer_gh[s], (unsigned long long)s_fh[s], (unsigned long long)s_peer_fh[s]);
    if (s_first_desync) return;
    s_first_desync = frame;
    const char *dir = o.desync_dir ? o.desync_dir : ".";
    const char *who = o.host ? "host" : "join";
    char txt[1024], dtr[1024];
    snprintf(txt, sizeof txt, "%s/desync-%llu-%s.txt", dir, (unsigned long long)frame, who);
    snprintf(dtr, sizeof dtr, "%s/desync-%llu-%s.dtr", dir, (unsigned long long)frame, who);
    bool rec = replay_write(dtr, frame);
    FILE *f = fopen(txt, "w");
    if (!f) { fprintf(stderr, "netplay: cannot write %s\n", txt); return; }
    fprintf(f, "Destrega netplay desync report (%s, player %d)\n\n", who, s_local_idx + 1);
    fprintf(f, "first differing final frame: %llu (state at the start of the frame; every input before it was\n"
               "confirmed on both peers)\n", (unsigned long long)frame);
    fprintf(f, "gameplay hash: %016llx here, %016llx on the peer\n", (unsigned long long)s_gh[s], (unsigned long long)s_peer_gh[s]);
    if (s_fh[s] || s_peer_fh[s])
        fprintf(f, "state hash:    %016llx here, %016llx on the peer\n", (unsigned long long)s_fh[s], (unsigned long long)s_peer_fh[s]);
    fprintf(f, "\nfield        here (hash value)        peer (hash)\n");
    int nd = 0;
    for (int k = 0; k < NF && k < replay_field_count(); k++) {
        char v[24];
        replay_field_str(k, s_fval[s][k], v, sizeof v);
        bool d = s_fld[s][k] != s_peer_fld[s][k];
        nd += d;
        fprintf(f, "%-10s   %04x %-12s       %04x  %s\n", replay_field_name(k), s_fld[s][k], v, s_peer_fld[s][k],
                d ? "DIFFERS" : "same");
    }
    if (!nd) fprintf(f, "(no gameplay field differs: the difference is elsewhere in the emulated state)\n");
    fprintf(f, "\nlast agreeing check: ");
    u64 ok = 0;
    for (u64 k = frame - 1; k + RING > frame && k; k--) {
        u32 t = slot(k);
        if (s_peer_hash_frame[t] == k && s_hframe[t] == k && s_peer_gh[t] == s_gh[t]) { ok = k; break; }
    }
    if (ok) fprintf(f, "frame %llu\n", (unsigned long long)ok); else fprintf(f, "none in the last %d frames\n", RING);
    fprintf(f, "\nlink: %llu frames, %llu rollbacks, %llu mispredicted inputs, %llu impossible rollbacks, delay %d, window %d\n",
            (unsigned long long)st.frames, (unsigned long long)st.rollbacks, (unsigned long long)st.mispredicted,
            (unsigned long long)st.impossible, o.delay, o.window);
    if (rec) fprintf(f, "\ninput log (replay file): %s\n  destrega --headless --replay %s\n", dtr, dtr);
    fclose(f);
    fprintf(stderr, "netplay: desync report written to %s%s%s\n", txt, rec ? " and " : "", rec ? dtr : "");
    if (o.desync_stop) { fprintf(stderr, "netplay: stopping (--net-desync-stop)\n"); frontend_set_max_frames(s_applied + 1); }
    if (o.from_menu) {
        char m[512];
        snprintf(m, sizeof m, "Netplay stopped: the two games went out of sync at frame %llu. Continuing offline. "
                 "A report was saved as %s.", (unsigned long long)frame, txt);
        end_later(m);
    }
}


/* ---- menu sessions: Netplay > Host Game / Join Game ------------------------------------------
 * The game is already running when a player picks Host or Join, and the two machines are in
 * different states. Starting from a common point is done the simplest correct way: a fresh boot
 * on both sides. A short "lobby" exchange runs between frames while the game keeps playing
 * offline (the host waits for a hello, the joiner sends one every 200 ms); it checks the same
 * identity as the session handshake and agrees on the input delay (the larger of the two). When
 * both agree, each process re-executes itself (main.c frontend_restart_with, the same clean
 * restart as after loading a game image) with --host / --join, the agreed --net-delay and
 * --net-from-menu, and the normal session handshake runs at frame 1 of the new boot. Sending a
 * running state instead would need the host's host-side call chain on the joiner (savestate.c),
 * which another process cannot reproduce at an arbitrary moment.
 * --net-from-menu turns every failure into an alert and offline play instead of an exit. */
enum { LOBBY_OFF, LOBBY_HOSTING, LOBBY_JOINING };
static int s_lobby;
static NetSock *s_lsock;
static int s_lport, s_ldelay;
static char s_laddr[256];
static u64 s_lt0, s_llast;

bool netplay_busy(void) { return o.on || s_lobby != LOBBY_OFF; }

static void lobby_stop(void) {
    portmap_stop();   /* nothing after portmap_keep_for_restart */
    if (s_lsock) net_close(s_lsock);
    s_lsock = NULL;
    s_lobby = LOBBY_OFF;
    menu_netplay_progress(NULL);
}

static void lobby_restart(bool host, int delay) {
    static char a1[300], d[16];
    snprintf(d, sizeof d, "%d", delay);
    if (host) snprintf(a1, sizeof a1, "%d", s_lport);
    else snprintf(a1, sizeof a1, "%s:%d", s_laddr, s_lport);
    const char *args[] = { host ? "--host" : "--join", a1, "--net-delay", d, "--net-from-menu" };
    fprintf(stderr, "netplay: opponent found; restarting both games from a fresh boot (%s %s, input delay %d)\n",
            args[0], a1, delay);
    net_flush(s_lsock, 100);
    if (host) portmap_keep_for_restart();   /* the session keeps the router's port mapping */
    lobby_stop();
    frontend_restart_with(args, 5);
}

void netplay_menu_host(int port, int delay) {
    if (netplay_busy()) { frontend_alert("A netplay session is already running. Disconnect first."); return; }
    if (port <= 0 || port > 65535) port = 7000;
    if (delay < 1 || delay > 30) delay = 2;
    s_lsock = net_open(NULL, port, port, NULL);
    if (!s_lsock) {
        char m[200];
        snprintf(m, sizeof m, "Could not host on port %d: it may be in use by another program. Try another port.", port);
        frontend_alert(m);
        return;
    }
    s_lobby = LOBBY_HOSTING; s_lport = port; s_ldelay = delay; s_lt0 = ss_now_ns();
    char ip[256];
    net_local_addresses(ip, sizeof ip);
    fprintf(stderr, "netplay: hosting a game on port %d (addresses: %s), waiting for an opponent\n", port, *ip ? ip : "none");
    portmap_start(port);
    lobby_portmap_changed();
}

/* Host's waiting panel: the home-network address, and what the router said (portmap.c). */
static void lobby_portmap_changed(void) {
    if (s_lobby != LOBBY_HOSTING) return;
    char ip[256], m[800];
    net_local_addresses(ip, sizeof ip);
    const char *pm = portmap_text();
    snprintf(m, sizeof m, "Waiting for an opponent on port %d...\nOn your home network, your opponent joins with: %s:%d%s%s",
             s_lport, *ip ? ip : "(your IP address)", s_lport, *pm ? "\n" : "", pm);
    menu_netplay_progress(m);
}

void netplay_menu_join(const char *addr, int port, int delay) {
    if (netplay_busy()) { frontend_alert("A netplay session is already running. Disconnect first."); return; }
    if (delay < 1 || delay > 30) delay = 2;
    if (port <= 0 || port > 65535 || !addr || !*addr) { frontend_alert("Enter the host's address as address:port, for example 192.168.1.20:7000."); return; }
    s_lsock = net_open(addr, port, 0, NULL);
    if (!s_lsock) {
        char m[400];
        snprintf(m, sizeof m, "Could not find \"%s\". Check the address your opponent gave you.", addr);
        frontend_alert(m);
        return;
    }
    s_lobby = LOBBY_JOINING; s_lport = port; s_ldelay = delay; s_lt0 = ss_now_ns(); s_llast = 0;
    snprintf(s_laddr, sizeof s_laddr, "%s", addr);
    char m[400];
    snprintf(m, sizeof m, "Connecting to %s:%d...", addr, port);
    fprintf(stderr, "netplay: joining %s:%d\n", addr, port);
    menu_netplay_progress(m);
}

void netplay_menu_disconnect(void) {
    if (s_lobby != LOBBY_OFF) { fprintf(stderr, "netplay: cancelled\n"); lobby_stop(); return; }
    if (o.on && s_started) session_end(NULL);
}

static void lobby_poll(void) {
    if (s_lobby == LOBBY_OFF) return;
    u8 b[1500];
    Ident me;
    int saved = o.delay;
    o.delay = s_ldelay;
    ident_local(&me);
    o.delay = saved;
    u64 now = ss_now_ns();
    int n;
    if (s_lobby == LOBBY_JOINING) {
        if ((double)(now - s_lt0) / 1e9 > o.timeout_s + 5) {   /* 20 s by default */
            char m[512];
            snprintf(m, sizeof m, "No answer from %s:%d. Check the address, that your opponent chose Host Game, and that "
                     "their network lets UDP port %d through. The game continues offline.", s_laddr, s_lport, s_lport);
            lobby_stop();
            frontend_alert(m);
            return;
        }
        if (now - s_llast > 200000000ull) { net_send(s_lsock, b, pkt_ident(b, PKT_LOBBY_HELLO, &me)); s_llast = now; }
    }
    while (s_lsock && (n = net_recv(s_lsock, b, sizeof b)) > 0) {
        if (n < 8 || get_u32(b) != NET_MAGIC) continue;
        if (s_lobby == LOBBY_HOSTING && b[4] == PKT_LOBBY_HELLO) {
            const char *e = ident_check_ex(b, n, PKT_LOBBY_HELLO, false);
            if (e && !*e) continue;
            if (e) {   /* tell the joiner why, keep waiting */
                u8 r[300];
                put_u32(r, NET_MAGIC); r[4] = PKT_LOBBY_REFUSE; r[5] = NET_VERSION;
                int k = snprintf((char *)r + 8, sizeof r - 8, "%s", e);
                for (int i = 0; i < 3; i++) net_send(s_lsock, r, 8 + k + 1);
                fprintf(stderr, "netplay: refused a player: %s\n", e);
                continue;
            }
            int pd = (int)get_u32(b + 20), d = pd > s_ldelay ? pd : s_ldelay;
            net_lock_peer(s_lsock);
            int saved2 = o.delay;
            o.delay = d;
            Ident w;
            ident_local(&w);
            o.delay = saved2;
            int m = pkt_ident(b, PKT_LOBBY_WELCOME, &w);
            for (int i = 0; i < 5; i++) net_send(s_lsock, b, m);
            fprintf(stderr, "netplay: %s wants to play\n", net_peer_name(s_lsock));
            lobby_restart(true, d);
            return;
        }
        if (s_lobby == LOBBY_JOINING && b[4] == PKT_LOBBY_REFUSE && n > 8) {
            char m[512];
            b[n < (int)sizeof b ? n : (int)sizeof b - 1] = 0;
            snprintf(m, sizeof m, "The host could not accept you: %s. The game continues offline.", (char *)b + 8);
            lobby_stop();
            frontend_alert(m);
            return;
        }
        if (s_lobby == LOBBY_JOINING && b[4] == PKT_LOBBY_WELCOME) {
            const char *e = ident_check_ex(b, n, PKT_LOBBY_WELCOME, false);
            if (e && !*e) continue;
            if (e) {
                char m[512];
                snprintf(m, sizeof m, "Could not join: %s. The game continues offline.", e);
                lobby_stop();
                frontend_alert(m);
                return;
            }
            lobby_restart(false, (int)get_u32(b + 20));
            return;
        }
    }
}

/* One line for the Netplay menu's status item. */
const char *netplay_status_text(void) {
    static char t[512];
    if (s_lobby == LOBBY_HOSTING) {
        const char *pm = portmap_status_short();
        snprintf(t, sizeof t, "Waiting for an opponent on port %d%s%s%s", s_lport, *pm ? " (" : "", pm, *pm ? ")" : "");
    }
    else if (s_lobby == LOBBY_JOINING) snprintf(t, sizeof t, "Connecting to %s:%d", s_laddr, s_lport);
    else if (o.on && s_first_desync) snprintf(t, sizeof t, "Desync at frame %llu", (unsigned long long)s_first_desync);
    else if (o.on && s_started && s_sock) snprintf(t, sizeof t, "Connected to %s, ping %.0f ms", net_peer_name(s_sock), s_rtt);
    else if (o.on) snprintf(t, sizeof t, "Connecting...");
    else snprintf(t, sizeof t, "Not connected");
    return t;
}

/* ---- options / report ---------------------------------------------------------------------- */

static bool parse_sim(const char *s, NetSim *n) {
    /* LAT[:JITTER[:LOSS[:REORDER[:SEED]]]]  (ms, ms, %, %) */
    double v[5] = {0, 0, 0, 0, 1};
    int k = 0;
    while (*s && k < 5) { char *e; v[k++] = strtod(s, &e); if (e == s) return false; s = e; if (*s == ':') s++; }
    n->latency_ms = v[0]; n->jitter_ms = v[1]; n->loss_pct = v[2]; n->reorder_pct = v[3]; n->seed = (u32)v[4];
    return true;
}

int netplay_parse_arg(int argc, char **argv, int i) {
    const char *a = argv[i];
    bool has = i + 1 < argc;
    static bool scanned;
    if (!scanned) {   /* headless runs are unpaced unless --net-pace is given */
        scanned = true;
        for (int k = 1; k < argc; k++) if (!strcmp(argv[k], "--headless")) o.unpaced = true;
    }
    if (!strcmp(a, "--host") && has) { o.on = o.host = true; o.port = atoi(argv[i + 1]); replay_record_in_memory(); return 2; }
    if (!strcmp(a, "--join") && has) {
        const char *v = argv[i + 1], *c = strrchr(v, ':');
        if (!c) { fprintf(stderr, "--join HOST:PORT\n"); exit(2); }
        snprintf(o.addr, sizeof o.addr, "%.*s", (int)(c - v), v);
        o.port = atoi(c + 1);
        o.on = true; o.host = false;
        replay_record_in_memory();   /* from frame 1 on: desync reports are replay files */
        return 2;
    }
    if (!strcmp(a, "--net-port") && has) { o.local_port = atoi(argv[i + 1]); return 2; }
    if (!strcmp(a, "--net-delay") && has) { o.delay = atoi(argv[i + 1]); if (o.delay < 1 || o.delay > 30) o.delay = 2; return 2; }
    if (!strcmp(a, "--net-window") && has) { o.window = atoi(argv[i + 1]); if (o.window < 0 || o.window > 60) o.window = 8; return 2; }
    if (!strcmp(a, "--net-pace") && has) { o.pace_hz = atof(argv[i + 1]); return 2; }
    if (!strcmp(a, "--net-sim") && has) {
        if (!parse_sim(argv[i + 1], &o.sim)) { fprintf(stderr, "--net-sim LAT[:JITTER[:LOSS%%[:REORDER%%[:SEED]]]]\n"); exit(2); }
        return 2;
    }
    if (!strcmp(a, "--net-hash-log") && has) { o.hash_log = argv[i + 1]; return 2; }
    if (!strcmp(a, "--net-hash-every") && has) { o.hash_every = atoi(argv[i + 1]); return 2; }
    if (!strcmp(a, "--net-desync-dir") && has) { o.desync_dir = argv[i + 1]; return 2; }
    { int k = smooth_parse_arg(argc, argv, i); if (k) return k; }
    { int k = portmap_parse_arg(argc, argv, i); if (k) return k; }
    if (!strcmp(a, "--net-from-menu")) { o.from_menu = true; return 1; }
    if (!strcmp(a, "--net-lockstep")) { o.lockstep = true; return 1; }
    if (!strcmp(a, "--net-full-boot")) { o.full_boot = true; return 1; }
    if (!strcmp(a, "--net-sync-hard")) { o.sync_hard = true; return 1; }
    if (!strcmp(a, "--boot-to-vs")) { o.boot_to_vs_offline = true; return 1; }
    if (!strcmp(a, "--net-desync-stop")) { o.desync_stop = true; return 1; }
    if (!strcmp(a, "--net-inject-desync") && has) { o.inject_frame = strtoull(argv[i + 1], NULL, 0); return 2; }
    if (!strcmp(a, "--net-test-ident") && has) {   /* build=HEX or source=HEX */
        const char *v = argv[i + 1];
        if (!strncmp(v, "build=", 6)) o.fake_build = (u32)strtoul(v + 6, NULL, 16);
        else if (!strncmp(v, "source=", 7)) o.fake_source = (u32)strtoul(v + 7, NULL, 16);
        return 2;
    }
    if (!strcmp(a, "--net-timeout") && has) { o.timeout_s = atof(argv[i + 1]); return 2; }
    return 0;
}

void netplay_usage(void) {
    fprintf(stderr,
        "  --host PORT           rollback netplay: host (player 1) on UDP PORT\n"
        "  --join HOST:PORT      rollback netplay: join a host (player 2)\n"
        "  --net-delay N         input delay in vblanks, 1..30 (default 2)\n"
        "  --net-window N        max rollback / prediction in vblanks (default 8 = 4 game updates)\n"
        "  --net-sim L:J:LOSS:RE[:SEED]  simulate latency/jitter (ms), loss and reordering (%%) on sends\n"
        "  --net-pace HZ         headless: pace frames at HZ (netplay tests)\n"
        "  --net-hash-log FILE   per final frame: gameplay hash (+ state hash every --net-hash-every N)\n"
        "  --net-desync-dir DIR  where a desync report + replay file goes (default: current directory)\n"
        "  --net-lockstep        never predict remote input (no rollback; wait for every input)\n"
        "  --net-desync-stop     stop at the first desync (default: report it and keep playing)\n"
        "  --net-inject-desync F test: corrupt p1.hp at frame F on this peer\n"
        "  --net-test-ident build=HEX|source=HEX  test: claim other game code / emulation sources\n");
    portmap_usage();
}

/* windowed pacing (main.c): extra time to add to this frame (gradual time sync) */
u64 netplay_pace_extra_ns(void) {
    u64 x = s_fe_extra_ns;
    s_fe_extra_ns = 0;
    return x;
}

void netplay_finish(void) {
    portmap_finish();   /* delete the router's port mapping (bounded wait) */
    smooth_report();
    if (!o.on && s_hash_log) { fclose(s_hash_log); s_hash_log = NULL; }
    if (!o.on || !s_started) return;
    if (s_sock) {
        u8 b[8]; put_u32(b, NET_MAGIC); b[4] = PKT_BYE; b[5] = 0; b[6] = b[7] = 0;
        for (int k = 0; k < 3; k++) net_send(s_sock, b, 8);
        net_flush(s_sock, 200);
    }
    if (s_hash_log) fclose(s_hash_log);
    print_stats();
}

static void print_stats(void) {
    double n = st.rollbacks ? (double)st.rollbacks : 1;
    fprintf(stderr, "netplay: %llu frames, %llu predicted remote inputs, %llu mispredicted; %llu rollbacks "
            "(max depth %llu vblanks, %llu deferred one vblank, %llu impossible), %llu frames re-simulated, "
            "re-sim avg %.2f ms max %.2f ms per rollback; %llu stalls (%.0f ms); %llu time-sync waits (%llu frames); %llu/%llu packets out/in; "
            "%llu final frames, %llu hashes checked against the peer, %llu desyncs\n",
            (unsigned long long)st.frames, (unsigned long long)st.predicted, (unsigned long long)st.mispredicted,
            (unsigned long long)st.rollbacks, (unsigned long long)st.max_depth, (unsigned long long)st.deferred,
            (unsigned long long)st.impossible, (unsigned long long)st.resim_frames, st.resim_ns / n / 1e6,
            st.max_resim_ns / 1e6, (unsigned long long)st.stalls, st.stall_ns / 1e6,
            (unsigned long long)st.sync_waits, (unsigned long long)st.sync_frames,
            (unsigned long long)st.pkts_out, (unsigned long long)st.pkts_in, (unsigned long long)s_final_through,
            (unsigned long long)s_hash_checked, (unsigned long long)s_desyncs);
    if (st.resim_frames)
        fprintf(stderr, "netplay: re-simulation cost %.3f ms per re-simulated vblank (incl. restore)\n",
                st.resim_ns / 1e6 / (double)st.resim_frames);
}

int netplay_failed(void) { return s_desyncs != 0 && !o.from_menu; }
