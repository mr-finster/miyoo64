/* addrset.c -- out-of-line parts of addrset.h. */
#include "addrset.h"
#include "port_log.h"

void addrset_full_warning(AddrSet *s)
{
    static const AddrSet *warned[16];
    for (int i = 0; i < 16; i++) {
        if (warned[i] == s) return;
        if (warned[i] == NULL) { warned[i] = s; break; }
    }
    port_log("SSB64: address set %s is full (%u entries); raise its capacity\n",
             s->name, (unsigned)s->cap);
}

/* Both passes walk the used slots through occ[], skipping 32 empty slots a word (round 16):
 * every file load calls this for seven sets of 8k-128k slots holding ~11k keys in all, and
 * the full scans were 8% of a fight's ARM instructions. The re-seat visits the same slots in
 * the same order as the full scan did (the next used slot at or after i, read as it stands),
 * so the table ends up exactly as before. */
void addrset_remove_range(AddrSet *s, uintptr_t lo, uintptr_t hi)
{
    uint32_t removed = 0;
    const uint32_t words = s->cap / 32;
    if (s->count == 0 || lo >= hi) return;
    for (uint32_t w = 0; w < words; w++) {
        uint32_t bits = s->occ[w];
        while (bits != 0) {
            const uint32_t b = (uint32_t)__builtin_ctz(bits), i = w * 32 + b;
            bits &= bits - 1;
            if (s->keys[i] >= lo && s->keys[i] < hi) {
                s->keys[i] = 0;
                s->occ[w] &= ~(1u << b);
                removed++;
            }
        }
    }
    if (removed == 0) return;
    s->count -= removed;
    /* Linear probing: re-seat every survivor so no probe chain is broken. */
    for (uint32_t i = 0; i < s->cap; ) {
        const uint32_t w = i >> 5, bits = s->occ[w] & (~0u << (i & 31));
        if (bits == 0) { i = (w + 1) << 5; continue; }
        i = (w << 5) + (uint32_t)__builtin_ctz(bits);
        uintptr_t k = s->keys[i];
        uint32_t v = s->vals ? s->vals[i] : 0;
        s->keys[i] = 0;
        addrset_occ_clr(s, i);
        uint32_t slot;
        addrset_find(s, k, &slot);
        s->keys[slot] = k;
        addrset_occ_set(s, slot);
        if (s->vals) s->vals[slot] = v;
        i++;
    }
}
