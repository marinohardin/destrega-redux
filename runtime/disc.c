/* Disc sources: a raw MODE2/2352 .bin (or .cue pointing at one), or the
 * compressed data pack created on first launch (Ship of Harkinian-style
 * import: the user supplies their own disc once, after which the original
 * image is no longer needed).
 *
 * Pack format (little endian):
 *   char magic[8] = "DSTRPAK1"
 *   u32 sector_count, block_sectors, block_count, exe_crc32
 *   u64 offset[block_count + 1]      (byte offset of each zlib block)
 *   zlib blocks of block_sectors raw 2352-byte sectors each
 */
#include "psx.h"
#include "disc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define SECTOR 2352
#define BLOCK_SECTORS 16
#define PAK_MAGIC "DSTRPAK1"

/* US release (SLUS-00732): SLUS_007.32 is 641024 bytes, CRC-32 57818cb0. */
#define EXE_SIZE 641024u
#define EXE_CRC 0x57818cb0u

/* ASCII case-insensitive compare of n bytes (portable strncasecmp). */
static int ci_ncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb || !ca) return ca - cb;
    }
    return 0;
}

static FILE *s_f;
static bool s_pak;
static u32 s_sectors;
static u64 *s_offsets;
static u32 s_blocks;
static u8 *s_cache, *s_zbuf;
static u32 s_cache_block = 0xffffffffu;
static u32 s_missing_logged;

u32 disc_sector_count(void) { return s_sectors; }

void disc_close(void) {
    if (s_f) fclose(s_f);
    s_f = NULL;
    free(s_offsets); s_offsets = NULL;
    free(s_cache); s_cache = NULL;
    free(s_zbuf); s_zbuf = NULL;
    s_cache_block = 0xffffffffu;
    s_pak = false;
    s_sectors = 0;
}

/* Resolve a .cue to its (single) data track file; other paths pass through. */
static bool resolve_cue(const char *path, char *out, size_t outsz) {
    size_t n = strlen(path);
    if (n < 4 || ci_ncmp(path + n - 4, ".cue", 5)) {
        snprintf(out, outsz, "%s", path);
        return true;
    }
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[1024], file[1024] = {0};
    bool mode2 = false;
    while (fgets(line, sizeof line, f)) {
        char *q1, *q2;
        if (!file[0] && strstr(line, "FILE") && (q1 = strchr(line, '"')) && (q2 = strchr(q1 + 1, '"'))) {
            *q2 = 0;
            snprintf(file, sizeof file, "%s", q1 + 1);
        }
        if (strstr(line, "MODE2/2352")) mode2 = true;
    }
    fclose(f);
    if (!file[0] || !mode2) return false;
    const char *slash = strrchr(path, '/');
    if (file[0] == '/' || !slash) snprintf(out, outsz, "%s", file);
    else snprintf(out, outsz, "%.*s/%s", (int)(slash - path), path, file);
    return true;
}

bool disc_open(const char *path) {
    disc_close();
    char real[2048];
    if (!resolve_cue(path, real, sizeof real)) return false;
    s_f = fopen(real, "rb");
    if (!s_f) return false;
    char magic[8];
    if (fread(magic, 1, 8, s_f) == 8 && !memcmp(magic, PAK_MAGIC, 8)) {
        u32 hdr[4];
        if (fread(hdr, 4, 4, s_f) != 4 || hdr[1] != BLOCK_SECTORS) { disc_close(); return false; }
        s_sectors = hdr[0];
        s_blocks = hdr[2];
        s_offsets = malloc(sizeof(u64) * (s_blocks + 1));
        if (fread(s_offsets, sizeof(u64), s_blocks + 1, s_f) != s_blocks + 1) { disc_close(); return false; }
        s_cache = malloc(BLOCK_SECTORS * SECTOR);
        s_zbuf = malloc(compressBound(BLOCK_SECTORS * SECTOR));
        s_pak = true;
        return true;
    }
    fseek(s_f, 0, SEEK_END);
    long size = ftell(s_f);
    if (size <= 0 || size % SECTOR) { disc_close(); return false; }
    s_sectors = (u32)(size / SECTOR);
    return true;
}

bool disc_read_sector(u32 lba, u8 *out) {
    if (!s_f || lba >= s_sectors) {
        memset(out, 0, SECTOR);
        if (s_f && s_missing_logged++ < 8) rt_log("disc: read past end (lba %u)", lba);
        return false;
    }
    if (!s_pak) {
        fseek(s_f, (long)lba * SECTOR, SEEK_SET);
        return fread(out, 1, SECTOR, s_f) == SECTOR;
    }
    u32 b = lba / BLOCK_SECTORS;
    if (b != s_cache_block) {
        u64 off = s_offsets[b], len = s_offsets[b + 1] - off;
        fseek(s_f, (long)off, SEEK_SET);
        uLongf outlen = BLOCK_SECTORS * SECTOR;
        if (fread(s_zbuf, 1, len, s_f) != len ||
            uncompress(s_cache, &outlen, s_zbuf, (uLong)len) != Z_OK) {
            rt_log("disc: corrupt data pack block %u", b);
            memset(out, 0, SECTOR);
            return false;
        }
        s_cache_block = b;
    }
    memcpy(out, s_cache + (lba % BLOCK_SECTORS) * SECTOR, SECTOR);
    return true;
}

/* ---- ISO9660 (enough to locate files by path) --------------------------------- */

static u32 le32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }

bool disc_find_file(const char *path, u32 *lba, u32 *size) {
    u8 sec[SECTOR];
    if (!disc_read_sector(16, sec) || memcmp(sec + 25, "CD001", 5)) return false;
    const u8 *root = sec + 24 + 156;
    u32 dlba = le32(root + 2), dsize = le32(root + 10);
    const char *p = path;
    for (;;) {
        const char *e = strchr(p, '/');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        bool found = false;
        for (u32 s = 0; s < (dsize + 2047) / 2048 && !found; s++) {
            disc_read_sector(dlba + s, sec);
            const u8 *d = sec + 24;
            for (u32 pos = 0; pos < 2048 && d[pos];) {
                const u8 *rec = d + pos;
                u8 nl = rec[32];
                const char *nm = (const char *)rec + 33;
                size_t len = 0;
                while (len < nl && nm[len] != ';') len++;
                if (len == n && !ci_ncmp(nm, p, n)) {
                    dlba = le32(rec + 2);
                    dsize = le32(rec + 10);
                    found = true;
                    break;
                }
                pos += rec[0];
            }
        }
        if (!found) return false;
        if (!e) break;
        p = e + 1;
    }
    *lba = dlba;
    *size = dsize;
    return true;
}

u8 *disc_load_file(const char *path, u32 *size_out) {
    u32 lba, size;
    if (!disc_find_file(path, &lba, &size)) return NULL;
    u8 *buf = malloc(size + 2048), sec[SECTOR];
    for (u32 i = 0; i * 2048 < size; i++) {
        disc_read_sector(lba + i, sec);
        memcpy(buf + i * 2048, sec + 24, 2048);
    }
    *size_out = size;
    return buf;
}

const char *disc_verify(void) {
    u32 size;
    u8 *exe = disc_load_file("SLUS_007.32", &size);
    if (!exe) return "This disc image is not Destrega: SLUS_007.32 was not found.";
    u32 crc = (u32)crc32(0, exe, size);
    free(exe);
    if (size != EXE_SIZE || crc != EXE_CRC)
        return "This is a different version of Destrega. Only the North American release (SLUS-00732) is supported.";
    return NULL;
}

/* ---- import ------------------------------------------------------------------------ */

/* Cheap structural checks on the file `disc_open` would read, so the user gets a precise message
 * for the common wrong inputs instead of a generic one. */
static const char *check_raw_image(const char *src) {
    static const u8 sync[12] = {0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0};
    char real[2048];
    if (!resolve_cue(src, real, sizeof real))
        return "Could not read the .cue file (expected a single MODE2/2352 data track).";
    FILE *f = fopen(real, "rb");
    if (!f) return "Could not open the disc image (for a .cue, the .bin it names must be next to it).";
    u8 head[12];
    size_t n = fread(head, 1, sizeof head, f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    if (size <= 0) return "That file is empty.";
    if (n == sizeof head && !memcmp(head, PAK_MAGIC, 8)) return "That file is already a data pack.";
    if (n < sizeof head || memcmp(head, sync, sizeof sync)) {
        if (size % 2048 == 0)
            return "This looks like a 2048-byte-sector .iso. Destrega streams its music and movies from "
                   "CD-XA (mode 2 form 2) sectors, which an .iso cannot hold, so it cannot run from one. "
                   "Use a raw 2352-byte-sector image: the .bin (or the .cue that names it).";
        return "That is not a raw MODE2/2352 disc image (expected a .bin, a .cue or a raw-sector .iso).";
    }
    if (size % SECTOR)
        return "That disc image is damaged or truncated (its size is not a whole number of 2352-byte sectors).";
    return NULL;
}

typedef struct {
    FILE *f; bool pak; u32 sectors; u64 *offsets; u32 blocks; u8 *cache, *zbuf; u32 cache_block;
} DiscState;

/* The import uses the same module state as the running game's disc. Park the game's state while
 * importing (the "Load Game Image" menu runs inside a live session) and put it back afterwards. */
static const char *disc_import_inner(const char *src, const char *pak_path, DiscProgressFn progress, void *ud);
const char *disc_import(const char *src, const char *pak_path, DiscProgressFn progress, void *ud) {
    DiscState keep = { s_f, s_pak, s_sectors, s_offsets, s_blocks, s_cache, s_zbuf, s_cache_block };
    s_f = NULL; s_pak = false; s_sectors = 0; s_offsets = NULL; s_blocks = 0;
    s_cache = s_zbuf = NULL; s_cache_block = 0xffffffffu;
    const char *err = disc_import_inner(src, pak_path, progress, ud);
    disc_close();
    s_f = keep.f; s_pak = keep.pak; s_sectors = keep.sectors; s_offsets = keep.offsets;
    s_blocks = keep.blocks; s_cache = keep.cache; s_zbuf = keep.zbuf; s_cache_block = keep.cache_block;
    return err;
}

static const char *disc_import_inner(const char *src, const char *pak_path, DiscProgressFn progress, void *ud) {
    const char *pre = check_raw_image(src);
    if (pre) return pre;
    if (!disc_open(src)) return "Could not open the disc image (expected a MODE2/2352 .bin or a .cue).";
    if (s_pak) { disc_close(); return "That file is already a data pack."; }
    const char *err = disc_verify();
    if (err) { disc_close(); return err; }
    {   /* the ISO9660 volume must fit in the file: catches truncated copies of the real disc */
        u8 pvd[SECTOR];
        if (!disc_read_sector(16, pvd) || le32(pvd + 24 + 80) > s_sectors) {
            disc_close();
            return "This disc image is incomplete (shorter than the disc it describes). Re-dump or re-copy it.";
        }
    }

    char tmp[2048];
    snprintf(tmp, sizeof tmp, "%s.tmp", pak_path);
    FILE *o = fopen(tmp, "wb");
    if (!o) { disc_close(); return "Could not write the data pack (is the data folder writable?)."; }
    u32 blocks = (s_sectors + BLOCK_SECTORS - 1) / BLOCK_SECTORS;
    u32 hdr[4] = {s_sectors, BLOCK_SECTORS, blocks, EXE_CRC};
    u64 *offs = calloc(blocks + 1, sizeof(u64));
    fwrite(PAK_MAGIC, 1, 8, o);
    fwrite(hdr, 4, 4, o);
    fwrite(offs, sizeof(u64), blocks + 1, o);   /* placeholder, rewritten below */
    u64 pos = 8 + 16 + (u64)sizeof(u64) * (blocks + 1);
    u8 *raw = malloc(BLOCK_SECTORS * SECTOR);
    uLong zcap = compressBound(BLOCK_SECTORS * SECTOR);
    u8 *z = malloc(zcap);
    bool ok = true;
    for (u32 b = 0; b < blocks && ok; b++) {
        memset(raw, 0, BLOCK_SECTORS * SECTOR);
        for (u32 i = 0; i < BLOCK_SECTORS && b * BLOCK_SECTORS + i < s_sectors; i++)
            disc_read_sector(b * BLOCK_SECTORS + i, raw + i * SECTOR);
        uLongf zlen = zcap;
        ok = compress2(z, &zlen, raw, BLOCK_SECTORS * SECTOR, 6) == Z_OK &&
             fwrite(z, 1, zlen, o) == zlen;
        offs[b] = pos;
        pos += zlen;
        if (progress && (b % 256 == 0 || b + 1 == blocks)) progress(ud, (float)(b + 1) / blocks);
    }
    offs[blocks] = pos;
    if (ok) {
        fseek(o, 8 + 16, SEEK_SET);
        ok = fwrite(offs, sizeof(u64), blocks + 1, o) == blocks + 1;
    }
    ok = (fclose(o) == 0) && ok;
    free(raw); free(z); free(offs);
    disc_close();
    if (!ok) { remove(tmp); return "Writing the data pack failed (disk full?)."; }

    /* re-open and verify before committing */
    if (!disc_open(tmp) || disc_verify()) { disc_close(); remove(tmp); return "The new data pack failed verification."; }
    disc_close();
    if (rename(tmp, pak_path)) { remove(tmp); return "Could not move the data pack into place."; }
    return NULL;
}
