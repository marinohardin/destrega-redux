/* Deterministic input recording / replay = gameplay regression tests.
 *
 *   --record FILE   write both pads' inputs per frame, the build / disc identity, the gameplay
 *                   fields at every frame and the complete state hash every 60 frames.
 *   --replay FILE   drive both pads from FILE (any other input is overridden), compare every
 *                   frame's gameplay fields and the periodic state hashes, report the first
 *                   divergence as a field-by-field diff and exit non-zero.
 *
 * The file holds inputs and hashes only, never game data: each gameplay field is stored as a
 * 16-bit hash of its value (salted with the field), so a divergence still names the frame, the
 * fighter and the field; the values themselves come from the live run (and --gameplay-trace).
 *
 * Frame numbers are the frontend's (the same numbers --press uses). Inputs are the pad state
 * after the frontend's per-frame work of frame F (what the game reads from then on); gameplay
 * fields and state hashes are sampled at the start of frame F, before its inputs (savestate.c
 * frame boundary), so they depend only on the inputs of frames < F.
 *
 * File format (text, one record per line, sorted by frame; '#' starts a comment):
 *   destrega-replay 1
 *   disc CRC32 SECTORS        SLUS_007.32 CRC-32 and the image's sector count
 *   build HASH                hash of the recompiled-function table (code identity)
 *   frames N                  last frame
 *   i F P1 P2                 pad masks (hex, active high) from frame F on, until the next i line
 *   g F name=HASH ...         16-bit hashes of the gameplay fields that changed at the start of F
 *   s F HASH                  complete emulated-state hash at the start of frame F
 * Files may be gzip-compressed (read through zlib).
 */
#include "savestate.h"
#include "disc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* ---- gameplay fields (analysis/symbols.txt, "Fight state" in NOTES.md) ------------------ */

#define FIGHTER_P1   0x800c9938u   /* fighter structs, 0x1dc bytes each */
#define FIGHTER_P2   0x800c9b14u
#define FIGHTER_SIZE 0x1dcu

enum { G_P1_HP, G_P1_GAUGE, G_P1_X, G_P1_Y, G_P1_Z, G_P2_HP, G_P2_GAUGE, G_P2_X, G_P2_Y, G_P2_Z,
       G_TIME, G_WINS1, G_WINS2, G_RNG, G_P1_STRUCT, G_P2_STRUCT, G_N };
static const char *const s_gname[G_N] = {
    "p1.hp", "p1.gauge", "p1.x", "p1.y", "p1.z", "p2.hp", "p2.gauge", "p2.x", "p2.y", "p2.z",
    "time", "p1.wins", "p2.wins", "rng", "p1.struct", "p2.struct",
};
typedef struct { u32 v[G_N]; } Gp;

static u32 fnv_range(u32 a, u32 n) {
    u32 h = 0x811c9dc5u;
    for (u32 i = 0; i < n; i++) h = (h ^ mem_r8(a + i)) * 0x01000193u;
    return h;
}

void gameplay_sample(Gp *g) {
    const u32 f[2] = { FIGHTER_P1, FIGHTER_P2 };
    for (int i = 0; i < 2; i++) {
        u32 *o = g->v + i * 5;
        o[0] = (u32)(s32)(s16)mem_r16(f[i] + 0xc);
        o[1] = (u32)(s32)(s16)mem_r16(f[i] + 0xe);
        o[2] = mem_r32(f[i] + 0xa0); o[3] = mem_r32(f[i] + 0xa4); o[4] = mem_r32(f[i] + 0xa8);
    }
    g->v[G_TIME] = mem_r32(0x800e379c);
    g->v[G_WINS1] = mem_r32(0x800e3788);
    g->v[G_WINS2] = mem_r32(0x800e378c);
    g->v[G_RNG] = mem_r32(0x800e7560);
    g->v[G_P1_STRUCT] = fnv_range(FIGHTER_P1, FIGHTER_SIZE);
    g->v[G_P2_STRUCT] = fnv_range(FIGHTER_P2, FIGHTER_SIZE);
}

u64 gameplay_hash(void) {
    Gp g;
    gameplay_sample(&g);
    return ss_hash_bytes(0x9ae16a3b2f90404full, g.v, sizeof g.v);
}

static bool is_hex_field(int k) { return k >= G_RNG; }
static void fmt_field(char *b, size_t n, int k, u32 v) {
    if (is_hex_field(k)) snprintf(b, n, "%08x", v); else snprintf(b, n, "%d", (s32)v);
}

/* 16-bit per-field hash (what a recording stores instead of the value) */
static u32 field_hash(int k, u32 v) {
    u32 h = 0x811c9dc5u ^ (u32)k * 0x9e3779b9u;
    for (int i = 0; i < 4; i++) h = (h ^ ((v >> (8 * i)) & 0xff)) * 0x01000193u;
    return (h ^ (h >> 16)) & 0xffff;
}
static void gp_hashed(const Gp *in, Gp *out) {
    for (int k = 0; k < G_N; k++) out->v[k] = field_hash(k, in->v[k]);
}

/* ---- identity ---------------------------------------------------------------------------- */

u32 replay_build_id(void) {
    u32 h = 0x811c9dc5u;
    for (u32 i = 0; i < g_recomp_count; i++) {
        const RecompEntry *e = &g_recomp_table[i];
        u32 w[5] = { e->addr, e->overlay, e->lo, e->hi, e->hash };
        for (int k = 0; k < 5; k++) h = (h ^ w[k]) * 0x01000193u;
    }
    return h ^ g_recomp_count;
}

void replay_disc_id(u32 *crc, u32 *sectors) {
    static u32 c, s;
    static bool done;
    if (!done) {
        u32 size = 0;
        u8 *exe = disc_load_file("SLUS_007.32", &size);
        c = exe ? (u32)crc32(0, exe, size) : 0;
        free(exe);
        s = disc_sector_count();
        done = true;
    }
    *crc = c; *sectors = s;
}

/* ---- state ------------------------------------------------------------------------------- */

void frontend_set_max_frames(u64 n);   /* main.c */

static const char *s_rec_path, *s_play_path;
static bool s_rec_mem;   /* netplay: record in memory for desync reports (replay_write) */
static u32 *s_in;        /* per frame: p1 | p2 << 16 (active high) */
static Gp *s_gp;         /* per frame gameplay fields */
static u64 *s_sh;        /* per frame complete state hash (0 = none) */
static bool *s_have_gp;
static u64 s_cap, s_last;
static int s_fail;
static bool s_play_done;
static u64 s_frames_cmp, s_sh_cmp, s_sh_bad;
static u64 s_state_every = 60;
static bool s_strict;
static u32 s_ok_val[G_N];      /* live values when every field last matched (divergence report) */
static u64 s_ok_frame[G_N];

static void grow(u64 f) {
    if (f < s_cap) return;
    u64 c = s_cap ? s_cap : 16384;
    while (c <= f) c *= 2;
    s_in = realloc(s_in, c * sizeof *s_in);
    s_gp = realloc(s_gp, c * sizeof *s_gp);
    s_sh = realloc(s_sh, c * sizeof *s_sh);
    s_have_gp = realloc(s_have_gp, c * sizeof *s_have_gp);
    if (!s_in || !s_gp || !s_sh || !s_have_gp) abort();
    memset(s_in + s_cap, 0, (c - s_cap) * sizeof *s_in);
    memset(s_gp + s_cap, 0, (c - s_cap) * sizeof *s_gp);
    memset(s_sh + s_cap, 0, (c - s_cap) * sizeof *s_sh);
    memset(s_have_gp + s_cap, 0, (c - s_cap) * sizeof *s_have_gp);
    s_cap = c;
}

int replay_failed(void) { return s_fail; }
bool replay_active(void) { return s_play_path != NULL; }

/* ---- loading ----------------------------------------------------------------------------- */

static bool load(const char *path) {
    gzFile f = gzopen(path, "rb");
    if (!f) { fprintf(stderr, "replay: cannot open %s\n", path); return false; }
    char line[4096];
    int ver = 0;
    u32 crc = 0, sec = 0, build = 0;
    u64 frames = 0, last_in = 0;
    u32 cur_in = 0;
    Gp cur = {{0}};
    bool have_build = false;
    while (gzgets(f, line, sizeof line)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || !*p) continue;
        unsigned long long fr;
        if (!strncmp(p, "destrega-replay ", 16)) ver = atoi(p + 16);
        else if (!strncmp(p, "disc ", 5)) sscanf(p + 5, "%x %u", &crc, &sec);
        else if (!strncmp(p, "build ", 6)) { sscanf(p + 6, "%x", &build); have_build = true; }
        else if (!strncmp(p, "frames ", 7)) frames = strtoull(p + 7, NULL, 10);
        else if (p[0] == 'i' && p[1] == ' ') {
            unsigned a, b;
            if (sscanf(p + 2, "%llu %x %x", &fr, &a, &b) != 3) goto bad;
            grow(fr);
            for (u64 k = last_in; k < fr; k++) s_in[k] = cur_in;
            cur_in = (a & 0xffff) | (b & 0xffff) << 16;
            last_in = fr;
        } else if (p[0] == 'g' && p[1] == ' ') {
            char *e;
            fr = strtoull(p + 2, &e, 10);
            grow(fr);
            for (char *t = strtok(e, " \n"); t; t = strtok(NULL, " \n")) {
                char *eq = strchr(t, '=');
                if (!eq) goto bad;
                *eq = 0;
                int k = 0;
                while (k < G_N && strcmp(s_gname[k], t)) k++;
                if (k == G_N) { fprintf(stderr, "replay: unknown field %s\n", t); goto bad; }
                cur.v[k] = (u32)strtoul(eq + 1, NULL, 16) & 0xffff;
            }
            s_gp[fr] = cur;
            s_have_gp[fr] = true;
        } else if (p[0] == 's' && p[1] == ' ') {
            unsigned long long h;
            if (sscanf(p + 2, "%llu %llx", &fr, &h) != 2) goto bad;
            grow(fr);
            s_sh[fr] = h;
        } else goto bad;
    }
    gzclose(f);
    if (ver != 1 || !frames) { fprintf(stderr, "replay: %s is not a version 1 replay\n", path); return false; }
    grow(frames + 1);
    for (u64 k = last_in; k <= frames; k++) s_in[k] = cur_in;
    /* gameplay fields are deltas: fill the frames without a g line */
    Gp run = {{0}};
    for (u64 k = 0; k <= frames; k++) { if (s_have_gp[k]) run = s_gp[k]; else s_gp[k] = run; }
    s_last = frames;
    u32 mc, ms;
    replay_disc_id(&mc, &ms);
    if (crc != mc || sec != ms) {
        fprintf(stderr, "replay: %s was recorded from a different disc (CRC %08x, %u sectors; this one %08x, %u)\n",
                path, crc, sec, mc, ms);
        return false;
    }
    if (have_build && build != replay_build_id())
        fprintf(stderr, "replay: note: recorded with recompiled-code table %08x, this build has %08x\n",
                build, replay_build_id());
    fprintf(stderr, "replay: %s: %llu frames\n", path, (unsigned long long)frames);
    frontend_set_max_frames(frames);
    return true;
bad:
    fprintf(stderr, "replay: %s: bad line: %s", path, line);
    gzclose(f);
    return false;
}

/* ---- hooks (savestate.c) ----------------------------------------------------------------- */

static bool s_loaded;
void replay_vblank_begin(u64 frame) {
    if (s_play_path && !s_loaded) {        /* the disc is open by the first vblank */
        s_loaded = true;
        if (!load(s_play_path)) { s_fail = 1; s_play_done = true; frontend_set_max_frames(frame); return; }
    }
    if (s_rec_path || s_rec_mem) {
        grow(frame);
        Gp g;
        gameplay_sample(&g);
        gp_hashed(&g, &s_gp[frame]);
        s_have_gp[frame] = true;
        s_sh[frame] = frame % s_state_every == 0 ? ss_hash_all() : 0;
        if (frame > s_last) s_last = frame;
    }
    if (s_play_path && !s_play_done && frame <= s_last) {
        Gp g, gh;
        gameplay_sample(&g);
        gp_hashed(&g, &gh);
        s_frames_cmp++;
        int nd = 0;
        for (int k = 0; k < G_N; k++) nd += gh.v[k] != s_gp[frame].v[k];
        if (!nd) { for (int k = 0; k < G_N; k++) { s_ok_val[k] = g.v[k]; s_ok_frame[k] = frame; } }
        if (nd) {
            fprintf(stderr, "replay: DIVERGED at frame %llu: %d gameplay field(s) differ at the start of the frame "
                    "(state at frame %llu was identical):\n", (unsigned long long)frame, nd,
                    (unsigned long long)(frame > 1 ? frame - 1 : 0));
            for (int k = 0; k < G_N; k++) {
                char a[16], b[16];
                fmt_field(b, sizeof b, k, g.v[k]);
                fmt_field(a, sizeof a, k, s_ok_val[k]);
                fprintf(stderr, "    %-10s %s  now %-10s (value when it last matched, frame %llu: %s)\n", s_gname[k],
                        gh.v[k] != s_gp[frame].v[k] ? "DIFFERS" : "same   ", b, (unsigned long long)s_ok_frame[k], a);
            }
            fprintf(stderr, "    (pN.struct = FNV-1a of the whole 0x1dc-byte fighter struct at 800c9938 / 800c9b14; "
                    "expected values: replay with the recording's build and --gameplay-trace)\n");
            s_fail = 1;
            s_play_done = true;
            frontend_set_max_frames(frame);
            return;
        }
        if (s_sh[frame]) {
            s_sh_cmp++;
            u64 h = ss_hash_all();
            if (h != s_sh[frame]) {
                if (s_sh_bad++ < 3)
                    fprintf(stderr, "replay: %s: complete state hash differs at frame %llu (gameplay fields still equal)\n",
                            s_strict ? "DIVERGED" : "warning", (unsigned long long)frame);
                if (s_strict) { s_fail = 1; s_play_done = true; frontend_set_max_frames(frame); }
            }
        }
    }
}

void replay_vblank_end(u64 frame) {
    if (s_play_path) {
        u32 v = frame <= s_last ? s_in[frame] : 0;
        g_pad_buttons[0] = (u16)~(v & 0xffff);
        g_pad_buttons[1] = (u16)~(v >> 16);
    }
    if (s_rec_path || s_rec_mem) {
        grow(frame);
        s_in[frame] = (u16)~g_pad_buttons[0] | (u32)(u16)~g_pad_buttons[1] << 16;
    }
}

/* ---- writing ----------------------------------------------------------------------------- */

/* Write the recording of frames 1..last (the current timeline: a rollback overwrites the frames
 * it re-simulates) to path. */
bool replay_write(const char *path, u64 last) {
    if (!s_rec_path && !s_rec_mem) return false;
    u64 keep = s_last;
    if (last && last < s_last) s_last = last;
    FILE *f = fopen(path, "w");
    if (!f) { s_last = keep; return false; }
    u32 crc, sec;
    replay_disc_id(&crc, &sec);
    fprintf(f, "destrega-replay 1\n");
    fprintf(f, "# Destrega input recording: pads per frame + hashes of the gameplay fields / emulated state.\n");
    fprintf(f, "# Replay: destrega --headless --replay FILE   (format: runtime/replay.c)\n");
    fprintf(f, "disc %08x %u\nbuild %08x\nframes %llu\n", crc, sec, replay_build_id(), (unsigned long long)s_last);
    u32 prev_in = 0xffffffffu;
    Gp prev;
    memset(&prev, 0, sizeof prev);
    for (u64 fr = 0; fr <= s_last; fr++) {
        if (s_have_gp[fr]) {
            char line[1024];
            int n = snprintf(line, sizeof line, "g %llu", (unsigned long long)fr);
            int changed = 0;
            for (int k = 0; k < G_N; k++) {
                if (fr > 1 && s_gp[fr].v[k] == prev.v[k]) continue;
                n += snprintf(line + n, sizeof line - (size_t)n, " %s=%04x", s_gname[k], s_gp[fr].v[k]);
                changed++;
            }
            if (changed) fprintf(f, "%s\n", line);
            prev = s_gp[fr];
        }
        if (s_sh[fr]) fprintf(f, "s %llu %016llx\n", (unsigned long long)fr, (unsigned long long)s_sh[fr]);
        if (fr && s_in[fr] != prev_in) {
            fprintf(f, "i %llu %x %x\n", (unsigned long long)fr, s_in[fr] & 0xffff, s_in[fr] >> 16);
            prev_in = s_in[fr];
        }
    }
    fclose(f);
    s_last = keep;
    return true;
}

static void write_recording(void) {
    if (!replay_write(s_rec_path, 0)) { fprintf(stderr, "replay: cannot write %s\n", s_rec_path); s_fail = 1; return; }
    fprintf(stderr, "replay: recorded %llu frames to %s\n", (unsigned long long)s_last, s_rec_path);
}

void replay_record_in_memory(void) { s_rec_mem = true; }

/* gameplay fields for netplay desync reports */
int replay_field_count(void) { return G_N; }
const char *replay_field_name(int k) { return s_gname[k]; }
void replay_fields_now(u32 *val, u16 *hash) {
    Gp g, h;
    gameplay_sample(&g);
    gp_hashed(&g, &h);
    for (int k = 0; k < G_N; k++) { val[k] = g.v[k]; hash[k] = (u16)h.v[k]; }
}
void replay_field_str(int k, u32 v, char *b, size_t n) { fmt_field(b, n, k, v); }

static bool s_finished;
void replay_finish(void) {
    if (s_finished) return;
    s_finished = true;
    if (s_rec_path) write_recording();
    if (s_play_path) {
        bool complete = s_frames_cmp >= s_last;
        if (!s_fail && !complete) { fprintf(stderr, "replay: stopped before the end of the recording\n"); s_fail = 1; }
        fprintf(stderr, "replay: %s: %llu frames compared, %llu state hashes compared (%llu differ): %s\n", s_play_path,
                (unsigned long long)s_frames_cmp, (unsigned long long)s_sh_cmp, (unsigned long long)s_sh_bad,
                s_fail ? "FAIL" : "ok");
    }
}

static void atexit_finish(void) { replay_finish(); }

int replay_parse_arg(int argc, char **argv, int i) {
    const char *a = argv[i];
    bool has = i + 1 < argc;
    if (!strcmp(a, "--record") && has) {
        s_rec_path = argv[i + 1];
        ss_set_deterministic_audio(true);
        atexit(atexit_finish);   /* windowed: Esc / closing the window exits without finish() */
        return 2;
    }
    if (!strcmp(a, "--replay") && has) {
        s_play_path = argv[i + 1];
        ss_set_deterministic_audio(true);
        return 2;
    }
    if (!strcmp(a, "--replay-strict")) { s_strict = true; return 1; }
    return 0;
}

void replay_usage(void) {
    fprintf(stderr,
        "  --record FILE         record both pads + gameplay fields / state hashes per frame (replay.c)\n"
        "  --replay FILE         play a recording back, compare every frame, report the first divergence\n"
        "  --replay-strict       a differing complete state hash (not only gameplay fields) also fails\n");
}
