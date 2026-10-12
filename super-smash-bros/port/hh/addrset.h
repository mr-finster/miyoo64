/* addrset.h -- fixed-capacity open-addressing sets/maps keyed by address.
 *
 * Replaces the std::unordered_set / unordered_map bookkeeping in the
 * BattleShip reloc code. Storage is static (declared by the user with
 * ADDRSET_DEFINE), never malloc'd, so it can sit inside the save-state
 * region: a restored state brings the "already fixed up" marks back with
 * the data they describe.
 *
 *   ADDRSET_DEFINE(sFixups, 1 << 16);          // capacity must be a power of 2
 *   addrset_insert(&sFixups, key)  -> 1 if newly inserted, 0 if present, -1 if full
 *   addrset_contains(&sFixups, key)
 *   addrset_remove_range(&sFixups, lo, hi)     // drop keys in [lo, hi)
 *   addrset_clear(&sFixups)
 * Maps carry one u32 value per key (addrmap_*). Key 0 is reserved (empty).
 * Each set also keeps one bit per slot, set while the slot holds a key (occ), so a range
 * removal walks only the used slots (round 16: the full scans of seven sets on every file
 * load were 8% of a fight's ARM instructions). Anything that writes keys[] keeps it in step
 * (addrset_occ_set/clr); a save state that restores keys[] must restore occ[] with them. */
#ifndef HH_ADDRSET_H
#define HH_ADDRSET_H

#include <stdint.h>
#include <string.h>

typedef struct AddrSet {
    uint32_t cap;       /* power of two */
    uint32_t count;
    uintptr_t *keys;
    uint32_t *vals;     /* NULL for plain sets */
    const char *name;
    uint32_t *occ;      /* bit i set while keys[i] != 0 (capacity / 32 words) */
} AddrSet;

#define ADDRSET_DEFINE(var, capacity) \
    static uintptr_t var##_keys[capacity]; \
    static uint32_t var##_occ[(capacity) / 32]; \
    static AddrSet var = { (capacity), 0, var##_keys, NULL, #var, var##_occ }
#define ADDRMAP_DEFINE(var, capacity) \
    static uintptr_t var##_keys[capacity]; \
    static uint32_t var##_vals[capacity]; \
    static uint32_t var##_occ[(capacity) / 32]; \
    static AddrSet var = { (capacity), 0, var##_keys, var##_vals, #var, var##_occ }

static inline void addrset_occ_set(AddrSet *s, uint32_t i) { s->occ[i >> 5] |= 1u << (i & 31); }
static inline void addrset_occ_clr(AddrSet *s, uint32_t i) { s->occ[i >> 5] &= ~(1u << (i & 31)); }

void addrset_full_warning(AddrSet *s);

static inline uint32_t addrset_hash(uintptr_t k)
{
    uint32_t h = (uint32_t)k;
    h ^= h >> 16; h *= 0x7feb352dU; h ^= h >> 15; h *= 0x846ca68bU; h ^= h >> 16;
    return h;
}

static inline int addrset_find(const AddrSet *s, uintptr_t k, uint32_t *slot)
{
    uint32_t m = s->cap - 1, i = addrset_hash(k) & m;
    for (uint32_t n = 0; n < s->cap; n++, i = (i + 1) & m) {
        if (s->keys[i] == k) { *slot = i; return 1; }
        if (s->keys[i] == 0) { *slot = i; return 0; }
    }
    *slot = UINT32_MAX;
    return 0;
}

static inline int addrset_contains(const AddrSet *s, uintptr_t k)
{
    uint32_t slot;
    return k != 0 && addrset_find(s, k, &slot);
}

/* Returns 1 if inserted, 0 if already present, -1 if the table is full. */
static inline int addrset_insert(AddrSet *s, uintptr_t k)
{
    uint32_t slot;
    if (k == 0) return 0;
    if (addrset_find(s, k, &slot)) return 0;
    if (slot == UINT32_MAX || s->count + 1 > s->cap - s->cap / 8) {
        addrset_full_warning(s);
        return -1;
    }
    s->keys[slot] = k;
    addrset_occ_set(s, slot);
    if (s->vals) s->vals[slot] = 0;
    s->count++;
    return 1;
}

static inline void addrset_clear(AddrSet *s)
{
    memset(s->keys, 0, sizeof(uintptr_t) * s->cap);
    memset(s->occ, 0, sizeof(uint32_t) * (s->cap / 32));
    if (s->vals) memset(s->vals, 0, sizeof(uint32_t) * s->cap);
    s->count = 0;
}

/* Removes every key in [lo, hi) and rehashes the survivors: O(cap / 32 + used slots). */
void addrset_remove_range(AddrSet *s, uintptr_t lo, uintptr_t hi);

/* Map helpers: value 0 is returned for missing keys. */
static inline uint32_t addrmap_get(const AddrSet *s, uintptr_t k)
{
    uint32_t slot;
    return (k != 0 && addrset_find(s, k, &slot)) ? s->vals[slot] : 0;
}
static inline void addrmap_set(AddrSet *s, uintptr_t k, uint32_t v)
{
    uint32_t slot;
    if (k == 0) return;
    if (!addrset_find(s, k, &slot)) {
        if (addrset_insert(s, k) < 0) return;
        addrset_find(s, k, &slot);
    }
    s->vals[slot] = v;
}

#endif
