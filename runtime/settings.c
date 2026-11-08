/* Saved settings store and INI-style file (see settings.h). Plain C, no SDL. */
#include "settings.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_ENTRIES 512
#define MAX_LOCKS 32
#define SEC_LEN 32
#define KEY_LEN 64
#define VAL_LEN 512
#define LINE_MAX_LEN 1024
#define FILE_MAX (256 * 1024)

typedef struct { char sec[SEC_LEN], key[KEY_LEN], val[VAL_LEN]; } Entry;
static Entry s_e[MAX_ENTRIES];
static int s_n;
static struct { char sec[SEC_LEN], key[KEY_LEN]; } s_lock[MAX_LOCKS];
static int s_nlock;
static char s_path[2048];
static bool s_dirty;
static uint64_t s_changed_ns, s_last_seen_ns;

static bool ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return !*a && !*b;
}

static bool name_ok(const char *s, size_t max) {
    size_t n = strlen(s);
    if (!n || n >= max) return false;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '_' && *s != '.' && *s != '-') return false;
    return true;
}

static bool value_ok(const char *v) {
    if (strlen(v) >= VAL_LEN) return false;
    for (; *v; v++) if ((unsigned char)*v < 0x20 && *v != '\t') return false;   /* no control chars */
    return true;
}

static Entry *find(const char *sec, const char *key) {
    for (int i = 0; i < s_n; i++)
        if (!strcmp(s_e[i].sec, sec) && !strcmp(s_e[i].key, key)) return &s_e[i];
    return NULL;
}

static bool locked(const char *sec, const char *key) {
    for (int i = 0; i < s_nlock; i++)
        if (!strcmp(s_lock[i].sec, sec) && !strcmp(s_lock[i].key, key)) return true;
    return false;
}

/* raw insert/update, no lock or dirty handling */
static bool put(const char *sec, const char *key, const char *val) {
    Entry *e = find(sec, key);
    if (!e) {
        if (s_n >= MAX_ENTRIES) return false;
        /* keep a section's keys together: insert after the section's last key */
        int at = s_n;
        for (int i = s_n - 1; i >= 0; i--) if (!strcmp(s_e[i].sec, sec)) { at = i + 1; break; }
        memmove(&s_e[at + 1], &s_e[at], (size_t)(s_n - at) * sizeof *s_e);
        s_n++;
        e = &s_e[at];
        snprintf(e->sec, SEC_LEN, "%s", sec);
        snprintf(e->key, KEY_LEN, "%s", key);
    }
    snprintf(e->val, VAL_LEN, "%s", val);
    return true;
}

void settings_set_path(const char *path) { snprintf(s_path, sizeof s_path, "%s", path ? path : ""); }
const char *settings_path(void) { return s_path[0] ? s_path : NULL; }
void settings_clear(void) { s_n = 0; s_dirty = false; }

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t l = strlen(s);
    while (l && (s[l - 1] == ' ' || s[l - 1] == '\t' || s[l - 1] == '\r')) s[--l] = 0;
    return s;
}

int settings_parse(const char *text) {
    settings_clear();
    int bad = 0, lineno = 0;
    char sec[SEC_LEN] = "";
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        lineno++;
        char line[LINE_MAX_LEN];
        bool ok = len < sizeof line;
        if (ok) {
            memcpy(line, p, len);
            line[len] = 0;
            ok = strlen(line) == len;   /* embedded NUL */
        }
        p += len + (eol ? 1 : 0);
        if (!ok) { if (bad++ < 8) fprintf(stderr, "settings: line %d ignored (too long or not text)\n", lineno); continue; }
        char *s = trim(line);
        if (lineno == 1 && (unsigned char)s[0] == 0xef && (unsigned char)s[1] == 0xbb && (unsigned char)s[2] == 0xbf) s += 3;   /* UTF-8 BOM */
        if (!*s || *s == '#' || *s == ';') continue;
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (e && !*trim(e + 1)) {
                *e = 0;
                char *name = trim(s + 1);
                if (name_ok(name, SEC_LEN)) { snprintf(sec, sizeof sec, "%s", name); continue; }
            }
            sec[0] = 0;   /* keys under a bad header are ignored too */
            if (bad++ < 8) fprintf(stderr, "settings: line %d ignored (bad section header)\n", lineno);
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq || !sec[0]) { if (bad++ < 8) fprintf(stderr, "settings: line %d ignored (%s)\n", lineno, sec[0] ? "no '='" : "outside a section"); continue; }
        *eq = 0;
        char *k = trim(s), *v = trim(eq + 1);
        if (!name_ok(k, KEY_LEN) || !value_ok(v) || !put(sec, k, v)) {
            if (bad++ < 8) fprintf(stderr, "settings: line %d ignored (bad key or value)\n", lineno);
        }
    }
    s_dirty = false;
    return bad;
}

int settings_load(void) {
    settings_clear();
    if (!s_path[0]) return 0;
    FILE *f = fopen(s_path, "rb");
    if (!f) return 0;   /* first run: nothing saved yet */
    char *buf = malloc(FILE_MAX + 1);
    size_t n = buf ? fread(buf, 1, FILE_MAX + 1, f) : 0;
    fclose(f);
    if (!buf) return -1;
    if (n > FILE_MAX) {
        fprintf(stderr, "settings: %s is larger than %d KiB; ignored (it will be rewritten when a setting changes)\n", s_path, FILE_MAX / 1024);
        free(buf);
        return -1;
    }
    buf[n] = 0;
    int bad = settings_parse(buf);
    free(buf);
    if (bad) fprintf(stderr, "settings: %s: %d line(s) ignored\n", s_path, bad);
    return bad;
}

char *settings_format(void) {
    size_t cap = 256 + (size_t)s_n * (SEC_LEN + KEY_LEN + VAL_LEN + 8);
    char *out = malloc(cap), *o = out;
    if (!out) return NULL;
    o += snprintf(o, cap, "# Destrega settings. Written by the game; edit only while it is not running.\n"
                          "# Command-line options override these for one run and are not saved.\n");
    const char *cur = NULL;
    for (int i = 0; i < s_n; i++) {
        if (!cur || strcmp(cur, s_e[i].sec)) {
            cur = s_e[i].sec;
            o += snprintf(o, cap - (size_t)(o - out), "\n[%s]\n", cur);
        }
        o += snprintf(o, cap - (size_t)(o - out), "%s = %s\n", s_e[i].key, s_e[i].val);
    }
    return out;
}

const char *settings_get(const char *section, const char *key) {
    Entry *e = find(section, key);
    return e ? e->val : NULL;
}

int settings_get_int(const char *section, const char *key, int def) {
    const char *v = settings_get(section, key);
    if (!v || !*v) return def;
    char *end;
    long x = strtol(v, &end, 10);
    return *end || x < -1000000000L || x > 1000000000L ? def : (int)x;
}

bool settings_get_bool(const char *section, const char *key, bool def) {
    const char *v = settings_get(section, key);
    if (!v) return def;
    static const char *const yes[] = { "on", "true", "yes", "1" }, *const no[] = { "off", "false", "no", "0" };
    for (int i = 0; i < 4; i++) {
        if (ieq(v, yes[i])) return true;
        if (ieq(v, no[i])) return false;
    }
    return def;
}

bool settings_set(const char *section, const char *key, const char *value) {
    if (!name_ok(section, SEC_LEN) || !name_ok(key, KEY_LEN) || locked(section, key)) return false;
    Entry *e = find(section, key);
    if (!value) {
        if (!e) return true;
        int i = (int)(e - s_e);
        memmove(&s_e[i], &s_e[i + 1], (size_t)(s_n - i - 1) * sizeof *s_e);
        s_n--;
    } else {
        if (!value_ok(value)) return false;
        if (e && !strcmp(e->val, value)) return true;
        if (!put(section, key, value)) return false;
    }
    s_dirty = true;
    s_changed_ns = s_last_seen_ns;
    return true;
}

bool settings_set_int(const char *section, const char *key, int v) {
    char b[24];
    snprintf(b, sizeof b, "%d", v);
    return settings_set(section, key, b);
}
bool settings_set_bool(const char *section, const char *key, bool v) { return settings_set(section, key, v ? "on" : "off"); }

void settings_lock(const char *section, const char *key) {
    if (locked(section, key) || s_nlock >= MAX_LOCKS) return;
    snprintf(s_lock[s_nlock].sec, SEC_LEN, "%s", section);
    snprintf(s_lock[s_nlock].key, KEY_LEN, "%s", key);
    s_nlock++;
}

bool settings_dirty(void) { return s_dirty; }

bool settings_save(void) {
    if (!s_dirty || !s_path[0]) return true;
    char *text = settings_format();
    if (!text) return false;
    char tmp[2100];
    snprintf(tmp, sizeof tmp, "%s.tmp", s_path);
    FILE *f = fopen(tmp, "wb");
    bool ok = f != NULL;
    if (ok) ok = fwrite(text, 1, strlen(text), f) == strlen(text);
    if (f) { if (ok) { fflush(f); plat_fsync(f); } ok = fclose(f) == 0 && ok; }
    if (ok) ok = plat_replace_file(tmp, s_path);
    if (!ok) { remove(tmp); fprintf(stderr, "settings: cannot write %s\n", s_path); }
    else s_dirty = false;
    free(text);
    return ok;
}

void settings_autosave(uint64_t now_ns) {
    if (!s_dirty) { s_last_seen_ns = now_ns; return; }
    if (s_changed_ns == 0 || s_changed_ns > now_ns) s_changed_ns = now_ns;
    s_last_seen_ns = now_ns;
    if (now_ns - s_changed_ns >= 500000000ull) settings_save();
}
