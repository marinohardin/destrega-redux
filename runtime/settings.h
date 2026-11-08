/* Saved settings: a small INI-style text file (settings.ini next to the data pack, in SDL's pref
 * path), one store of "section.key = value" strings for the frontend (display, netplay defaults)
 * and the input layer (controls).
 *
 * File format (written by the game; hand edits are fine while it is not running):
 *     # comment            (also ';' at the start of a line)
 *     [section]
 *     key = value          (value runs to the end of the line, spaces trimmed)
 * Section and key names: letters, digits, '_', '.', '-'. Anything else on a line, over-long lines
 * and keys outside a section are ignored (logged once with the line number), never fatal. Unknown
 * keys are kept and written back. Comments are not preserved (the file is rewritten).
 *
 * Writing is atomic (temp file, fsync, replace) and only happens when a value changed and a path is
 * set. Headless runs have no path unless --settings FILE is given, so tests never see the user's
 * file. Keys "locked" by a command-line flag keep their file value: settings_set on them changes
 * nothing, so a flag overrides the file for one run and never overwrites it. */
#ifndef SETTINGS_H
#define SETTINGS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* File to load from / save to; NULL = none (in-memory only). */
void settings_set_path(const char *path);
const char *settings_path(void);
/* Load the file at the path (a missing file is an empty store). Returns the number of ignored lines,
 * or -1 if the file exists but cannot be read (the store is then empty). */
int settings_load(void);
/* Parse text into the store (replacing it). Returns the number of ignored lines. */
int settings_parse(const char *text);
/* The store as file text (malloc'd, caller frees). */
char *settings_format(void);
void settings_clear(void);

/* NULL if absent. */
const char *settings_get(const char *section, const char *key);
int settings_get_int(const char *section, const char *key, int def);
/* on/off, true/false, yes/no, 1/0; def otherwise */
bool settings_get_bool(const char *section, const char *key, bool def);
/* value NULL removes the key. Marks the store dirty if it changed. Refused (false) for locked keys,
 * bad names or over-long values. */
bool settings_set(const char *section, const char *key, const char *value);
bool settings_set_int(const char *section, const char *key, int v);
bool settings_set_bool(const char *section, const char *key, bool v);
/* A command-line flag overrides this key for the run: it is never changed or written. */
void settings_lock(const char *section, const char *key);

bool settings_dirty(void);
/* Write the file now if dirty and a path is set (atomically). True if nothing needed writing or the
 * write worked. */
bool settings_save(void);
/* Debounced save for frequent changes (window resizes): writes once no change happened for 0.5 s. */
void settings_autosave(uint64_t now_ns);
#endif
