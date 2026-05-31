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

/* --------------------------------------------------------------------------
 * v1 (legacy): 64-bit digest, "$L$<salt>$<16 hex>".  Retained only so shadow
 * entries written by older builds still verify.  New hashes use v2.
 * -------------------------------------------------------------------------- */
static inline void crypt_lite_v1(const char *key, const char *setting,
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

/* --------------------------------------------------------------------------
 * v2: a wider (256-bit) salted, heavily-iterated digest, "$L2$<salt>$<64 hex>".
 * Four mixing lanes are stretched over 4096 rounds with cross-lane diffusion
 * and salt+key re-injection.  Still not a vetted KDF, but materially stronger
 * than v1 (16x the output width and ~4x the work).
 * -------------------------------------------------------------------------- */
static inline void crypt_lite_v2(const char *key, const char *setting,
                                 char *out, size_t outsz) {
    char salt[17];
    size_t si = 0;
    const char *p = setting ? setting : "";
    if (p[0] == '$' && p[1] == 'L' && p[2] == '2' && p[3] == '$') p += 4;
    else if (p[0] == '$' && p[1] == 'L' && p[2] == '$') p += 3;
    while (*p && *p != '$' && si < 16) salt[si++] = *p++;
    salt[si] = '\0';

    uint64_t s[4] = {
        0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
        0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
    };
    s[0] = cl_mix(s[0], salt);
    s[1] = cl_mix(s[1], key ? key : "");
    s[2] = cl_mix(s[2], salt);
    s[3] = cl_mix(s[3], key ? key : "");
    for (int i = 0; i < 4096; i++) {
        s[0] ^= s[3] >> 17; s[0] *= 0xff51afd7ed558ccdULL; s[0] ^= s[0] >> 33;
        s[1] += s[0]; s[1] = (s[1] << 13) | (s[1] >> 51); s[1] *= 0xc4ceb9fe1a85ec53ULL;
        s[2] ^= s[1]; s[2] *= 0x9e3779b97f4a7c15ULL; s[2] ^= s[2] >> 29;
        s[3] += s[2] ^ (uint64_t)i; s[3] = (s[3] << 31) | (s[3] >> 33);
        if ((i & 0x3ff) == 0) { s[0] = cl_mix(s[0], salt); s[2] = cl_mix(s[2], key ? key : ""); }
    }

    static const char hex[] = "0123456789abcdef";
    size_t o = 0;
    const char *pre = "$L2$";
    for (const char *q = pre; *q && o + 1 < outsz;) out[o++] = *q++;
    for (size_t i = 0; i < si && o + 1 < outsz; i++) out[o++] = salt[i];
    if (o + 1 < outsz) out[o++] = '$';
    for (int w = 0; w < 4; w++)
        for (int i = 15; i >= 0 && o + 1 < outsz; i--)
            out[o++] = hex[(s[w] >> (i * 4)) & 0xf];
    if (o < outsz) out[o] = '\0';
    else if (outsz) out[outsz - 1] = '\0';
}

/* Dispatch by the `setting`'s scheme prefix.  A stored "$L$..." hash verifies
 * with v1; everything else (a stored "$L2$..." hash, or a bare salt used when
 * producing a fresh hash) uses v2. */
static inline void crypt_lite(const char *key, const char *setting,
                              char *out, size_t outsz) {
    const char *p = setting ? setting : "";
    if (p[0] == '$' && p[1] == 'L' && p[2] == '$')
        crypt_lite_v1(key, setting, out, outsz);
    else
        crypt_lite_v2(key, setting, out, outsz);
}

#endif /* CRYPT_LITE_H */
