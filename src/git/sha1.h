#pragma once

// SHA-1 (FIPS 180-4) for git object ids. Not a security boundary here:
// git uses it as a content address, and mep only needs to agree with the
// ids git itself computes. (A hardened/collision-detecting variant belongs
// in the Phase 2 crypto module if anything ever needs one.)

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace mep::git {

class Sha1 {
public:
    Sha1();
    void Update(const void *data, size_t len);
    void Update(std::string_view s) { Update(s.data(), s.size()); }
    std::array<uint8_t, 20> Final();

    static std::array<uint8_t, 20> Hash(std::string_view s) {
        Sha1 h;
        h.Update(s);
        return h.Final();
    }

private:
    void Block(const uint8_t *p);
    uint32_t h_[5];
    uint8_t buf_[64];
    size_t buf_len_ = 0;
    uint64_t total_ = 0;
};

}  // namespace mep::git
