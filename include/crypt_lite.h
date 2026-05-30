#ifndef CRYPT_LITE_H
#define CRYPT_LITE_H

/* --------------------------------------------------------------------------
 * crypt-lite: a tiny, self-contained password hash shared verbatim by the
 * kernel (to seed /etc/shadow at boot) and by libc (login/su/passwd).  It is
 * NOT cryptographically secure - it exists so the multi-user machinery has a
 * single, identical hashing routine on both sides of the syscall boundary.
 *
 * Output format mirrors crypt(3)'s "$id$salt$hash" convention:
 *     $L$<salt>$<16 hex digits>
 * crypt_lite() accepts either a bare salt or a previously produced hash string
 * as `setting`, extracting the salt either way, so verification is simply:
 *     crypt_lite(typed, stored, buf, n); strcmp(buf, stored) == 0
 * -------------------------------------------------------------------------- */

#include <stdint.h>
#include <stddef.h>

static inline uint64_t cl_mix(uint64_t h, const char *s) {
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 0x100000001b3ULL;
        h ^= h >> 29;
    }
    return h;
}

static inline void crypt_lite(const char *key, const char *setting,
                              char *out, size_t outsz) {
    char salt[9];
    size_t si = 0;
    const char *p = setting ? setting : "";
    if (p[0] == '$' && p[1] == 'L' && p[2] == '$') p += 3;
    while (*p && *p != '$' && si < 8) salt[si++] = *p++;
    salt[si] = '\0';

    uint64_t h = 1469598103934665603ULL;
    h = cl_mix(h, salt);
    h = cl_mix(h, key ? key : "");
    h = cl_mix(h, salt);
    for (int i = 0; i < 1000; i++) {
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 33;
    }

    static const char hex[] = "0123456789abcdef";
    size_t o = 0;
    const char *pre = "$L$";
    for (const char *q = pre; *q && o + 1 < outsz;) out[o++] = *q++;
    for (size_t i = 0; i < si && o + 1 < outsz; i++) out[o++] = salt[i];
    if (o + 1 < outsz) out[o++] = '$';
    for (int i = 15; i >= 0 && o + 1 < outsz; i--)
        out[o++] = hex[(h >> (i * 4)) & 0xf];
    if (o < outsz) out[o] = '\0';
    else if (outsz) out[outsz - 1] = '\0';
}

#endif /* CRYPT_LITE_H */
