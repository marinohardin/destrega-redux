/* Files embedded into the executable by tools/embed_files.py (game_sdk_files.c in the build
 * directory): the module SDK (kind 0, "sdk/<name>") and the recompiler's analysis inputs
 * (kind 1, "analysis/<path>"). */
#ifndef EMBEDDED_H
#define EMBEDDED_H
#include <stddef.h>
typedef struct {
    const char *name;
    const char *data;
    size_t size;
    int kind;
} EmbeddedFile;
extern const EmbeddedFile g_embedded_files[];
extern const int g_embedded_count;
#endif
