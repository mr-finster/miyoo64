/* debug_cfg.h: the settings file, debug.cfg in the data dir (the data folder's  on the card; the desktop proxy's working
 * directory; PM64_DATA moves both), GoldenEye's form (goldeneye-007 port/hh/hh_card.c). One NAME=value per line;
 * read first thing in main (port_host.c), so every switch the port reads with getenv sees it.
 *   - NAME must be a PM64_ switch: PM64_ then capitals, digits and '_'. Anything else is ignored, with a log line.
 *   - '#' starts a comment line; blank lines are skipped; spaces around the name and the value and a trailing
 *     " # note" are dropped; a value in double quotes loses them; CRLF line ends are fine.
 *   - PM64_DATA is ignored (the file lives in that folder). An empty value is ignored.
 *   - A switch already set in the environment keeps its value (tests and the desktop proxy set them that way; the
 *     relaunch inherits the first start's, the same values, so it logs nothing).
 *   - No file: nothing happens, no log line (the port as before the file existed).
 * Header only, so tools/port/debugcfg_test.c tests the same code. Returns how many switches it set. */
#ifndef DEBUG_CFG_H
#define DEBUG_CFG_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*DebugCfgLog)(const char *fmt, ...);

static int debug_cfg_name_ok(const char *n) {
    if (strncmp(n, "PM64_", 5) != 0 || !n[5]) return 0;
    for (const char *c = n + 5; *c; c++)
        if (!((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_')) return 0;
    return 1;
}

static int debug_cfg_load(const char *path, DebugCfgLog log) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[512];
    int set = 0, no = 0;
    while (fgets(line, sizeof line, f)) {
        no++;
        size_t len = strlen(line);
        if (len == sizeof line - 1 && line[len - 1] != '\n') { /* longer than the buffer: skip the rest of it */
            int ch;
            while ((ch = fgetc(f)) != EOF && ch != '\n') {}
            log("debug.cfg: line %d ignored (longer than %d characters)\n", no, (int) sizeof line - 2);
            continue;
        }
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        char *end = s + strlen(s);
        while (end > s && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
        if (!*s || *s == '#') continue;
        char *eq = strchr(s, '=');
        if (!eq) { log("debug.cfg: line %d ignored (not NAME=value): %s\n", no, s); continue; }
        char *ne = eq;
        while (ne > s && (ne[-1] == ' ' || ne[-1] == '\t')) ne--;
        *ne = 0;
        char *v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        for (char *h = v; (h = strchr(h, '#')) != NULL; h++) /* a trailing " # note" */
            if (h > v && (h[-1] == ' ' || h[-1] == '\t')) { *h = 0; break; }
        end = v + strlen(v);
        while (end > v && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
        if (end - v >= 2 && v[0] == '"' && end[-1] == '"') { end[-1] = 0; v++; }
        if (!debug_cfg_name_ok(s)) { log("debug.cfg: line %d ignored (not a PM64_ switch): %s\n", no, s); continue; }
        if (!strcmp(s, "PM64_DATA")) { log("debug.cfg: line %d ignored (PM64_DATA cannot be set here)\n", no); continue; }
        if (!*v) { log("debug.cfg: line %d ignored (no value): %s\n", no, s); continue; }
        const char *was = getenv(s);
        if (was && !strcmp(was, v)) continue; /* a relaunch inherits what the first start set */
        if (was) { log("debug.cfg: %s=%s not applied: the environment has %s=%s\n", s, v, s, was); continue; }
        setenv(s, v, 0);
        log("debug.cfg: %s=%s\n", s, v);
        set++;
    }
    fclose(f);
    return set;
}
#endif
