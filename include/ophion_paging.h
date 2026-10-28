/* Pure four-level x64 page walk. Reader must provide safe physical reads.
   This is NOT a public physical-memory API; kernel caller restricts VA to its MDL. */
#pragma once
#include <stdint.h>
typedef int (*OPH_READ_PTE)(void *, uint64_t, uint64_t *);
typedef struct OPH_TRANSLATION {
    uint64_t physical, page_size;
    uint32_t writable, executable, user;
} OPH_TRANSLATION;
static inline int oph_walk4(uint64_t cr3, uint64_t va, unsigned physical_bits,
    OPH_READ_PTE read_entry, void *context, OPH_TRANSLATION *out) {
    uint64_t upper = va >> 48, mask, table;
    unsigned shifts[4] = {39, 30, 21, 12};
    unsigned i;
    if (!out || !read_entry || physical_bits < 36 || physical_bits > 52 ||
        upper != (((va >> 47) & 1) ? 0xffffULL : 0)) return 0;
    mask = ((1ULL << physical_bits) - 1) & ~0xfffULL;
    table = cr3 & mask;
    out->writable = out->executable = out->user = 1;
    for (i = 0; i < 4; ++i) {
        uint64_t entry, size, base;
        if (!read_entry(context, table + ((va >> shifts[i]) & 511) * 8, &entry) ||
            !(entry & 1) || (entry & 0x000ffffffffff000ULL & ~mask)) return 0;
        out->writable &= (uint32_t)((entry >> 1) & 1);
        out->user &= (uint32_t)((entry >> 2) & 1);
        out->executable &= (uint32_t)(!(entry >> 63));
        if (i == 0 && (entry & 0x80)) return 0;
        if (i == 3 || (entry & 0x80)) {
            size = 1ULL << shifts[i];
            /* Large-page bit 12 is PAT; other address bits below size are reserved. */
            if (i != 3 && (entry & mask & (size - 1) & ~0x1000ULL)) return 0;
            base = entry & mask & ~(size - 1);
            out->physical = base | (va & (size - 1));
            out->page_size = size;
            return 1;
        }
        table = entry & mask;
    }
    return 0;
}
