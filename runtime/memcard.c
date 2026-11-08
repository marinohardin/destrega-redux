/* PS1 memory card images and the BIOS "bu" filesystem on top of them. See memcard.h.
 *
 * Image layout (psx-spx "Memory Card Data Format"): 1024 sectors of 128 bytes = 16 blocks of 8 KiB.
 *   sector 0         "MC" header, byte 127 = XOR of bytes 0..126
 *   sectors 1..15    directory: one entry per data block 1..15
 *                      +0 u32 state (51 first/only, 52 middle, 53 last, A0 free, A1..A3 deleted)
 *                      +4 u32 file size (first block only)  +8 u16 next block - 1 (FFFF = last)
 *                      +10 name[20]   +127 XOR checksum
 *   sectors 16..35   broken-sector list     36..62 replacement sectors     63 write-test sector
 *   sectors 64..1023 data blocks 1..15
 */
#include "memcard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"

typedef struct {
    bool present, dirty, is_new;
    char *path;
    uint8_t img[MC_SIZE];
} Card;

typedef struct { bool used, writable; int port, head; uint32_t size, pos; } Fd;
static Fd s_fd[MC_MAXFD];

static Card s_card[2];
static bool s_write_protect;

static uint8_t xor127(const uint8_t *p) {
    uint8_t x = 0;
    for (int i = 0; i < 127; i++) x ^= p[i];
    return x;
}

static uint8_t *dirent(Card *c, int block) { return c->img + (size_t)block * MC_SECTOR; }

static void dir_seal(uint8_t *e) { e[127] = xor127(e); }

void memcard_make_blank(uint8_t img[MC_SIZE]) {
    memset(img, 0, MC_SIZE);
    img[0] = 'M'; img[1] = 'C';
    img[127] = xor127(img);
    for (int b = 1; b <= MC_BLOCKS; b++) {
        uint8_t *e = img + b * MC_SECTOR;
        e[0] = 0xa0;
        e[8] = e[9] = 0xff;
        dir_seal(e);
    }
    for (int s = 16; s < 36; s++) {          /* empty broken-sector list */
        uint8_t *e = img + s * MC_SECTOR;
        memset(e, 0xff, 4);
        e[8] = e[9] = 0xff;
        dir_seal(e);
    }
}

static void mark_dirty(Card *c) { c->dirty = true; }

const char *memcard_attach(int port, const char *path) {
    static char err[600];
    Card *c = &s_card[port & 1];
    free(c->path);
    memset(c, 0, sizeof *c);
    for (int h = 0; h < MC_MAXFD; h++)        /* swapping a card invalidates handles open on it */
        if (s_fd[h].used && s_fd[h].port == (port & 1)) s_fd[h].used = false;
    if (!path || !*path) return NULL;
    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        bool ok = n == MC_SIZE && fread(c->img, 1, MC_SIZE, f) == MC_SIZE;
        fclose(f);
        if (!ok) {
            snprintf(err, sizeof err, "memory card %s: not a raw 128 KiB image (%ld bytes); ignoring it", path, n);
            memset(c, 0, sizeof *c);
            return err;
        }
    } else {
        memcard_make_blank(c->img);
        c->dirty = true;                      /* create the file */
    }
    c->path = strdup(path);
    c->present = true;
    c->is_new = true;
    if (c->dirty) memcard_flush();
    return NULL;
}

bool memcard_present(int port) { return s_card[port & 1].present; }
const char *memcard_path(int port) { return s_card[port & 1].path; }
void memcard_mark_new(int port) { if (s_card[port & 1].present) s_card[port & 1].is_new = true; }

const char *memcard_validate_file(const char *path) {
    static uint8_t img[MC_SIZE];
    FILE *f = fopen(path, "rb");
    if (!f) return "Could not open that file.";
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    bool ok = n == MC_SIZE && fread(img, 1, MC_SIZE, f) == MC_SIZE;
    fclose(f);
    if (!ok) return "That is not a raw 128 KiB memory card image (.mcd). DexDrive/.gme-style files with a header are not supported.";
    if (img[0] != 'M' || img[1] != 'C') return "That file is 128 KiB but has no memory card header (\"MC\"); it is not a formatted memory card image.";
    if (img[127] != xor127(img)) return "That memory card image has a damaged header (checksum mismatch).";
    return NULL;
}

/* Write `img` to `path` atomically (temp file + rename). */
static bool write_file_atomic(const char *path, const uint8_t *img) {
    size_t n = strlen(path) + 8;
    char *tmp = malloc(n);
    snprintf(tmp, n, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    bool ok = f && fwrite(img, 1, MC_SIZE, f) == MC_SIZE && fflush(f) == 0;
    if (f) fclose(f);
    if (ok) ok = plat_replace_file(tmp, path);
    if (!ok) remove(tmp);
    free(tmp);
    return ok;
}

const char *memcard_export(int port, const char *dst) {
    Card *c = &s_card[port & 1];
    if (!c->present) return "No card is inserted in that slot.";
    memcard_flush();
    return write_file_atomic(dst, c->img) ? NULL : "Could not write the file (is the destination writable?).";
}
void memcard_set_write_protect(bool on) { s_write_protect = on; }

void memcard_flush(void) {
    for (int p = 0; p < 2; p++) {
        Card *c = &s_card[p];
        if (!c->present || !c->dirty || !c->path || s_write_protect) continue;
        size_t n = strlen(c->path) + 8;
        char *tmp = malloc(n);
        snprintf(tmp, n, "%s.tmp", c->path);
        FILE *f = fopen(tmp, "wb");
        bool ok = f && fwrite(c->img, 1, MC_SIZE, f) == MC_SIZE && fflush(f) == 0;
        if (ok) plat_fsync(f);
        if (f) fclose(f);
        if (ok) ok = plat_replace_file(tmp, c->path);
        if (!ok) { fprintf(stderr, "memcard: cannot write %s\n", c->path); remove(tmp); }
        else c->dirty = false;
        free(tmp);
    }
}

bool memcard_formatted(int port) {
    Card *c = &s_card[port & 1];
    return c->present && c->img[0] == 'M' && c->img[1] == 'C' && c->img[127] == xor127(c->img);
}

void memcard_format(int port) {
    Card *c = &s_card[port & 1];
    if (!c->present) return;
    memcard_make_blank(c->img);
    mark_dirty(c);
}

bool memcard_new_flag(int port) { return s_card[port & 1].is_new; }
void memcard_clear_new(int port) { s_card[port & 1].is_new = false; }

bool memcard_read_sector(int port, uint32_t sector, uint8_t *dst) {
    Card *c = &s_card[port & 1];
    if (!c->present || sector >= MC_SIZE / MC_SECTOR) return false;
    memcpy(dst, c->img + (size_t)sector * MC_SECTOR, MC_SECTOR);
    return true;
}

bool memcard_write_sector(int port, uint32_t sector, const uint8_t *src) {
    Card *c = &s_card[port & 1];
    if (!c->present || sector >= MC_SIZE / MC_SECTOR) return false;
    if (memcmp(c->img + (size_t)sector * MC_SECTOR, src, MC_SECTOR)) {
        memcpy(c->img + (size_t)sector * MC_SECTOR, src, MC_SECTOR);
        mark_dirty(c);
    }
    c->is_new = false;                        /* any write clears the card-changed flag */
    return true;
}

uint64_t memcard_hash(uint64_t h) {
    for (int p = 0; p < 2; p++) {
        Card *c = &s_card[p];
        h = (h ^ (uint64_t)(c->present | (c->is_new << 1))) * 0x9E3779B97F4A7C15ull;
        if (!c->present) continue;
        for (size_t i = 0; i < MC_SIZE; i += 8) {
            uint64_t w;
            memcpy(&w, c->img + i, 8);
            h = (h ^ w) * 0x9E3779B97F4A7C15ull;
            h ^= h >> 29;
        }
    }
    return h;
}

/* ---- file layer ---------------------------------------------------------------------- */

static bool is_free(const uint8_t *e) { return (e[0] & 0xf0) == 0xa0; }

static bool name_eq(const uint8_t *e, const char *name) {
    return strncmp((const char *)e + 10, name, 20) == 0;
}

/* Block chain of the file starting at `head` into out[]; returns the length. */
static int chain(Card *c, int head, int out[MC_BLOCKS]) {
    int n = 0, b = head;
    while (b >= 1 && b <= MC_BLOCKS && n < MC_BLOCKS) {
        out[n++] = b;
        const uint8_t *e = dirent(c, b);
        unsigned next = e[8] | (e[9] << 8);
        if (next == 0xffff) break;
        b = (int)next + 1;
    }
    return n;
}

static int find_file(Card *c, const char *name) {
    for (int b = 1; b <= MC_BLOCKS; b++) {
        const uint8_t *e = dirent(c, b);
        if (e[0] == 0x51 && name_eq(e, name)) return b;
    }
    return 0;
}

int memcard_open(int port, const char *name, bool create, int blocks, bool writable) {
    Card *c = &s_card[port & 1];
    if (!memcard_formatted(port) || !*name || strlen(name) > 20) return -1;
    int h = 0;
    for (; h < MC_MAXFD; h++) if (!s_fd[h].used) break;
    if (h == MC_MAXFD) return -1;
    int head = find_file(c, name);
    if (create) {
        if (head || blocks < 1 || blocks > MC_BLOCKS) return -1;
        int pick[MC_BLOCKS], n = 0;
        for (int b = 1; b <= MC_BLOCKS && n < blocks; b++) if (is_free(dirent(c, b))) pick[n++] = b;
        if (n < blocks) return -1;
        for (int i = 0; i < n; i++) {
            uint8_t *e = dirent(c, pick[i]);
            memset(e, 0, MC_SECTOR);
            e[0] = i == 0 ? 0x51 : (i == n - 1 ? 0x53 : 0x52);
            if (i == 0) {
                uint32_t sz = (uint32_t)blocks * MC_BLOCK;
                memcpy(e + 4, &sz, 4);               /* little-endian host assumed (as elsewhere) */
                memcpy(e + 10, name, strlen(name));
            }
            unsigned next = i == n - 1 ? 0xffff : (unsigned)pick[i + 1] - 1;
            e[8] = next & 0xff; e[9] = next >> 8;
            dir_seal(e);
        }
        mark_dirty(c);
        head = pick[0];
        writable = true;
    } else if (!head) {
        return -1;
    }
    const uint8_t *e = dirent(c, head);
    uint32_t size;
    memcpy(&size, e + 4, 4);
    s_fd[h] = (Fd){true, writable, port & 1, head, size, 0};
    return h;
}

int memcard_close(int h) {
    if (h < 0 || h >= MC_MAXFD || !s_fd[h].used) return -1;
    s_fd[h].used = false;
    return h;
}

int memcard_handle_port(int h) { return h >= 0 && h < MC_MAXFD && s_fd[h].used ? s_fd[h].port : -1; }

long memcard_seek(int h, long off, int whence) {
    if (h < 0 || h >= MC_MAXFD || !s_fd[h].used) return -1;
    Fd *f = &s_fd[h];
    long p = whence == 0 ? off : whence == 1 ? (long)f->pos + off : (long)f->size + off;
    if (p < 0 || p > (long)f->size) return -1;
    f->pos = (uint32_t)p;
    return p;
}

/* Copy n bytes between the file at its position and `buf`; advances the position. */
static long xfer(int h, uint8_t *buf, size_t n, bool write) {
    if (h < 0 || h >= MC_MAXFD || !s_fd[h].used) return -1;
    Fd *f = &s_fd[h];
    Card *c = &s_card[f->port];
    if (!c->present || (write && !f->writable)) return -1;
    if (n > f->size - f->pos) n = f->size - f->pos;
    int ch[MC_BLOCKS];
    int len = chain(c, f->head, ch);
    size_t done = 0;
    while (done < n) {
        uint32_t p = f->pos + (uint32_t)done;
        if ((int)(p / MC_BLOCK) >= len) return done ? (long)done : -1;
        size_t off = (size_t)ch[p / MC_BLOCK] * MC_BLOCK + p % MC_BLOCK;
        size_t k = MC_BLOCK - p % MC_BLOCK;
        if (k > n - done) k = n - done;
        if (write) {
            if (memcmp(c->img + off, buf + done, k)) { memcpy(c->img + off, buf + done, k); mark_dirty(c); }
        } else {
            memcpy(buf + done, c->img + off, k);
        }
        done += k;
    }
    if (write) c->is_new = false;
    f->pos += (uint32_t)done;
    return (long)done;
}

long memcard_read(int h, uint8_t *dst, size_t n) { return xfer(h, dst, n, false); }
long memcard_write(int h, const uint8_t *src, size_t n) { return xfer(h, (uint8_t *)src, n, true); }

static bool match(const char *pat, const char *s) {
    for (;; pat++, s++) {
        if (*pat == '*') return true;
        if (!*pat) return !*s;
        if (*pat != '?' && *pat != *s) return false;
        if (!*s) return false;
    }
}

bool memcard_dir_next(int port, const char *pattern, int *cursor, McDirEntry *out) {
    Card *c = &s_card[port & 1];
    if (!memcard_formatted(port)) return false;
    for (int b = *cursor < 1 ? 1 : *cursor; b <= MC_BLOCKS; b++) {
        const uint8_t *e = dirent(c, b);
        if (e[0] != 0x51) continue;
        char name[21];
        memcpy(name, e + 10, 20);
        name[20] = 0;
        if (*pattern && !match(pattern, name)) continue;
        memcpy(out->name, name, 21);
        memcpy(&out->size, e + 4, 4);
        out->head = b;
        *cursor = b + 1;
        return true;
    }
    *cursor = MC_BLOCKS + 1;
    return false;
}

bool memcard_erase(int port, const char *name) {
    Card *c = &s_card[port & 1];
    if (!memcard_formatted(port)) return false;
    int head = find_file(c, name);
    if (!head) return false;
    int ch[MC_BLOCKS];
    int n = chain(c, head, ch);
    for (int i = 0; i < n; i++) {
        uint8_t *e = dirent(c, ch[i]);
        e[0] = i == 0 ? 0xa1 : (i == n - 1 ? 0xa3 : 0xa2);
        dir_seal(e);
    }
    mark_dirty(c);
    c->is_new = false;
    return true;
}

/* ---- save states (savestate.h) ---------------------------------------------- */
#include "savestate.h"
/* Images, card-changed flags and open files. The attached file paths and write protection are
 * host settings, not machine state. A restored image that differs from the current one is
 * marked dirty so the next flush writes it (no-op while write-protected, e.g. netplay). */
void memcard_state_io(StateIO *io) {
    for (int p = 0; p < 2; p++) {
        Card *c = &s_card[p];
        bool present = c->present;
        SS_VAR(io, present);
        SS_VAR(io, c->is_new);
        if (io->mode == SS_LOAD && present != c->present) {   /* different card setup: skip */
            if (present) io->pos += MC_SIZE;
            continue;
        }
        if (!present) continue;
        if (io->mode == SS_LOAD) {
            if (io->pos + MC_SIZE <= io->cap && memcmp(c->img, io->buf + io->pos, MC_SIZE)) c->dirty = true;
        }
        ss_io(io, c->img, MC_SIZE);
    }
    for (int i = 0; i < MC_MAXFD; i++) {
        Fd *f = &s_fd[i];
        SS_VAR(io, f->used); SS_VAR(io, f->writable); SS_VAR(io, f->port); SS_VAR(io, f->head);
        SS_VAR(io, f->size); SS_VAR(io, f->pos);
    }
}
