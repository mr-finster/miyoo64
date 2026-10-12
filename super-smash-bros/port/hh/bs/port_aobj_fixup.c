/* From BattleShip port/port_aobj_fixup.cpp (MIT, see port/NOTICE.md), converted to C.
 *
 * port_aobj_fixup.c — see port_aobj_fixup.h for architecture notes.
 *
 * The opcode size table below mirrors the decomp parser's switch in
 * src/sys/objanim.c — gcParseDObjAnimJoint (opcodes 0-17) and
 * gcParseMObjMatAnimJoint (opcodes 0-14, 18-22).  Extending the parser
 * with a new opcode means extending this walker too.
 *
 * handheld port notes (ILP32): Jump/SetAnim/SetInterp slots hold the real
 * 32-bit pointer (BattleShip's token is the identity here). All bookkeeping
 * is in fixed-capacity static tables so it is captured by save states.
 */

#include "port_aobj_fixup.h"
#include "port_log.h"
#include "addrset.h"

#include <stdbool.h>
#include <stdint.h>

ADDRSET_DEFINE(sUnswappedHeads, 1 << 14);
ADDRSET_DEFINE(sRejectedHeads, 1 << 14);  /* walked once, didn't look like EVENT32 — don't retry */

/* Registered fighter-figatree file ranges (base, end).  The walker only
 * operates on head pointers that fall within one of these — this guards
 * against a stream from a non-halfswapped file being un-halfswapped,
 * which would silently corrupt it. */
typedef struct Range { uintptr_t base; uintptr_t end; } Range;
#define HALFSWAPPED_RANGE_CAP 128
static Range sHalfswappedRanges[HALFSWAPPED_RANGE_CAP];
static uint32_t sHalfswappedRangeCount;

/* Visited set for one-shot idempotent in-place fixups (e.g. interp.c
 * spline-data un-halfswap).  Cleared per-range when
 * port_aobj_register_halfswapped_range adds a new range so that
 * figatree-heap reloads at the same address get re-fixed. */
ADDRSET_DEFINE(sUnhalfswappedVisited, 1 << 13);

static bool is_in_halfswapped_range(const void *p)
{
    uintptr_t addr = (uintptr_t)p;
    for (uint32_t i = 0; i < sHalfswappedRangeCount; i++) {
        if (addr >= sHalfswappedRanges[i].base && addr < sHalfswappedRanges[i].end)
            return true;
    }
    return false;
}

/* Safety cap per stream: long-form figatree animations rarely exceed a
 * few hundred events.  65k u32s = 256 KB of stream data — far beyond
 * any realistic authored animation and well short of "walk forever". */
#define STREAM_STEP_LIMIT 65536

static inline uint32_t unhalfswap(uint32_t v)
{
    return (v << 16) | (v >> 16);
}

static inline uint32_t cmd_opcode(uint32_t cmd)
{
    /* AObjEvent32.command layout matches src/sys/objtypes.h LE variant:
     * payload:15 (bits 0..14), flags:10 (bits 15..24), opcode:7 (25..31). */
    return (cmd >> 25) & 0x7F;
}

static inline uint32_t cmd_flags(uint32_t cmd)
{
    return (cmd >> 15) & 0x3FF;
}

static inline int popcount_bits(uint32_t v, int first_bit, int count)
{
    int c = 0;
    for (int i = 0; i < count; i++) {
        if (v & (1u << (first_bit + i))) c++;
    }
    return c;
}

/* Walker uses a two-phase approach: first phase validates the stream by
 * simulating the walk without modifying data (it computes the
 * un-halfswapped u32 and reads the opcode from it but doesn't write
 * anywhere).  It records the set of u32 addresses that WOULD be
 * modified.  If the simulation completes — hits a proper terminator
 * (End, Jump, SetAnim) with only valid opcodes — the second phase
 * applies the halfswap fix to every collected address.  If the
 * simulation hits an unknown opcode or runs away, the stream is added
 * to sRejectedHeads and left untouched.
 *
 * This protects streams that aren't truly EVENT32 from silent
 * corruption: gcParseDObjAnimJoint gets called with some pointers
 * that point to EVENT16 data or non-figatree data (because the same
 * call site handles multiple animation types).
 *
 * handheld: the scan context is static scratch (only one scan runs at a
 * time; walk() runs its three scans back to back). Running out of scratch
 * rejects the stream, which leaves it untouched (the safe direction). */

#define SCAN_PENDING_CAP 32768   /* u32 slots to un-halfswap per tree */
#define SCAN_VISITED_CAP 256     /* distinct stream heads per tree */

typedef struct ScanCtx {
    uint32_t *pending[SCAN_PENDING_CAP];  /* populated only when unhalfswap=true */
    uint32_t pending_count;
    uintptr_t visited[SCAN_VISITED_CAP];
    uint32_t visited_count;
    int total_steps;
    bool unhalfswap;       /* interpret each command as unhalfswap(raw); collect pending addresses */
    bool follow_recurse;   /* follow Jump/SetAnim pointers into sub-streams */
} ScanCtx;

static ScanCtx sScanCtx;

static void ctx_reset(ScanCtx *ctx, bool unhalfswap_mode, bool follow_recurse)
{
    ctx->pending_count = 0;
    ctx->visited_count = 0;
    ctx->total_steps = 0;
    ctx->unhalfswap = unhalfswap_mode;
    ctx->follow_recurse = follow_recurse;
}

static bool ctx_push_pending(ScanCtx *ctx, uint32_t *p)
{
    if (ctx->pending_count >= SCAN_PENDING_CAP)
        return false;
    ctx->pending[ctx->pending_count++] = p;
    return true;
}

/* Returns 1 if newly inserted, 0 if already present, -1 if full. */
static int ctx_visit(ScanCtx *ctx, uintptr_t key)
{
    for (uint32_t i = 0; i < ctx->visited_count; i++) {
        if (ctx->visited[i] == key) return 0;
    }
    if (ctx->visited_count >= SCAN_VISITED_CAP) return -1;
    ctx->visited[ctx->visited_count++] = key;
    return 1;
}

static bool scan(uint32_t *head, ScanCtx *ctx);

static bool scan_recurse(uint32_t slot, ScanCtx *ctx)
{
    if (!ctx->follow_recurse) return true;
    if (slot == 0) return true;
    /* ILP32: the slot holds the target pointer. A bad value is harmless
     * here — scan() ignores anything outside a registered range. */
    return scan((uint32_t *)(uintptr_t)slot, ctx);
}

static bool scan(uint32_t *head, ScanCtx *ctx)
{
    if (head == NULL) return true;
    if (!is_in_halfswapped_range(head)) return true; /* not our data; skip silently */
    uintptr_t key = (uintptr_t)head;
    if (addrset_contains(&sUnswappedHeads, key)) return true;
    if (addrset_contains(&sRejectedHeads, key)) return false;
    int v = ctx_visit(ctx, key);
    if (v == 0) return true;   /* already scanning this head */
    if (v < 0) return false;   /* scratch exhausted — reject */

    uint32_t *ev = head;

    while (true) {
        if (++ctx->total_steps > STREAM_STEP_LIMIT) {
            return false;
        }

        uint32_t cmd_raw = *ev;
        uint32_t cmd = ctx->unhalfswap ? unhalfswap(cmd_raw) : cmd_raw;
        if (ctx->unhalfswap && !ctx_push_pending(ctx, ev)) return false;
        ev++;

        uint32_t opcode = cmd_opcode(cmd);
        uint32_t flags_in_cmd = cmd_flags(cmd);

        switch (opcode) {
        case 0:  /* End */
            return true;

        case 1:  /* Jump — [pointer] */
        case 14: /* SetAnim — [pointer] */
            if (!scan_recurse(*ev, ctx)) return false;
            ev++;
            return true;

        case 2:  /* Wait */
        case 15: /* SetFlags */
        case 16: /* ANIM_CMD_16 */
            break;

        case 13: /* SetInterp — [pointer] (no recurse) */
            ev++;
            break;

        /* Flags come from the command word itself (cmd.flags) — the
         * decomp reads them via AObjAnimAdvance (post-increment) so
         * what looks like "advance past header to get flags from next"
         * is actually just reading the just-consumed command's flags.
         * All of the opcodes below consume the command plus zero or
         * more f32 payload slots. */

        case 3: case 4: /* SetValBlock, SetVal */
        case 7:         /* SetTargetRate */
        case 8: case 9: /* SetVal0RateBlock, SetVal0Rate */
        case 10: case 11: /* SetValAfterBlock, SetValAfter */
        case 17:        /* ANIM_CMD_17 */
        case 18: case 19: case 20: case 21: /* SetExtVal* (MObj) */
        {
            int n = popcount_bits(flags_in_cmd, 0, 10);
            if (n > 10) return false;
            for (int k = 0; k < n; k++) {
                if (ctx->unhalfswap && !ctx_push_pending(ctx, ev)) return false;
                ev++;
            }
            break;
        }

        case 5: case 6: /* SetValRate(Block) — value+rate pairs */
        {
            int n = popcount_bits(flags_in_cmd, 0, 10);
            if (n > 10) return false;
            for (int k = 0; k < 2 * n; k++) {
                if (ctx->unhalfswap && !ctx_push_pending(ctx, ev)) return false;
                ev++;
            }
            break;
        }

        case 12: /* ANIM_CMD_12 — no f values, flags-only effect */
            break;

        case 22: /* ANIM_CMD_22 (MObj) — N u32 from flag bits [0..4] */
        {
            int n = popcount_bits(flags_in_cmd, 0, 5);
            if (n > 5) return false;
            for (int k = 0; k < n; k++) {
                if (ctx->unhalfswap && !ctx_push_pending(ctx, ev)) return false;
                ev++;
            }
            break;
        }

        default:
            return false; /* invalid opcode — reject the whole stream */
        }
    }
}

static void walk(uint32_t *head)
{
    if (head == NULL) return;
    if (!is_in_halfswapped_range(head)) return;
    uintptr_t key = (uintptr_t)head;
    if (addrset_contains(&sUnswappedHeads, key)) return;
    if (addrset_contains(&sRejectedHeads, key)) return;

    ScanCtx *ctx = &sScanCtx;

    /* Phase 1: measure main-stream length under the un-halfswap
     * interpretation (no recursion yet — we just need the count for
     * the length comparison in phase 2).  If the un-halfswap scan
     * can't even validate the main stream, the stream isn't
     * halfswap-corrupted EVENT32 — reject. */
    ctx_reset(ctx, true, false);
    bool unswap_valid = scan(head, ctx);

    if (!unswap_valid) {
        addrset_insert(&sRejectedHeads, key);
        return;
    }

    /* Phase 2: measure main-stream length under the raw (halfswap-
     * applied) interpretation.  If raw validates AND walks >= 2
     * commands, the stream is genuinely ambiguous and we leave it
     * alone (sibling fixup passes may have rewritten it to native
     * form).
     *
     * The length threshold is the key heuristic.  A halfswap-
     * corrupted stream's first u32 often has bits [31:25] = 0 when
     * the original upper 16 bits held a non-End opcode: halfswap
     * moves the original opcode bits to [15:9] and places the
     * original low payload bits into [31:16], which are frequently
     * zero.  Bits [31:25] then read as opcode 0 (End) and the raw
     * scan validates in a single step.  A genuine native stream
     * carries real animation commands (SetVal, Wait, etc.) before
     * reaching End, so a raw scan of length >= 2 is a strong signal
     * that the stream is actually native; length 1 is almost always
     * a halfswap-coincidence false positive.
     *
     * Recursion is disabled for both comparison scans because
     * Jump/SetAnim targets may have been un-halfswapped by an earlier
     * walk and so aren't in raw form anymore. */
    ctx_reset(ctx, false, false);
    bool raw_valid = scan(head, ctx);

    if (raw_valid && ctx->total_steps >= 2) {
        addrset_insert(&sRejectedHeads, key);
        return;
    }

    /* Un-halfswap wins.  Do the full scan WITH recursion so we
     * collect the complete pending slot list across Jump/SetAnim
     * sub-streams. */
    ctx_reset(ctx, true, true);
    if (!scan(head, ctx)) {
        /* Main stream validated but a sub-stream failed.  Don't
         * partially un-halfswap; reject the whole tree. */
        addrset_insert(&sRejectedHeads, key);
        return;
    }

    for (uint32_t i = 0; i < ctx->pending_count; i++) {
        uint32_t *p = ctx->pending[i];
        *p = unhalfswap(*p);
    }
    for (uint32_t i = 0; i < ctx->visited_count; i++) {
        addrset_insert(&sUnswappedHeads, ctx->visited[i]);
    }
}

void port_aobj_event32_unhalfswap_stream(void *head)
{
    walk((uint32_t *)head);
}

void port_aobj_register_halfswapped_range(void *base, unsigned long size)
{
    if (base == NULL || size == 0) return;
    uintptr_t b = (uintptr_t)base;
    uintptr_t e = b + (uintptr_t)size;
    /* Evict every cache keyed on an address inside the new range —
     * fighter figatree heaps are bump-reset between status changes, so a
     * later load lands fresh halfswap-corrupted bytes at addresses a
     * previous load already populated.  Any cached "already processed"
     * verdict from the previous load is stale once the bytes change.
     *
     * Three caches need eviction in lockstep, all keyed on raw heap
     * pointer:
     *   - sUnhalfswappedVisited: per-block one-shot fixup tracker (used
     *     by interp.c spline data).
     *   - sUnswappedHeads: event32 streams the walker successfully fixed
     *     up.  Without eviction, the walker short-circuits on a fresh
     *     load's stream that happens to land at the same address.
     *   - sRejectedHeads: event32 streams the walker decided not to
     *     touch.  Without eviction, a stream that was non-event32 in the
     *     previous load (correctly rejected) blocks a real event32 stream
     *     in the new load from being un-halfswapped.
     *
     * Symptom that surfaced the missing two (BattleShip): Master Hand's
     * okutsubushi / okupunch / drill animations parsed garbage —
     * gcParseDObjAnimJoint would hit a halfswap-corrupted command word
     * with an out-of-range opcode (typically 64) and bail, leaving
     * TransN.translate at (0,0,0) for the rest of the attack.  Cache keyed
     * on pointer, heap reuses addresses, fresh bytes skip fixup. */
    addrset_remove_range(&sUnhalfswappedVisited, b, e);
    addrset_remove_range(&sUnswappedHeads, b, e);
    addrset_remove_range(&sRejectedHeads, b, e);
    /* Heaps are bump-reset and reloaded at the same base across status
     * changes, so the same {b,e} arrives repeatedly — update in place
     * instead of accumulating duplicates (the list is linearly scanned on
     * the per-joint hot path). */
    for (uint32_t i = 0; i < sHalfswappedRangeCount; i++) {
        if (sHalfswappedRanges[i].base == b) {
            sHalfswappedRanges[i].end = e;
            return;
        }
    }
    if (sHalfswappedRangeCount >= HALFSWAPPED_RANGE_CAP) {
        /* handheld: fixed capacity. Ranges overlapping the new one describe
         * memory that has just been overwritten by this load, so they are
         * dead — drop them to make room. */
        uint32_t out = 0;
        for (uint32_t i = 0; i < sHalfswappedRangeCount; i++) {
            Range r = sHalfswappedRanges[i];
            if (r.base < e && r.end > b) continue;
            sHalfswappedRanges[out++] = r;
        }
        sHalfswappedRangeCount = out;
        if (sHalfswappedRangeCount >= HALFSWAPPED_RANGE_CAP) {
            port_log("SSB64: halfswapped range table full (%u); figatree at %p not "
                     "registered — raise HALFSWAPPED_RANGE_CAP\n",
                     (unsigned)HALFSWAPPED_RANGE_CAP, base);
            return;
        }
    }
    sHalfswappedRanges[sHalfswappedRangeCount].base = b;
    sHalfswappedRanges[sHalfswappedRangeCount].end = e;
    sHalfswappedRangeCount++;
}

int port_aobj_unhalfswap_visit(const void *p)
{
    if (p == NULL) return 1;
    /* insert() returns 1 only when newly recorded (a full table returns -1,
     * which reports "visited" so the fixup is skipped rather than repeated). */
    return addrset_insert(&sUnhalfswappedVisited, (uintptr_t)p) == 1 ? 0 : 1;
}

void port_aobj_event32_unhalfswap_reset(void)
{
    addrset_clear(&sUnswappedHeads);
    addrset_clear(&sRejectedHeads);
    sHalfswappedRangeCount = 0;
    addrset_clear(&sUnhalfswappedVisited);
}

int port_aobj_is_in_halfswapped_range(const void *p)
{
    return is_in_halfswapped_range(p) ? 1 : 0;
}
