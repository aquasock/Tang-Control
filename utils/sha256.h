#pragma once

#include <stddef.h>
#include <stdint.h>

// Portable FIPS 180-4 SHA-256 so firmware images are identified by the same
// digest on the device, in host tests, and by tangctl.
struct Sha256 {
    uint32_t state[8];
    uint64_t length;
    uint8_t block[64];
    size_t used;
};

namespace sha256_detail {

inline uint32_t rotr(uint32_t value, unsigned bits)
{
    return (value >> bits) | (value << (32 - bits));
}

inline void compress(Sha256 &sha, const uint8_t *block)
{
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
        0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (unsigned i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(block[4 * i]) << 24) |
               (static_cast<uint32_t>(block[4 * i + 1]) << 16) |
               (static_cast<uint32_t>(block[4 * i + 2]) << 8) | block[4 * i + 3];
    }
    for (unsigned i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = sha.state[0], b = sha.state[1], c = sha.state[2], d = sha.state[3];
    uint32_t e = sha.state[4], f = sha.state[5], g = sha.state[6], h = sha.state[7];
    for (unsigned i = 0; i < 64; ++i) {
        const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) +
                            ((e & f) ^ (~e & g)) + k[i] + w[i];
        const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) +
                            ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    sha.state[0] += a; sha.state[1] += b; sha.state[2] += c; sha.state[3] += d;
    sha.state[4] += e; sha.state[5] += f; sha.state[6] += g; sha.state[7] += h;
}

} // namespace sha256_detail

inline void sha256_init(Sha256 &sha)
{
    static const uint32_t initial[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    for (unsigned i = 0; i < 8; ++i) {
        sha.state[i] = initial[i];
    }
    sha.length = 0;
    sha.used = 0;
}

inline void sha256_update(Sha256 &sha, const uint8_t *data, size_t length)
{
    sha.length += length;
    while (length != 0) {
        const size_t take = length < 64 - sha.used ? length : 64 - sha.used;
        for (size_t i = 0; i < take; ++i) {
            sha.block[sha.used + i] = data[i];
        }
        sha.used += take;
        data += take;
        length -= take;
        if (sha.used == 64) {
            sha256_detail::compress(sha, sha.block);
            sha.used = 0;
        }
    }
}

inline void sha256_final(Sha256 &sha, uint8_t digest[32])
{
    const uint64_t bits = sha.length * 8;
    const uint8_t pad = 0x80;
    sha256_update(sha, &pad, 1);
    const uint8_t zero = 0;
    while (sha.used != 56) {
        sha256_update(sha, &zero, 1);
    }
    uint8_t length_bytes[8];
    for (unsigned i = 0; i < 8; ++i) {
        length_bytes[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    }
    sha256_update(sha, length_bytes, 8);
    for (unsigned i = 0; i < 8; ++i) {
        digest[4 * i] = static_cast<uint8_t>(sha.state[i] >> 24);
        digest[4 * i + 1] = static_cast<uint8_t>(sha.state[i] >> 16);
        digest[4 * i + 2] = static_cast<uint8_t>(sha.state[i] >> 8);
        digest[4 * i + 3] = static_cast<uint8_t>(sha.state[i]);
    }
}
