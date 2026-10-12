/* debugcfg_test.c: game/port/debug_cfg.h, the settings file (debug.cfg), on the host.
 *   cc -O2 -Wall -Igame/port tools/port/debugcfg_test.c -o /tmp/debugcfg_test && /tmp/debugcfg_test <scratch dir>
 * Writes debug.cfg files into the scratch dir and checks what lands in the environment and in the log. */
#include <stdarg.h>
#include "debug_cfg.h"

static char sLog[8192];
static int sLines;
static void logf_(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const size_t n = strlen(sLog);
    vsnprintf(sLog + n, sizeof sLog - n, fmt, ap);
    va_end(ap);
    sLines++;
}
static int ok = 1;
static void check(int c, const char *what) {
    printf("%-72s %s\n", what, c ? "ok" : "FAIL");
    if (!c) { ok = 0; printf("--- log:\n%s---\n", sLog); }
}
static int env_is(const char *n, const char *v) { const char *e = getenv(n); return v ? e && !strcmp(e, v) : !e; }
static void reset(void) {
    sLog[0] = 0; sLines = 0;
    const char *names[] = { "PM64_HUDEXACT", "PM64_FIRECHEAP", "PM64_FIRENEAREST", "PM64_PRIO", "PM64_STRETCH", "PM64_DATA",
                            "PM64_SAMPLE_MS", "PM64_MAGBILIN", "PM64_LONG", "PM64_X", "PM64_SPACED", "GE_HUD1TO1", "pm64_hudexact" };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) unsetenv(names[i]);
}
static void write_file(const char *path, const char *text) { FILE *f = fopen(path, "wb"); fputs(text, f); fclose(f); }

int main(int argc, char **argv) {
    char path[600];
    snprintf(path, sizeof path, "%s/debug.cfg", argc > 1 ? argv[1] : ".");
    remove(path);

    reset();
    check(debug_cfg_load(path, logf_) == 0 && sLines == 0 && env_is("PM64_HUDEXACT", NULL), "no file: nothing set, no log line");

    reset();
    write_file(path, "# message windows\nPM64_HUDEXACT=1\n\nPM64_FIRECHEAP=0\r\n  PM64_FIRENEAREST = 0   # the N64 fire\n"
                     "PM64_STRETCH=\"0\"\n");
    int n = debug_cfg_load(path, logf_);
    check(n == 4, "4 good lines: 4 set");
    check(env_is("PM64_HUDEXACT", "1"), "PM64_HUDEXACT=1");
    check(env_is("PM64_FIRECHEAP", "0"), "CRLF line end dropped");
    check(env_is("PM64_FIRENEAREST", "0"), "spaces around name and value, trailing # note dropped");
    check(env_is("PM64_STRETCH", "0"), "quoted value unquoted");
    check(strstr(sLog, "debug.cfg: PM64_HUDEXACT=1\n") != NULL, "each one logged");

    reset();
    char longline[700];
    memset(longline, 'A', sizeof longline);
    memcpy(longline, "PM64_LONG=", 10);
    longline[sizeof longline - 2] = '\n'; longline[sizeof longline - 1] = 0;
    char text[2048];
    snprintf(text, sizeof text,
             "PM64_HUDEXACT\n"            /* no '=' */
             "GE_HUD1TO1=1\n"            /* another port's switch */
             "pm64_hudexact=1\n"          /* lower case */
             "HOME=/tmp\n"              /* not ours */
             "PM64_=1\n"                /* no name */
             "PM64_X Y=1\n"             /* a space inside the name */
             "PM64_DATA=/tmp\n"         /* the folder the file is in */
             "PM64_SAMPLE_MS=\n"         /* no value */
             "%s"                       /* over 510 characters */
             "PM64_MAGBILIN=1\n"        /* the good line after them all */
             "=1\n"
             "# PM64_PRIO=5\n",
             longline);
    write_file(path, text);
    n = debug_cfg_load(path, logf_);
    check(n == 1 && env_is("PM64_MAGBILIN", "1"), "bad lines ignored, the good one after them still set");
    check(env_is("PM64_HUDEXACT", NULL) && env_is("GE_HUD1TO1", NULL) && env_is("pm64_hudexact", NULL) && env_is("PM64_X", NULL)
          && env_is("PM64_DATA", NULL) && env_is("PM64_SAMPLE_MS", NULL) && env_is("PM64_LONG", NULL) && env_is("PM64_PRIO", NULL),
          "none of the bad lines reached the environment");
    check(strcmp(getenv("HOME") ? getenv("HOME") : "", "/tmp") != 0, "HOME untouched");
    check(sLines == 11, "one log line per ignored line (10) + the good one");

    reset();
    setenv("PM64_HUDEXACT", "0", 1);
    write_file(path, "PM64_HUDEXACT=1\nPM64_PRIO=0\n");
    n = debug_cfg_load(path, logf_);
    check(n == 1 && env_is("PM64_HUDEXACT", "0") && env_is("PM64_PRIO", "0"), "the environment wins over the file");
    check(strstr(sLog, "not applied") != NULL, "  and the log says so");

    reset();
    setenv("PM64_HUDEXACT", "1", 1);
    write_file(path, "PM64_HUDEXACT=1\n");
    n = debug_cfg_load(path, logf_);
    check(n == 0 && sLines == 0 && env_is("PM64_HUDEXACT", "1"), "the same value inherited (a relaunch): silent");

    reset();
    write_file(path, "");
    check(debug_cfg_load(path, logf_) == 0 && sLines == 0, "empty file: nothing");

    remove(path);
    printf("debugcfg_test: %s\n", ok ? "PASS" : "FAIL");
    return !ok;
}
