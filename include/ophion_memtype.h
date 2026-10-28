#pragma once
/* Intel MTRR overlap rules. Unknown combinations conservatively become UC. */
static inline unsigned oph_merge_memtype(unsigned a, unsigned b) {
    if (a == 255) return b;
    if (a == b) return a;
    if (a == 0 || b == 0) return 0;
    if ((a == 4 && b == 6) || (a == 6 && b == 4)) return 4;
    return 0;
}
