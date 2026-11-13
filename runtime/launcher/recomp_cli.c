/* Standalone driver for the C recompiler (tests): recomp_c DISC_DIR ANALYSIS_DIR OUT_DIR.
 * DISC_DIR holds the extracted disc files (tools/extract_disc.py). tools/test_recomp_c.sh
 * diffs its output against recomp/recomp.py's. */
#include "recompiler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *s_disc, *s_analysis;

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *p = malloc((size_t)n + 1);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(p); return NULL; }
    fclose(f);
    p[n] = 0;
    *len = (size_t)n;
    return p;
}
static uint8_t *disc_file(void *ud, const char *path, uint32_t *size)
{
    (void)ud;
    char full[4096];
    snprintf(full, sizeof full, "%s/%s", s_disc, path);
    size_t n;
    uint8_t *p = (uint8_t *)slurp(full, &n);
    if (p) *size = (uint32_t)n;
    return p;
}
static char *analysis_file(void *ud, const char *rel, size_t *len)
{
    (void)ud;
    char full[4096];
    snprintf(full, sizeof full, "%s/%s", s_analysis, rel);
    return slurp(full, len);
}
static void log_line(void *ud, const char *line) { (void)ud; puts(line); }

int main(int argc, char **argv)
{
    if (argc != 4) { fprintf(stderr, "usage: %s DISC_DIR ANALYSIS_DIR OUT_DIR\n", argv[0]); return 2; }
    s_disc = argv[1];
    s_analysis = argv[2];
    RecompInput in = {disc_file, analysis_file, NULL, argv[3], log_line};
    int files = 0;
    const char *err = recomp_run(&in, &files);
    if (err) { fprintf(stderr, "recompile failed: %s\n", err); return 1; }
    return 0;
}
