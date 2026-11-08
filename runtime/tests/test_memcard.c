/* Unit tests for runtime/memcard.c (card image format, bu filesystem, atomic persistence). */
#include "memcard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static long fsize(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

int main(void) {
    const char *path = "build/tests/mc_test.mcd";
    remove(path);
    CHECK(memcard_attach(0, path) == NULL);       /* missing file -> created, formatted */
    CHECK(memcard_present(0) && !memcard_present(1));
    CHECK(fsize(path) == MC_SIZE);
    CHECK(memcard_formatted(0) && memcard_new_flag(0));

    int h = memcard_open(0, "BASLUS-00732DESTREGA", true, 1, true);
    CHECK(h >= 0);
    CHECK(memcard_open(0, "BASLUS-00732DESTREGA", true, 1, true) == -1);   /* already exists */
    uint8_t buf[300], back[300];
    for (int i = 0; i < 300; i++) buf[i] = (uint8_t)(i * 7);
    CHECK(memcard_write(h, buf, 300) == 300);
    CHECK(!memcard_new_flag(0));                  /* a write clears the card-changed flag */
    CHECK(memcard_seek(h, 0, 0) == 0);
    CHECK(memcard_read(h, back, 300) == 300 && !memcmp(buf, back, 300));
    CHECK(memcard_seek(h, 0, 2) == MC_BLOCK);
    CHECK(memcard_write(h, buf, 1) == 0);         /* no growing past the block count */
    CHECK(memcard_close(h) == h);

    /* a 3-block file chains blocks; data crosses block boundaries */
    int h3 = memcard_open(0, "THREE", true, 3, true);
    CHECK(h3 >= 0);
    static uint8_t big[3 * MC_BLOCK], bigback[3 * MC_BLOCK];
    for (size_t i = 0; i < sizeof big; i++) big[i] = (uint8_t)(i * 31 + (i >> 8));
    CHECK(memcard_write(h3, big, sizeof big) == (long)sizeof big);
    memcard_seek(h3, 0, 0);
    CHECK(memcard_read(h3, bigback, sizeof bigback) == (long)sizeof bigback && !memcmp(big, bigback, sizeof big));
    memcard_close(h3);
    CHECK(memcard_open(0, "TOOBIG", true, 12, true) == -1);      /* 4 blocks left of 15 */
    CHECK(memcard_open(0, "FOUR", true, 11, true) >= 0);

    /* directory walk */
    McDirEntry e;
    int cur = 0, n = 0;
    while (memcard_dir_next(0, "", &cur, &e)) n++;
    CHECK(n == 3);
    cur = 0;
    CHECK(memcard_dir_next(0, "BAS*", &cur, &e) && !strcmp(e.name, "BASLUS-00732DESTREGA") && e.size == MC_BLOCK);
    CHECK(!memcard_dir_next(0, "BAS*", &cur, &e));
    cur = 0;
    CHECK(memcard_dir_next(0, "T?REE", &cur, &e) && e.size == 3 * MC_BLOCK);

    /* erase frees the blocks again */
    CHECK(memcard_erase(0, "THREE"));
    CHECK(memcard_open(0, "THREE", false, 0, false) == -1);
    CHECK(memcard_open(0, "TOOBIG", true, 3, true) >= 0);

    /* directory sectors carry valid checksums */
    for (int s = 0; s <= 15; s++) {
        uint8_t sec[MC_SECTOR], x = 0;
        CHECK(memcard_read_sector(0, s, sec));
        for (int i = 0; i < 127; i++) x ^= sec[i];
        CHECK(x == sec[127]);
    }

    /* persistence: reload from disk, atomic temp file is gone */
    memcard_flush();
    CHECK(fsize("build/tests/mc_test.mcd.tmp") == -1);
    CHECK(memcard_attach(1, path) == NULL);       /* same image in slot 2 */
    cur = 0;
    CHECK(memcard_dir_next(1, "BAS*", &cur, &e));
    int hb = memcard_open(1, "BASLUS-00732DESTREGA", false, 0, false);
    CHECK(hb >= 0 && memcard_read(hb, back, 300) == 300 && !memcmp(buf, back, 300));
    CHECK(memcard_new_flag(1));

    /* write protect keeps the file untouched */
    memcard_set_write_protect(true);
    memcard_erase(0, "FOUR");
    memcard_flush();
    memcard_set_write_protect(false);
    cur = 0;
    CHECK(memcard_dir_next(1, "FOUR", &cur, &e));  /* slot 2 still sees the old disk image */

    /* format wipes; bad images are refused and left alone */
    memcard_format(0);
    cur = 0;
    CHECK(!memcard_dir_next(0, "", &cur, &e));
    FILE *f = fopen("build/tests/mc_bad.mcd", "wb");
    fwrite("nope", 1, 4, f);
    fclose(f);
    CHECK(memcard_attach(0, "build/tests/mc_bad.mcd") != NULL && !memcard_present(0));
    CHECK(fsize("build/tests/mc_bad.mcd") == 4);

    /* validate / export / swap helpers used by the File > Memory Card menu */
    CHECK(memcard_validate_file("build/tests/mc_bad.mcd") != NULL);
    CHECK(memcard_validate_file("build/tests/does_not_exist.mcd") != NULL);
    CHECK(memcard_attach(0, path) == NULL);
    memcard_format(0);
    int hs = memcard_open(0, "SAVE", true, 1, true);
    CHECK(hs >= 0);
    CHECK(memcard_write(hs, buf, 300) == 300);
    CHECK(memcard_export(0, "build/tests/mc_export.mcd") == NULL);        /* flushes first */
    CHECK(memcard_validate_file("build/tests/mc_export.mcd") == NULL);
    CHECK(memcard_attach(1, "build/tests/mc_export.mcd") == NULL);
    cur = 0;
    CHECK(memcard_dir_next(1, "SAVE", &cur, &e));
    CHECK(memcard_export(0, "build/tests/no_such_dir/x.mcd") != NULL);
    uint8_t blank[MC_SIZE];
    memcard_make_blank(blank);
    blank[1] = 'X';
    f = fopen("build/tests/mc_badhdr.mcd", "wb");
    fwrite(blank, 1, MC_SIZE, f);
    fclose(f);
    CHECK(memcard_validate_file("build/tests/mc_badhdr.mcd") != NULL);    /* 128 KiB without "MC" */
    blank[1] = 'C'; blank[5] ^= 1;
    f = fopen("build/tests/mc_badsum.mcd", "wb");
    fwrite(blank, 1, MC_SIZE, f);
    fclose(f);
    CHECK(memcard_validate_file("build/tests/mc_badsum.mcd") != NULL);
    /* swapping the card closes handles open on it and raises the changed flag */
    CHECK(memcard_attach(0, "build/tests/mc_export.mcd") == NULL);
    CHECK(memcard_read(hs, back, 1) == -1 && memcard_new_flag(0));
    CHECK(memcard_attach(0, NULL) == NULL && !memcard_present(0) && memcard_path(0) == NULL);
    CHECK(memcard_attach(0, path) == NULL && !strcmp(memcard_path(0), path));
    memcard_clear_new(0);
    memcard_mark_new(0);
    CHECK(memcard_new_flag(0));

    if (fails) { printf("memcard: %d FAILED\n", fails); return 1; }
    printf("memcard: all tests passed\n");
    return 0;
}
