/* MENU > OPTIONS' file part (platform/miyoo_options.c, built with MIYOO_OPTIONS_TEST: no SDL): debug.cfg
 * rewritten with only the changed keys, every other line kept; the atomic write; the start-up loader. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "miyoo_options.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static char sDir[64];
const char *port_save_dir(void) { return sDir; }

const struct miyoo_option miyoo_options[] = {
    { .label = "SECTION" },
    { .label = "A", .key = "fineaim", MIYOO_OPT_ONOFF, .def = "1", .when = MIYOO_OPT_NOW },
    { .label = "B", .key = "fineaimpct", MIYOO_OPT_VALUES({ "10", "10%" }, { "20", "20%" }), .def = "20" },
    { .label = "C", .key = "hudfilter", .env = "OTHER_HUDFILTER", MIYOO_OPT_ONOFF, .def = "1" },
};
const int miyoo_options_count = (int)(sizeof(miyoo_options) / sizeof(miyoo_options[0]));

static void rewrite_is(const char *in, const char *const *k, const char *const *v, int n, const char *want) {
    char *out = miyoo_options_rewrite(in, k, v, n);
    CHECK(out != NULL);
    if (out && strcmp(out, want) != 0) { printf("FAIL: rewrite of [%s]\n got [%s]\nwant [%s]\n", in, out, want); fails++; }
    free(out);
}

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    static char buf[4096];
    if (!f) return NULL;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    return buf;
}

int main(void) {
    const char *k1[] = { "fineaim" }, *v0[] = { "0" };
    /* a line rewritten; the comment, the look-alike keys, a blank line, a CR LF line and the last line without a
     * newline kept as they were */
    rewrite_is("# fineaim=1 in a comment\nfineaimpct=33\nfineaim=1\n\npacecarry=17\r\nlogverbose=0",
               k1, v0, 1, "# fineaim=1 in a comment\nfineaimpct=33\nfineaim=0\n\npacecarry=17\r\nlogverbose=0");
    /* a CR LF line keeps its CR LF; the last line without a newline stays without one; every copy of the key */
    rewrite_is("fineaim=1\r\nx=1\nfineaim=1", k1, v0, 1, "fineaim=0\r\nx=1\nfineaim=0");
    /* missing: appended, after a newline the file lacked */
    rewrite_is("x=1", k1, v0, 1, "x=1\nfineaim=0\n");
    rewrite_is("", k1, v0, 1, "fineaim=0\n");
    {   /* two keys, one there, one not */
        const char *k[] = { "fineaim", "hudfilter" }, *v[] = { "0", "1" };
        rewrite_is("hudfilter=0\n#c\n", k, v, 2, "hudfilter=1\n#c\nfineaim=0\n");
    }
    /* "fineaim" never matches "fineaimpct=", nor " fineaim=" (the games read the key as written) */
    rewrite_is("fineaimpct=20\n fineaim=1\n", k1, v0, 1, "fineaimpct=20\n fineaim=1\nfineaim=0\n");

    /* the file: written through path.new, nothing left over; no change, no write */
    snprintf(sDir, sizeof(sDir), "/tmp/options_test_%d", (int)getpid());
    char path[100], tmp[110];
    snprintf(path, sizeof(path), "%s/debug.cfg", sDir);
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    char cmd[200];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", sDir, sDir);
    CHECK(system(cmd) == 0);
    CHECK(miyoo_options_write_file(path, k1, v0, 1) == 0); /* no file yet: made */
    CHECK(slurp(path) && strcmp(slurp(path), "fineaim=0\n") == 0);
    FILE *f = fopen(path, "w");
    fputs("# mine\nfineaimpct=10\nfineaim=1\nhudfilter=0\n", f);
    fclose(f);
    CHECK(miyoo_options_write_file(path, k1, v0, 1) == 0);
    CHECK(strcmp(slurp(path), "# mine\nfineaimpct=10\nfineaim=0\nhudfilter=0\n") == 0);
    CHECK(access(tmp, F_OK) != 0);
    /* nothing to change: the file is not rewritten (its inode stays) */
    {
        struct stat a, b;
        CHECK(stat(path, &a) == 0);
        CHECK(miyoo_options_write_file(path, k1, v0, 1) == 0);
        CHECK(stat(path, &b) == 0);
        CHECK(a.st_ino == b.st_ino);
    }

    /* the loader: each row's line as its variable (the game's prefix, or the row's env), later lines winning,
     * keys not in the table left alone, the file winning over the environment */
    unsetenv("TEST_FINEAIM"); unsetenv("TEST_FINEAIMPCT"); unsetenv("OTHER_HUDFILTER"); unsetenv("TEST_PACECARRY");
    setenv("TEST_FINEAIMPCT", "50", 1);
    f = fopen(path, "w");
    fputs("fineaim=1\r\nfineaimpct=10\npacecarry=3\nhudfilter=0\nfineaim=0", f);
    fclose(f);
    CHECK(miyoo_options_load() == 4);
    CHECK(getenv("TEST_FINEAIM") && strcmp(getenv("TEST_FINEAIM"), "0") == 0);
    CHECK(getenv("TEST_FINEAIMPCT") && strcmp(getenv("TEST_FINEAIMPCT"), "10") == 0);
    CHECK(getenv("OTHER_HUDFILTER") && strcmp(getenv("OTHER_HUDFILTER"), "0") == 0);
    CHECK(getenv("TEST_PACECARRY") == NULL);

    snprintf(cmd, sizeof(cmd), "rm -rf %s", sDir);
    CHECK(system(cmd) == 0);
    printf("options: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
