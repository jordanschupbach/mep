#include "mail_crypto.h"

#include "mail.h"  // Sha256

#include <cstring>
#include <fstream>
#include <stdexcept>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <stdlib.h>  // arc4random_buf
#else
#include <unistd.h>  // getentropy
#endif

namespace mail {

std::string SecureRandom(size_t n) {
    std::string out(n, '\0');
    if (n == 0) return out;
#if defined(_WIN32)
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(out.data()), static_cast<ULONG>(n),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0) {
        return out;
    }
#elif defined(__APPLE__)
    arc4random_buf(out.data(), n);
    return out;
#else
    bool ok = true;
    for (size_t off = 0; off < n && ok; off += 256) {
        const size_t chunk = n - off < 256 ? n - off : 256;  // getentropy's per-call limit
        ok = getentropy(out.data() + off, chunk) == 0;
    }
    if (ok) return out;
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (urandom.read(out.data(), static_cast<std::streamsize>(n))) return out;
#endif
    // No randomness means no safe key or nonce: refuse rather than guess.
    throw std::runtime_error("no secure random source");
}

std::string HmacSha256(std::string_view key, std::string_view data) {
    std::string k(key);
    if (k.size() > 64) k = Sha256(k);
    k.resize(64, '\0');
    std::string ipad(64, '\0'), opad(64, '\0');
    for (size_t i = 0; i < 64; i++) {
        ipad[i] = static_cast<char>(k[i] ^ 0x36);
        opad[i] = static_cast<char>(k[i] ^ 0x5c);
    }
    std::string inner = ipad;
    inner.append(data);
    std::string mac = Sha256(opad + Sha256(inner));
    Wipe(&k);
    Wipe(&ipad);
    Wipe(&opad);
    Wipe(&inner);
    return mac;
}

std::string Pbkdf2Sha256(std::string_view password, std::string_view salt, uint32_t iterations, size_t length) {
    std::string out;
    for (uint32_t block = 1; out.size() < length; block++) {
        std::string s(salt);
        s.push_back(static_cast<char>((block >> 24) & 0xFF));
        s.push_back(static_cast<char>((block >> 16) & 0xFF));
        s.push_back(static_cast<char>((block >> 8) & 0xFF));
        s.push_back(static_cast<char>(block & 0xFF));
        std::string u = HmacSha256(password, s);
        std::string t = u;
        for (uint32_t i = 1; i < iterations; i++) {
            u = HmacSha256(password, u);
            for (size_t j = 0; j < t.size(); j++) t[j] = static_cast<char>(t[j] ^ u[j]);
        }
        out += t;
        Wipe(&u);
        Wipe(&t);
    }
    out.resize(length);
    return out;
}

namespace {

uint32_t Le32(const unsigned char *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

void PutLe32(unsigned char *p, uint32_t v) {
    p[0] = static_cast<unsigned char>(v);
    p[1] = static_cast<unsigned char>(v >> 8);
    p[2] = static_cast<unsigned char>(v >> 16);
    p[3] = static_cast<unsigned char>(v >> 24);
}

uint32_t Rotl(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

void QuarterRound(uint32_t *x, int a, int b, int c, int d) {
    x[a] += x[b];
    x[d] = Rotl(x[d] ^ x[a], 16);
    x[c] += x[d];
    x[b] = Rotl(x[b] ^ x[c], 12);
    x[a] += x[b];
    x[d] = Rotl(x[d] ^ x[a], 8);
    x[c] += x[d];
    x[b] = Rotl(x[b] ^ x[c], 7);
}

// One 64-byte ChaCha20 keystream block (RFC 8439 section 2.3).
void ChaChaBlock(const unsigned char key[32], uint32_t counter, const unsigned char nonce[12], unsigned char out[64]) {
    uint32_t s[16];
    s[0] = 0x61707865;
    s[1] = 0x3320646e;
    s[2] = 0x79622d32;
    s[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) s[4 + i] = Le32(key + 4 * i);
    s[12] = counter;
    for (int i = 0; i < 3; i++) s[13 + i] = Le32(nonce + 4 * i);
    uint32_t x[16];
    std::memcpy(x, s, sizeof(s));
    for (int i = 0; i < 10; i++) {
        QuarterRound(x, 0, 4, 8, 12);
        QuarterRound(x, 1, 5, 9, 13);
        QuarterRound(x, 2, 6, 10, 14);
        QuarterRound(x, 3, 7, 11, 15);
        QuarterRound(x, 0, 5, 10, 15);
        QuarterRound(x, 1, 6, 11, 12);
        QuarterRound(x, 2, 7, 8, 13);
        QuarterRound(x, 3, 4, 9, 14);
    }
    for (int i = 0; i < 16; i++) PutLe32(out + 4 * i, x[i] + s[i]);
    std::memset(x, 0, sizeof(x));
    std::memset(s, 0, sizeof(s));
}

std::string ChaChaXor(const unsigned char key[32], uint32_t counter, const unsigned char nonce[12], std::string_view in) {
    std::string out(in);
    unsigned char block[64];
    for (size_t off = 0; off < out.size(); off += 64) {
        ChaChaBlock(key, counter++, nonce, block);
        const size_t n = out.size() - off < 64 ? out.size() - off : 64;
        for (size_t i = 0; i < n; i++) out[off + i] = static_cast<char>(static_cast<unsigned char>(out[off + i]) ^ block[i]);
    }
    std::memset(block, 0, sizeof(block));
    return out;
}

// Poly1305 (RFC 8439 section 2.5) with 26-bit limbs: no 128-bit integers needed.
std::string Poly1305(const unsigned char key[32], std::string_view msg) {
    const uint32_t r0 = Le32(key) & 0x3ffffff;
    const uint32_t r1 = (Le32(key + 3) >> 2) & 0x3ffff03;
    const uint32_t r2 = (Le32(key + 6) >> 4) & 0x3ffc0ff;
    const uint32_t r3 = (Le32(key + 9) >> 6) & 0x3f03fff;
    const uint32_t r4 = (Le32(key + 12) >> 8) & 0x00fffff;
    const uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = 0, h1 = 0, h2 = 0, h3 = 0, h4 = 0;
    const unsigned char *m = reinterpret_cast<const unsigned char *>(msg.data());
    size_t left = msg.size();
    unsigned char buf[16];
    while (left > 0) {
        uint32_t hibit = 1u << 24;
        const unsigned char *blk = m;
        if (left < 16) {
            std::memset(buf, 0, sizeof(buf));
            std::memcpy(buf, m, left);
            buf[left] = 1;
            hibit = 0;
            blk = buf;
        }
        h0 += Le32(blk) & 0x3ffffff;
        h1 += (Le32(blk + 3) >> 2) & 0x3ffffff;
        h2 += (Le32(blk + 6) >> 4) & 0x3ffffff;
        h3 += (Le32(blk + 9) >> 6) & 0x3ffffff;
        h4 += (Le32(blk + 12) >> 8) | hibit;
        const uint64_t d0 = static_cast<uint64_t>(h0) * r0 + static_cast<uint64_t>(h1) * s4 + static_cast<uint64_t>(h2) * s3 +
                            static_cast<uint64_t>(h3) * s2 + static_cast<uint64_t>(h4) * s1;
        uint64_t d1 = static_cast<uint64_t>(h0) * r1 + static_cast<uint64_t>(h1) * r0 + static_cast<uint64_t>(h2) * s4 +
                      static_cast<uint64_t>(h3) * s3 + static_cast<uint64_t>(h4) * s2;
        uint64_t d2 = static_cast<uint64_t>(h0) * r2 + static_cast<uint64_t>(h1) * r1 + static_cast<uint64_t>(h2) * r0 +
                      static_cast<uint64_t>(h3) * s4 + static_cast<uint64_t>(h4) * s3;
        uint64_t d3 = static_cast<uint64_t>(h0) * r3 + static_cast<uint64_t>(h1) * r2 + static_cast<uint64_t>(h2) * r1 +
                      static_cast<uint64_t>(h3) * r0 + static_cast<uint64_t>(h4) * s4;
        uint64_t d4 = static_cast<uint64_t>(h0) * r4 + static_cast<uint64_t>(h1) * r3 + static_cast<uint64_t>(h2) * r2 +
                      static_cast<uint64_t>(h3) * r1 + static_cast<uint64_t>(h4) * r0;
        uint32_t c = static_cast<uint32_t>(d0 >> 26);
        h0 = static_cast<uint32_t>(d0) & 0x3ffffff;
        d1 += c;
        c = static_cast<uint32_t>(d1 >> 26);
        h1 = static_cast<uint32_t>(d1) & 0x3ffffff;
        d2 += c;
        c = static_cast<uint32_t>(d2 >> 26);
        h2 = static_cast<uint32_t>(d2) & 0x3ffffff;
        d3 += c;
        c = static_cast<uint32_t>(d3 >> 26);
        h3 = static_cast<uint32_t>(d3) & 0x3ffffff;
        d4 += c;
        c = static_cast<uint32_t>(d4 >> 26);
        h4 = static_cast<uint32_t>(d4) & 0x3ffffff;
        h0 += c * 5;
        c = h0 >> 26;
        h0 &= 0x3ffffff;
        h1 += c;
        const size_t step = left < 16 ? left : 16;
        m += step;
        left -= step;
    }
    // Full carry, then h - p if h >= p (constant time).
    uint32_t c = h1 >> 26;
    h1 &= 0x3ffffff;
    h2 += c;
    c = h2 >> 26;
    h2 &= 0x3ffffff;
    h3 += c;
    c = h3 >> 26;
    h3 &= 0x3ffffff;
    h4 += c;
    c = h4 >> 26;
    h4 &= 0x3ffffff;
    h0 += c * 5;
    c = h0 >> 26;
    h0 &= 0x3ffffff;
    h1 += c;
    uint32_t g0 = h0 + 5;
    c = g0 >> 26;
    g0 &= 0x3ffffff;
    uint32_t g1 = h1 + c;
    c = g1 >> 26;
    g1 &= 0x3ffffff;
    uint32_t g2 = h2 + c;
    c = g2 >> 26;
    g2 &= 0x3ffffff;
    uint32_t g3 = h3 + c;
    c = g3 >> 26;
    g3 &= 0x3ffffff;
    uint32_t g4 = h4 + c - (1u << 26);
    uint32_t mask = (g4 >> 31) - 1;
    g0 &= mask;
    g1 &= mask;
    g2 &= mask;
    g3 &= mask;
    g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0;
    h1 = (h1 & mask) | g1;
    h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3;
    h4 = (h4 & mask) | g4;
    h0 = h0 | (h1 << 26);
    h1 = (h1 >> 6) | (h2 << 20);
    h2 = (h2 >> 12) | (h3 << 14);
    h3 = (h3 >> 18) | (h4 << 8);
    uint64_t f = static_cast<uint64_t>(h0) + Le32(key + 16);
    h0 = static_cast<uint32_t>(f);
    f = static_cast<uint64_t>(h1) + Le32(key + 20) + (f >> 32);
    h1 = static_cast<uint32_t>(f);
    f = static_cast<uint64_t>(h2) + Le32(key + 24) + (f >> 32);
    h2 = static_cast<uint32_t>(f);
    f = static_cast<uint64_t>(h3) + Le32(key + 28) + (f >> 32);
    h3 = static_cast<uint32_t>(f);
    unsigned char tag[16];
    PutLe32(tag, h0);
    PutLe32(tag + 4, h1);
    PutLe32(tag + 8, h2);
    PutLe32(tag + 12, h3);
    return std::string(reinterpret_cast<char *>(tag), 16);
}

std::string AeadTag(const unsigned char key[32], const unsigned char nonce[12], std::string_view aad, std::string_view ct) {
    unsigned char block0[64];
    ChaChaBlock(key, 0, nonce, block0);  // the one-time Poly1305 key
    std::string mac_data(aad);
    mac_data.append((16 - aad.size() % 16) % 16, '\0');
    mac_data.append(ct);
    mac_data.append((16 - ct.size() % 16) % 16, '\0');
    unsigned char lens[16];
    const uint64_t la = aad.size(), lc = ct.size();
    for (int i = 0; i < 8; i++) {
        lens[i] = static_cast<unsigned char>(la >> (8 * i));
        lens[8 + i] = static_cast<unsigned char>(lc >> (8 * i));
    }
    mac_data.append(reinterpret_cast<char *>(lens), 16);
    std::string tag = Poly1305(block0, mac_data);
    std::memset(block0, 0, sizeof(block0));
    return tag;
}

}  // namespace

std::string AeadSeal(std::string_view key, std::string_view nonce, std::string_view plaintext, std::string_view aad) {
    if (key.size() != 32 || nonce.size() != 12) return "";
    const auto *k = reinterpret_cast<const unsigned char *>(key.data());
    const auto *n = reinterpret_cast<const unsigned char *>(nonce.data());
    std::string ct = ChaChaXor(k, 1, n, plaintext);
    return ct + AeadTag(k, n, aad, ct);
}

bool AeadOpen(std::string_view key, std::string_view nonce, std::string_view sealed, std::string_view aad,
              std::string *plaintext) {
    if (key.size() != 32 || nonce.size() != 12 || sealed.size() < 16) return false;
    const auto *k = reinterpret_cast<const unsigned char *>(key.data());
    const auto *n = reinterpret_cast<const unsigned char *>(nonce.data());
    const std::string_view ct = sealed.substr(0, sealed.size() - 16);
    const std::string want = AeadTag(k, n, aad, ct);
    unsigned char diff = 0;  // constant-time compare
    for (size_t i = 0; i < 16; i++) diff |= static_cast<unsigned char>(want[i] ^ sealed[sealed.size() - 16 + i]);
    if (diff != 0) return false;
    *plaintext = ChaChaXor(k, 1, n, ct);
    return true;
}

void Wipe(std::string *s) {
    volatile char *p = s->data();
    for (size_t i = 0; i < s->size(); i++) p[i] = 0;
    s->clear();
}

}  // namespace mail
