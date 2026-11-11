/* C port of the static recompiler (recomp/ (*.py)), for the launcher: players need no Python.
 *
 * recomp/recomp.py stays the reference (developer build, CI). This port must produce
 * byte-identical output for the real disc: tools/test_recomp_c.sh diffs every file.
 * Every rule here mirrors a line of recomp/{recomp,analyze,codegen,mips}.py; change both. */
#ifndef RECOMPILER_H
#define RECOMPILER_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    /* a disc file ("SLUS_007.32", "OVL/MGRAD.BIN"): malloc'd bytes, NULL if missing */
    uint8_t *(*disc_file)(void *ud, const char *path, uint32_t *size);
    /* an analysis input relative to analysis/ ("extra_seeds.txt", "overlays/MGRAD.procs.json"):
     * malloc'd bytes (NUL-terminated), NULL if absent (the Python reader skips absent files) */
    char *(*analysis_file)(void *ud, const char *rel, size_t *len);
    void *ud;
    const char *out_dir;   /* gen/ equivalent; must exist */
    /* optional: one line per report entry the Python driver prints */
    void (*log)(void *ud, const char *line);
} RecompInput;

/* Returns NULL on success, else an error message (static storage). On success *files_out
 * (if not NULL) receives the number of C files written (the compile units). */
const char *recomp_run(const RecompInput *in, int *files_out);

#endif
