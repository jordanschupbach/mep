#include "git/sha1.h"

#include <algorithm>
#include <cstring>

namespace mep::git {

namespace {
inline uint32_t Rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
}  // namespace

Sha1::Sha1() : h_{0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u}, buf_{} {}

void Sha1::Block(const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = (static_cast<uint32_t>(p[4 * i]) << 24) | (static_cast<uint32_t>(p[4 * i + 1]) << 16) |
               (static_cast<uint32_t>(p[4 * i + 2]) << 8) | static_cast<uint32_t>(p[4 * i + 3]);
    }
    for (int i = 16; i < 80; i++) w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        uint32_t t = Rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = Rol(b, 30);
        b = a;
        a = t;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
}

void Sha1::Update(const void *data, size_t len) {
    const uint8_t *p = static_cast<const uint8_t *>(data);
    total_ += len;
    if (buf_len_) {
        size_t take = std::min(len, 64 - buf_len_);
        std::memcpy(buf_ + buf_len_, p, take);
        buf_len_ += take;
        p += take;
        len -= take;
        if (buf_len_ == 64) {
            Block(buf_);
            buf_len_ = 0;
        }
    }
    while (len >= 64) {
        Block(p);
        p += 64;
        len -= 64;
    }
    if (len) {
        std::memcpy(buf_, p, len);
        buf_len_ = len;
    }
}

std::array<uint8_t, 20> Sha1::Final() {
    const uint64_t bits = total_ * 8;
    uint8_t pad[72] = {0x80};
    size_t pad_len = (buf_len_ < 56) ? 56 - buf_len_ : 120 - buf_len_;
    Update(pad, pad_len);
    uint8_t len_be[8];
    for (int i = 0; i < 8; i++) len_be[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    Update(len_be, 8);
    std::array<uint8_t, 20> out{};
    for (size_t i = 0; i < 5; i++) {
        out[4 * i] = static_cast<uint8_t>(h_[i] >> 24);
        out[4 * i + 1] = static_cast<uint8_t>(h_[i] >> 16);
        out[4 * i + 2] = static_cast<uint8_t>(h_[i] >> 8);
        out[4 * i + 3] = static_cast<uint8_t>(h_[i]);
    }
    return out;
}

}  // namespace mep::git
