/* PS1 memory card devices: standard 128 KiB raw card images (.mcd) in two slots, plus the
 * BIOS "bu" filesystem (open/read/write/firstfile/erase/format) the game's libmcrd uses.
 * Host-only (no guest memory access) so it can be unit-tested on its own; runtime/bios.c
 * adapts it to the BIOS calls and events. */
#ifndef MEMCARD_H
#define MEMCARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MC_SIZE 131072
#define MC_SECTOR 128
#define MC_BLOCK 8192
#define MC_BLOCKS 15

/* Load `path` into slot `port` (0/1); a missing file is created as a formatted empty card.
 * Returns NULL on success or an error message (the slot stays empty; a file that is not a
 * 128 KiB image is never overwritten). Passing NULL leaves the slot empty. */
const char *memcard_attach(int port, const char *path);
bool memcard_present(int port);
/* Write dirty cards to disk (temp file + rename). Cheap when nothing changed. */
void memcard_flush(void);
/* Make the next flushes no-ops (netplay / read-only use); in-memory state still updates. */
void memcard_set_write_protect(bool on);

/* Fresh formatted-empty image (what a new card looks like). */
void memcard_make_blank(uint8_t img[MC_SIZE]);
bool memcard_formatted(int port);
void memcard_format(int port);

/* "Card changed" flag, like the status bit real cards raise after power-up until the first
 * write. Set on attach. */
bool memcard_new_flag(int port);
void memcard_clear_new(int port);

/* Raw sector access (0..1023). */
bool memcard_read_sector(int port, uint32_t sector, uint8_t *dst);
bool memcard_write_sector(int port, uint32_t sector, const uint8_t *src);

/* Mixed into the --state-hash. */
uint64_t memcard_hash(uint64_t h);

/* ---- file layer (the BIOS bu00:/bu10: device) ---- */
#define MC_MAXFD 16
typedef struct {
    char name[21];
    uint32_t size;
    int head;        /* first block, 1..15 */
} McDirEntry;

/* Open `name` (without "buNN:"). create: allocate `blocks` blocks (8 KiB each) for a new file.
 * Returns a handle (>= 0) or -1. */
int memcard_open(int port, const char *name, bool create, int blocks, bool writable);
int memcard_close(int handle);
int memcard_handle_port(int handle);   /* -1 if not open */
long memcard_seek(int handle, long off, int whence);
/* Byte-granular transfers at the file position; return bytes moved or -1. */
long memcard_read(int handle, uint8_t *dst, size_t n);
long memcard_write(int handle, const uint8_t *src, size_t n);
/* Directory walk over files matching `pattern` ('*' and '?' wildcards, "" = all). `cursor`
 * starts at 0. Returns true and fills `out` for the next match. */
bool memcard_dir_next(int port, const char *pattern, int *cursor, McDirEntry *out);
bool memcard_erase(int port, const char *name);

#endif
