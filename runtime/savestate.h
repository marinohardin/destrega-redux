/* Save states: serialize / restore every piece of emulated state (runtime/savestate.c).
 *
 * Each hardware module exposes one `<module>_state_io(StateIO *)` function that visits its
 * state variables in a fixed order. The same visitor saves (copies out), loads (copies in),
 * sizes or hashes, so the list of state lives in exactly one place per module.
 *
 * Snapshots are only taken and restored at a frame boundary: inside the VBlank scheduler
 * event (bus.c vblank_cb), before the frontend's per-frame work. Guest code is recompiled C,
 * so the host C call chain holds the guest's return addresses; a restore is only valid when
 * the host is at the same point of that chain as when the snapshot was taken. See
 * "Save states and the snapshot-safe point" in analysis/NOTES.md.
 */
#ifndef SAVESTATE_H
#define SAVESTATE_H

#include "psx.h"

typedef enum { SS_SAVE, SS_LOAD, SS_HASH } SsMode;

typedef struct StateIO {
    SsMode mode;
    u8 *buf;            /* SAVE: grows; LOAD: read from */
    size_t pos, cap;
    u64 hash;           /* SS_HASH */
} StateIO;

/* Header-only so module unit tests (which link single modules) need nothing else. */
#include <stdlib.h>
#include <string.h>
static inline u64 ss_hash_bytes(u64 h, const void *p, size_t n) {
    const u8 *b = (const u8 *)p;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        u64 w;
        memcpy(&w, b + i, 8);
        h = (h ^ w) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}
static inline void ss_io(StateIO *io, void *p, size_t n) {
    switch (io->mode) {
    case SS_SAVE:
        if (io->pos + n > io->cap) {
            size_t c = io->cap ? io->cap : 1 << 20;
            while (c < io->pos + n) c *= 2;
            io->buf = (u8 *)realloc(io->buf, c);
            if (!io->buf) abort();
            io->cap = c;
        }
        memcpy(io->buf + io->pos, p, n);
        break;
    case SS_LOAD:
        if (io->pos + n <= io->cap) memcpy(p, io->buf + io->pos, n);
        break;
    case SS_HASH:
        io->hash = ss_hash_bytes(io->hash, p, n);
        break;
    }
    io->pos += n;
}
#define SS_VAR(io, v) ss_io((io), &(v), sizeof(v))
/* Host pointers (scheduler callbacks): copied by SAVE/LOAD, hashed only as null / non-null
 * so the hash is comparable between processes (ASLR). */
static inline void ss_io_ptr(StateIO *io, void *pp, size_t n) {
    if (io->mode == SS_HASH) {
        u8 nz = 0;
        for (size_t i = 0; i < n; i++) nz |= ((u8 *)pp)[i];
        nz = nz != 0;
        io->hash = ss_hash_bytes(io->hash, &nz, 1);
        io->pos += n;
        return;
    }
    ss_io(io, pp, n);
}

/* Module visitors (each in its own module). */
void cpu_state_io(StateIO *io);       /* rt.c: g_cpu (cycles, next_event, COP0) */
void bus_state_io(StateIO *io);       /* bus.c: RAM, scratchpad, scheduler, IRQ, DMA, timers, I/O */
void bios_state_io(StateIO *io);      /* bios.c: kernel HLE (events, pad buffers, hook, card events) */
void gpu_state_io(StateIO *io);       /* gpu.c: 1x VRAM, GPUSTAT, FIFO, transfers */
void gte_state_io(StateIO *io);       /* gte.c */
void spu_state_io(StateIO *io);       /* spu.c: RAM, voices, reverb, CD audio ring */
void cdrom_state_io(StateIO *io);     /* cdrom.c: controller, sector buffers, XA decoder */
void mdec_state_io(StateIO *io);      /* mdec.c */
void sio_state_io(StateIO *io);       /* sio.c */
void memcard_state_io(StateIO *io);   /* memcard.c: images, flags, open files */

/* Visit everything (fixed order). */
void ss_visit_all(StateIO *io);

/* ---- snapshots ---------------------------------------------------------------------- */

typedef struct Snapshot {
    u8 *buf;
    size_t len, cap;
    u64 frame;          /* frontend frame number the snapshot belongs to (see savestate.c) */
    bool valid;
    /* host context the snapshot was taken in (restore requires the same) */
    u32 ctx_n;
    void *ctx[64];
    u32 depth;
    u32 callstack_hash;
} Snapshot;

/* Is the current host point a snapshot-safe point? (vblank boundary, no exception handler,
 * no C helper holding guest state in locals). */
bool ss_safe_point(void);
bool ss_save(Snapshot *s);                    /* false if not at a safe point */
/* false if s is invalid or the host context differs from the snapshot's */
bool ss_restore(const Snapshot *s);
bool ss_context_matches(const Snapshot *s);
u64 ss_hash_all(void);                        /* hash of the complete emulated state */
void ss_free(Snapshot *s);

/* Nesting counter of C helpers that keep guest state in host locals while guest code runs
 * (bios event delivery, SysEnqIntRP calls, override verification, widescreen pass). A
 * snapshot is only taken while it is 0. */
extern int g_ss_unsafe;

/* Frame-boundary hooks, called from bus.c vblank_cb around frontend_vblank(). */
void ss_vblank_begin(void);
void ss_vblank_end(void);
/* The frontend registers its frame counter (part of the state: scripted input and replays
 * are keyed by it). */
void ss_set_frame_counter(u64 *frame);

/* Emulation-only frame (rollback re-simulation): no presentation, no audio output, no
 * per-frame logs. The frontend still advances the SPU. */
bool ss_resimulating(void);
/* Exactly one spu_render(735) per vblank (record / replay / netplay / savestate tests). */
bool ss_deterministic_audio(void);
void ss_set_deterministic_audio(bool on);
int ss_failed(void);           /* a save-state test, replay or netplay check failed (exit status 7) */

/* Command-line options handled by the savestate / replay / netplay modules. Returns the
 * number of argv entries consumed (0 = not ours). */
int ss_parse_arg(int argc, char **argv, int i);
void ss_usage(void);
void ss_finish(void);          /* at exit: reports, files */

#endif
