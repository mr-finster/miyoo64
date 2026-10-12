#!/bin/sh
# Host test of debug.cfg's parser (port/hh/hh_card.c hh_card_cfg_key + hh_card_cfg, compiled as they
# are from the source): the GE_NAME=value form and the short lowercase form (rspyield=0 = GE_RSPYIELD=0)
# both set the variable; comments, other names and malformed lines are skipped.
# Usage: sh tools/cfg_test.sh   (needs a host cc; no ROM, no SDK)
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
SRC=$ROOT/port/hh/hh_card.c
{
    printf '#include <ctype.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n'
    sed -n '/^static const char \*hh_card_cfg_key(/,/^}/p' "$SRC"
    sed -n '/^static void hh_card_cfg(/,/^}/p' "$SRC"
    cat <<'EOF'
static int bad;
static void want(const char *k, const char *v)
{
    const char *g = getenv(k);
    if (v == NULL ? g != NULL : (g == NULL || strcmp(g, v) != 0)) {
        printf("FAIL %s: got %s, want %s\n", k, g ? g : "(unset)", v ? v : "(unset)");
        bad = 1;
    }
}
int main(int argc, char **argv)
{
    (void)argc;
    hh_card_cfg(argv[1]);
    want("GE_RSPYIELD", "0");     /* short form */
    want("GE_INTRES", "85");      /* old form */
    want("GE_ARGS", "-level_33"); /* short form, quoted value */
    want("GE_HALF_SKY2", "2");    /* digits and '_' in a short key */
    want("GE_2DBILIN", "0");      /* a short key starting with a digit */
    want("GE_FOO", NULL);         /* commented out */
    want("GE_MIXED", NULL);       /* Mixed case: not a short key */
    want("GE_", NULL);            /* "=1": no key */
    want("GE_BAD-KEY", NULL);     /* '-' in a short key */
    want("GE_12", NULL);          /* no letter: not a name */
    want("RSPYIELD", NULL);       /* never set without the prefix */
    printf(bad ? "cfg_test: FAILED\n" : "cfg_test: ok\n");
    return bad;
}
EOF
} > "$T/t.c"
cat > "$T/debug.cfg" <<'EOF'
# a comment
rspyield=0
GE_INTRES=85
args="-level_33"
half_sky2=2
2dbilin=0
12=1
#foo=1
Mixed=1
=1
bad-key=1
EOF
cc -O1 -Wall -Werror -o "$T/t" "$T/t.c"
rc=0
"$T/t" "$T" > "$T/out" || rc=$?
grep -v '^\[card\]' "$T/out"
exit $rc
