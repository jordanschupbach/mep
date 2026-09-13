#include "pdf_crypt.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace pdfcrypt {

namespace {

// -- MD5 (RFC 1321) --------------------------------------------------

uint32_t LeftRotate(uint32_t x, uint32_t c) { return (x << c) | (x >> (32 - c)); }

std::vector<unsigned char> Md5Pad(const std::string &input) {
    std::vector<unsigned char> msg(input.begin(), input.end());
    uint64_t bit_len = input.size() * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0x00);
    for (int i = 0; i < 8; ++i) {
        msg.push_back(static_cast<unsigned char>((bit_len >> (8 * static_cast<unsigned>(i))) & 0xFFu));
    }
    return msg;
}

}  // namespace

std::string Md5(const std::string &input) {
    static const uint32_t kS[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
    };
    static const uint32_t kK[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
    };
    uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
    std::vector<unsigned char> msg = Md5Pad(input);

    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t m[16];
        for (size_t i = 0; i < 16; ++i) {
            size_t base = chunk + i * 4;
            m[i] = static_cast<uint32_t>(msg[base]) | (static_cast<uint32_t>(msg[base + 1]) << 8) |
                   (static_cast<uint32_t>(msg[base + 2]) << 16) | (static_cast<uint32_t>(msg[base + 3]) << 24);
        }
        uint32_t a = a0, b = b0, c = c0, d = d0;
        for (int i = 0; i < 64; ++i) {
            uint32_t f = 0;
            int g = 0;
            if (i < 16) {
                f = (b & c) | (~b & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | (~d & c);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3 * i + 5) % 16;
            } else {
                f = c ^ (b | ~d);
                g = (7 * i) % 16;
            }
            uint32_t temp = d;
            d = c;
            c = b;
            b = b + LeftRotate(a + f + kK[static_cast<size_t>(i)] + m[static_cast<size_t>(g)],
                                kS[static_cast<size_t>(i)]);
            a = temp;
        }
        a0 += a;
        b0 += b;
        c0 += c;
        d0 += d;
    }

    unsigned char digest[16];
    uint32_t words[4] = {a0, b0, c0, d0};
    for (size_t i = 0; i < 4; ++i) {
        digest[i * 4 + 0] = static_cast<unsigned char>(words[i] & 0xFFu);
        digest[i * 4 + 1] = static_cast<unsigned char>((words[i] >> 8) & 0xFFu);
        digest[i * 4 + 2] = static_cast<unsigned char>((words[i] >> 16) & 0xFFu);
        digest[i * 4 + 3] = static_cast<unsigned char>((words[i] >> 24) & 0xFFu);
    }
    return std::string(reinterpret_cast<char *>(digest), 16);
}

// -- RC4 ---------------------------------------------------------------

std::string Rc4(const std::string &key, const std::string &data) {
    if (key.empty()) return data;  // shouldn't happen -- object keys are always >=5 bytes; no-op rather than UB
    unsigned char s[256];
    for (int i = 0; i < 256; ++i) s[static_cast<size_t>(i)] = static_cast<unsigned char>(i);

    int j = 0;
    for (int i = 0; i < 256; ++i) {
        unsigned char key_byte = static_cast<unsigned char>(key[static_cast<size_t>(i) % key.size()]);
        j = (j + s[static_cast<size_t>(i)] + key_byte) % 256;
        std::swap(s[static_cast<size_t>(i)], s[static_cast<size_t>(j)]);
    }

    std::string out(data.size(), '\0');
    int x = 0;
    j = 0;
    for (size_t n = 0; n < data.size(); ++n) {
        x = (x + 1) % 256;
        j = (j + s[static_cast<size_t>(x)]) % 256;
        std::swap(s[static_cast<size_t>(x)], s[static_cast<size_t>(j)]);
        unsigned char k = s[static_cast<size_t>((s[static_cast<size_t>(x)] + s[static_cast<size_t>(j)]) % 256)];
        out[n] = static_cast<char>(static_cast<unsigned char>(static_cast<unsigned char>(data[n]) ^ k));
    }
    return out;
}

// -- AES-128 (decrypt-only) ---------------------------------------------

namespace {

unsigned char GfMul(unsigned char a, unsigned char b) {
    unsigned char p = 0;
    for (int i = 0; i < 8; ++i) {
        if (b & 1u) p = static_cast<unsigned char>(p ^ a);
        bool hi = (a & 0x80u) != 0;
        a = static_cast<unsigned char>(a << 1);
        if (hi) a = static_cast<unsigned char>(a ^ 0x1Bu);
        b = static_cast<unsigned char>(b >> 1);
    }
    return p;
}

unsigned char GfInverse(unsigned char a) {
    if (a == 0) return 0;
    for (int b = 1; b < 256; ++b) {
        if (GfMul(a, static_cast<unsigned char>(b)) == 1) return static_cast<unsigned char>(b);
    }
    return 0;
}

struct SboxTables {
    unsigned char sbox[256];
    unsigned char inv_sbox[256];
};

// Standard AES S-box construction (FIPS-197 5.1.1): multiplicative
// inverse in GF(2^8) (0 maps to itself) composed with a fixed affine
// transformation over GF(2) -- computed here rather than transcribed as
// a 256-entry literal table, since a single mistyped byte in a hand-
// copied table would silently corrupt every glyph/stream that happens
// to touch it, whereas this construction is self-checking (verified
// against the well-known sbox[0]==0x63/sbox[1]==0x7c spot values in
// pdf_crypt_test.cpp, plus the full cipher against a published FIPS-197
// test vector).
const SboxTables &GetSboxTables() {
    static const SboxTables table = [] {
        SboxTables t{};
        for (int i = 0; i < 256; ++i) {
            unsigned char inv = GfInverse(static_cast<unsigned char>(i));
            unsigned char s = inv;
            unsigned char x = inv;
            for (int r = 0; r < 4; ++r) {
                x = static_cast<unsigned char>(static_cast<unsigned char>(x << 1) | static_cast<unsigned char>(x >> 7));
                s = static_cast<unsigned char>(s ^ x);
            }
            s = static_cast<unsigned char>(s ^ 0x63u);
            t.sbox[static_cast<size_t>(i)] = s;
            t.inv_sbox[s] = static_cast<unsigned char>(i);
        }
        return t;
    }();
    return table;
}

constexpr unsigned char kRcon[11] = {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36};

struct RoundKeys {
    unsigned char rk[11][16];  // 11 round keys for AES-128 (Nr=10), each in column-major state layout
};

void KeyExpansion(const unsigned char key[16], RoundKeys *out) {
    const SboxTables &sbox = GetSboxTables();
    unsigned char w[44][4];
    for (size_t i = 0; i < 4; ++i) {
        for (size_t j = 0; j < 4; ++j) w[i][j] = key[i * 4 + j];
    }
    for (size_t i = 4; i < 44; ++i) {
        unsigned char temp[4];
        for (size_t j = 0; j < 4; ++j) temp[j] = w[i - 1][j];
        if (i % 4 == 0) {
            unsigned char t0 = temp[0];
            temp[0] = static_cast<unsigned char>(sbox.sbox[temp[1]] ^ kRcon[i / 4]);
            temp[1] = sbox.sbox[temp[2]];
            temp[2] = sbox.sbox[temp[3]];
            temp[3] = sbox.sbox[t0];
        }
        for (size_t j = 0; j < 4; ++j) w[i][j] = static_cast<unsigned char>(w[i - 4][j] ^ temp[j]);
    }
    for (size_t r = 0; r < 11; ++r) {
        for (size_t c = 0; c < 4; ++c) {
            for (size_t row = 0; row < 4; ++row) out->rk[r][c * 4 + row] = w[r * 4 + c][row];
        }
    }
}

void AddRoundKey(unsigned char state[16], const unsigned char round_key[16]) {
    for (int i = 0; i < 16; ++i) state[i] = static_cast<unsigned char>(state[i] ^ round_key[i]);
}

void InvSubBytes(unsigned char state[16], const unsigned char inv_sbox[256]) {
    for (int i = 0; i < 16; ++i) state[i] = inv_sbox[state[i]];
}

void InvShiftRows(unsigned char state[16]) {
    unsigned char t[16];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            int src_c = (c - r + 4) % 4;
            t[static_cast<size_t>(r + 4 * c)] = state[static_cast<size_t>(r + 4 * src_c)];
        }
    }
    std::copy(t, t + 16, state);
}

void InvMixColumns(unsigned char state[16]) {
    for (int c = 0; c < 4; ++c) {
        size_t i0 = static_cast<size_t>(4 * c), i1 = i0 + 1, i2 = i0 + 2, i3 = i0 + 3;
        unsigned char s0 = state[i0], s1 = state[i1], s2 = state[i2], s3 = state[i3];
        state[i0] = static_cast<unsigned char>(GfMul(s0, 14) ^ GfMul(s1, 11) ^ GfMul(s2, 13) ^ GfMul(s3, 9));
        state[i1] = static_cast<unsigned char>(GfMul(s0, 9) ^ GfMul(s1, 14) ^ GfMul(s2, 11) ^ GfMul(s3, 13));
        state[i2] = static_cast<unsigned char>(GfMul(s0, 13) ^ GfMul(s1, 9) ^ GfMul(s2, 14) ^ GfMul(s3, 11));
        state[i3] = static_cast<unsigned char>(GfMul(s0, 11) ^ GfMul(s1, 13) ^ GfMul(s2, 9) ^ GfMul(s3, 14));
    }
}

}  // namespace

void AesDecryptBlock(const std::string &key16, const unsigned char in[16], unsigned char out[16]) {
    if (key16.size() != 16) {
        std::copy(in, in + 16, out);
        return;
    }
    RoundKeys rk{};
    KeyExpansion(reinterpret_cast<const unsigned char *>(key16.data()), &rk);
    const SboxTables &sbox = GetSboxTables();

    unsigned char state[16];
    std::copy(in, in + 16, state);
    AddRoundKey(state, rk.rk[10]);
    for (int round = 9; round >= 1; --round) {
        InvShiftRows(state);
        InvSubBytes(state, sbox.inv_sbox);
        AddRoundKey(state, rk.rk[round]);
        InvMixColumns(state);
    }
    InvShiftRows(state);
    InvSubBytes(state, sbox.inv_sbox);
    AddRoundKey(state, rk.rk[0]);
    std::copy(state, state + 16, out);
}

bool AesCbcDecrypt(const std::string &key16, const std::string &iv16, const std::string &ciphertext,
                    std::string *out) {
    if (key16.size() != 16 || iv16.size() != 16) return false;
    if (ciphertext.empty() || ciphertext.size() % 16 != 0) return false;

    std::string plain(ciphertext.size(), '\0');
    unsigned char prev[16];
    std::copy(iv16.begin(), iv16.end(), prev);

    for (size_t off = 0; off < ciphertext.size(); off += 16) {
        const unsigned char *cblock = reinterpret_cast<const unsigned char *>(ciphertext.data()) + off;
        unsigned char block_out[16];
        AesDecryptBlock(key16, cblock, block_out);
        for (size_t i = 0; i < 16; ++i) {
            plain[off + i] = static_cast<char>(static_cast<unsigned char>(block_out[i] ^ prev[i]));
        }
        std::copy(cblock, cblock + 16, prev);
    }

    unsigned char pad = static_cast<unsigned char>(plain.back());
    if (pad == 0 || pad > 16 || static_cast<size_t>(pad) > plain.size()) return false;
    for (size_t i = plain.size() - pad; i < plain.size(); ++i) {
        if (static_cast<unsigned char>(plain[i]) != pad) return false;
    }
    plain.resize(plain.size() - pad);
    *out = std::move(plain);
    return true;
}

// -- Standard Security Handler ------------------------------------------

namespace {

// Spec 7.6.3.3 Algorithm 2's own fixed 32-byte padding string.
constexpr unsigned char kPad[32] = {
    0x28, 0xBF, 0x4E, 0x5E, 0x4E, 0x75, 0x8A, 0x41, 0x64, 0x00, 0x4E, 0x56, 0xFF, 0xFA, 0x01, 0x08,
    0x2E, 0x2E, 0x00, 0xB6, 0xD0, 0x68, 0x3E, 0x80, 0x2F, 0x0C, 0xA9, 0xFE, 0x64, 0x53, 0x69, 0x7A,
};

std::string PadString() { return std::string(reinterpret_cast<const char *>(kPad), 32); }

// Spec 7.6.2 Algorithm 1: object key = first min(n+5,16) bytes of
// MD5(file_key + low-order 3 bytes of `num` + low-order 2 bytes of
// `gen` [+ the fixed 4-byte "sAlT" suffix, AESV2 only]).
std::string DeriveObjectKey(const std::string &file_key, int num, int gen, bool for_aes) {
    std::string input = file_key;
    unsigned char extra[5] = {
        static_cast<unsigned char>(num & 0xFF),        static_cast<unsigned char>((num >> 8) & 0xFF),
        static_cast<unsigned char>((num >> 16) & 0xFF), static_cast<unsigned char>(gen & 0xFF),
        static_cast<unsigned char>((gen >> 8) & 0xFF),
    };
    input.append(reinterpret_cast<const char *>(extra), 5);
    if (for_aes) input += "sAlT";
    std::string digest = Md5(input);
    size_t key_len = std::min(file_key.size() + 5, static_cast<size_t>(16));
    return digest.substr(0, key_len);
}

std::string DecryptWithMethod(const std::string &file_key, int num, int gen, CryptMethod method,
                               const std::string &data) {
    switch (method) {
        case CryptMethod::kIdentity:
            return data;
        case CryptMethod::kRc4: {
            std::string key = DeriveObjectKey(file_key, num, gen, false);
            return Rc4(key, data);
        }
        case CryptMethod::kAesV2: {
            if (data.size() < 16) return data;  // too short to even hold an IV: tolerate, leave unchanged
            std::string key = DeriveObjectKey(file_key, num, gen, true);
            std::string iv = data.substr(0, 16);
            std::string ciphertext = data.substr(16);
            std::string plain;
            if (ciphertext.empty() || !AesCbcDecrypt(key, iv, ciphertext, &plain)) return data;
            return plain;
        }
    }
    return data;
}

}  // namespace

bool SetupStandardSecurityHandler(const pdfobj::Object &encrypt_dict, const std::string &id0, int encrypt_obj_num,
                                   EncryptionState *out) {
    const pdfobj::Object *filter = encrypt_dict.Find("Filter");
    if (!filter || filter->AsString("") != "Standard") return false;

    long long v = 0;
    if (const pdfobj::Object *vo = encrypt_dict.Find("V")) v = vo->AsInt(0);
    long long r = v < 2 ? 2 : 3;
    if (const pdfobj::Object *ro = encrypt_dict.Find("R")) r = ro->AsInt(r);
    if (r < 2 || r > 4) return false;  // R5/R6 (AES-256, /V 5): out of scope, see this header's own doc comment

    const pdfobj::Object *o_obj = encrypt_dict.Find("O");
    const pdfobj::Object *u_obj = encrypt_dict.Find("U");
    if (!o_obj || !o_obj->IsString() || !u_obj || !u_obj->IsString()) return false;
    if (o_obj->str_val.size() < 32 || u_obj->str_val.size() < 32) return false;
    std::string o = o_obj->str_val.substr(0, 32);
    std::string u = u_obj->str_val.substr(0, 32);

    long long p = 0;
    if (const pdfobj::Object *po = encrypt_dict.Find("P")) p = po->AsInt(0);

    int key_len_bytes = 5;
    if (const pdfobj::Object *length = encrypt_dict.Find("Length")) {
        key_len_bytes = static_cast<int>(length->AsInt(40) / 8);
    }
    if (key_len_bytes < 5 || key_len_bytes > 16) key_len_bytes = 16;

    CryptMethod stream_method = CryptMethod::kRc4;
    CryptMethod string_method = CryptMethod::kRc4;
    if (v == 4) {
        auto resolve_cfm = [&](const char *key) -> CryptMethod {
            const pdfobj::Object *name_obj = encrypt_dict.Find(key);
            std::string cf_name = name_obj ? name_obj->AsString("Identity") : "Identity";
            if (cf_name == "Identity") return CryptMethod::kIdentity;
            const pdfobj::Object *cf = encrypt_dict.Find("CF");
            if (!cf || !cf->IsDict()) return CryptMethod::kRc4;
            const pdfobj::Object *cf_entry = cf->Find(cf_name);
            if (!cf_entry || !cf_entry->IsDict()) return CryptMethod::kRc4;
            const pdfobj::Object *cfm = cf_entry->Find("CFM");
            std::string m = cfm ? cfm->AsString("") : "";
            return m == "AESV2" ? CryptMethod::kAesV2 : CryptMethod::kRc4;
        };
        stream_method = resolve_cfm("StmF");
        string_method = resolve_cfm("StrF");
    }

    // Algorithm 2 (empty user password: the pad string stands in for the
    // padded password directly).
    std::string input = PadString();
    input += o;
    unsigned char p_bytes[4] = {
        static_cast<unsigned char>(p & 0xFF),
        static_cast<unsigned char>((p >> 8) & 0xFF),
        static_cast<unsigned char>((p >> 16) & 0xFF),
        static_cast<unsigned char>((p >> 24) & 0xFF),
    };
    input.append(reinterpret_cast<const char *>(p_bytes), 4);
    input += id0;
    // (R>=4 with /EncryptMetadata false would append 4 bytes of 0xFF
    // here -- not implemented: /EncryptMetadata defaults to true, and no
    // fixture this plan has access to sets it false. A documented
    // Scoping gap, not a silent one.)
    std::string digest = Md5(input);
    if (r >= 3) {
        for (int i = 0; i < 50; ++i) digest = Md5(digest.substr(0, static_cast<size_t>(key_len_bytes)));
    }
    std::string file_key = digest.substr(0, static_cast<size_t>(key_len_bytes));

    // Algorithm 6: authenticate by recomputing /U from this key.
    bool authenticated = false;
    if (r == 2) {
        authenticated = Rc4(file_key, PadString()) == u;
    } else {
        std::string h_input = PadString();
        h_input += id0;
        std::string enc16 = Rc4(file_key, Md5(h_input));
        for (int i = 1; i <= 19; ++i) {
            std::string key_i = file_key;
            for (char &c : key_i) {
                c = static_cast<char>(static_cast<unsigned char>(c) ^ static_cast<unsigned char>(i));
            }
            enc16 = Rc4(key_i, enc16);
        }
        authenticated = enc16 == u.substr(0, 16);
    }
    if (!authenticated) return false;

    out->file_key = file_key;
    out->stream_method = stream_method;
    out->string_method = string_method;
    out->encrypt_obj_num = encrypt_obj_num;
    return true;
}

void DecryptObjectStrings(const EncryptionState *state, int num, int gen, pdfobj::Object *obj) {
    if (!state || num == state->encrypt_obj_num) return;
    if (obj->type == pdfobj::Type::String) {
        obj->str_val = DecryptWithMethod(state->file_key, num, gen, state->string_method, obj->str_val);
    } else if (obj->type == pdfobj::Type::Array) {
        for (pdfobj::Object &item : obj->array_val) DecryptObjectStrings(state, num, gen, &item);
    } else if (obj->type == pdfobj::Type::Dict) {
        for (auto &kv : obj->dict_val) DecryptObjectStrings(state, num, gen, &kv.second);
    }
}

std::string DecryptStreamBytes(const EncryptionState *state, int num, int gen, const std::string &data) {
    if (!state || num == state->encrypt_obj_num) return data;
    return DecryptWithMethod(state->file_key, num, gen, state->stream_method, data);
}

}  // namespace pdfcrypt
