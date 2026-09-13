// PDFIUM_REMOVAL_PLAN.md Phase 12 coverage for pdf_crypt.h/.cpp's hand-
// rolled MD5/RC4/AES-128 primitives and the Standard Security Handler's
// key derivation + authentication, each checked against a published or
// independently-generated (openssl CLI, used only to build test vectors
// here -- never linked into the module itself, see pdf_crypt.h's own
// "Scoping decision -- no OpenSSL") reference value before ever being
// trusted against a real PDF.

#include "pdf_crypt.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

std::string HexDecode(const std::string &hex) {
    std::string out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return 0;
        };
        out.push_back(static_cast<char>(static_cast<unsigned char>((nibble(hex[i]) << 4) | nibble(hex[i + 1]))));
    }
    return out;
}

std::string HexEncode(const std::string &data) {
    static const char *kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (char ch : data) {
        unsigned char c = static_cast<unsigned char>(ch);
        out.push_back(kDigits[c >> 4]);
        out.push_back(kDigits[c & 0xF]);
    }
    return out;
}

// -- RFC 1321 Appendix A.5's own published MD5 test suite --
void TestMd5KnownVectors() {
    CHECK(HexEncode(pdfcrypt::Md5("")) == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(HexEncode(pdfcrypt::Md5("abc")) == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(HexEncode(pdfcrypt::Md5("message digest")) == "f96b697d7cb7938d525a2f31aaf161d0");
    CHECK(HexEncode(pdfcrypt::Md5("abcdefghijklmnopqrstuvwxyz")) == "c3fcd3d76192e4007dfb496cca67e13b");
}

// -- The classic "Key"/"Plaintext" RC4 test vector (widely published,
// e.g. Wikipedia's RC4 article and countless implementation test
// suites) -- also independently re-derived via a from-scratch Python
// RC4 during this phase's own development, both agreeing on
// BBF316E8D940AF0AD3.
void TestRc4KnownVector() {
    std::string ct = pdfcrypt::Rc4("Key", "Plaintext");
    CHECK(HexEncode(ct) == "bbf316e8d940af0ad3");
    // RC4 is symmetric: decrypting the ciphertext with the same key
    // reproduces the plaintext.
    CHECK(pdfcrypt::Rc4("Key", ct) == "Plaintext");
}

// -- FIPS-197 Appendix C.1's own published AES-128 test vector (single
// block, no chaining/padding -- exercises AesDecryptBlock's round
// transforms directly, independent of AesCbcDecrypt's own CBC/PKCS#7
// wrapper) -- cross-confirmed independently via `openssl enc -aes-128-
// ecb -nopad` during this phase's own development.
void TestAesDecryptBlockFipsVector() {
    std::string key = HexDecode("000102030405060708090a0b0c0d0e0f");
    std::string ciphertext = HexDecode("69c4e0d86a7b0430d8cdb78070b4c55a");
    std::string expected_plaintext = HexDecode("00112233445566778899aabbccddeeff");
    unsigned char out[16];
    pdfcrypt::AesDecryptBlock(key, reinterpret_cast<const unsigned char *>(ciphertext.data()), out);
    CHECK(std::string(reinterpret_cast<char *>(out), 16) == expected_plaintext);
}

// -- AES-128-CBC + PKCS#7 unpadding, cross-checked against `openssl enc
// -aes-128-cbc` as an independent oracle (key/iv/plaintext chosen by
// hand, ciphertext is that command's own real output -- see this
// phase's own PDFIUM_REMOVAL_PLAN.md writeup for the exact invocation).
void TestAesCbcDecryptMatchesOpensslOracle() {
    std::string key = HexDecode("0123456789abcdef0123456789abcdef");
    std::string iv = HexDecode("000102030405060708090a0b0c0d0e0f");
    std::string ciphertext = HexDecode(
        "6517ea437bb37304796fe951c2a7678e65a7705bd4c8c6397d6cf3b3ea13da6693a3c721ad88124453347b7dd2b5ce81");
    std::string plain;
    CHECK(pdfcrypt::AesCbcDecrypt(key, iv, ciphertext, &plain));
    CHECK(plain == "Hello, AES-CBC world! This is a test.");
}

void TestAesCbcDecryptRejectsCorruptPadding() {
    std::string key = HexDecode("0123456789abcdef0123456789abcdef");
    std::string iv = HexDecode("000102030405060708090a0b0c0d0e0f");
    std::string ciphertext = HexDecode("00000000000000000000000000000000");  // random garbage: near-certainly bad padding
    std::string plain;
    CHECK(!pdfcrypt::AesCbcDecrypt(key, iv, ciphertext, &plain));
}

// -- Standard Security Handler: build an /Encrypt dict by hand for a
// revision-3 (128-bit RC4), empty-user-password document, using this
// phase's own now-vector-verified Md5/Rc4 to independently compute what
// /O and /U "should" be (the same computation SetupStandardSecurityHandler
// itself performs internally -- this test's value is confirming the
// *wiring* -- Length/P/ID/R handling, the dict layout -- not re-deriving
// the algorithm a second, differently-buggy way).
pdfobj::Object MakeString(const std::string &s) {
    pdfobj::Object o;
    o.type = pdfobj::Type::String;
    o.str_val = s;
    return o;
}
pdfobj::Object MakeName(const std::string &s) {
    pdfobj::Object o;
    o.type = pdfobj::Type::Name;
    o.str_val = s;
    return o;
}
pdfobj::Object MakeInt(long long v) {
    pdfobj::Object o;
    o.type = pdfobj::Type::Int;
    o.int_val = v;
    return o;
}

void TestSetupStandardSecurityHandlerR3Rc4() {
    std::string id0 = "0123456789abcdef";
    long long p = -3904;  // an arbitrary, realistic-looking permissions bitmask
    int key_len_bytes = 16;

    // Owner password also empty in this fixture -- /O is then just the
    // padding string RC4'd with the (owner-)key derived the same way as
    // the user key when both passwords are empty; a real PDF always has
    // *some* /O, its own exact derivation isn't part of Algorithm 2/6 at
    // all (Algorithm 3 governs /O and isn't needed to authenticate a
    // user password), so any 32-byte string works here -- Algorithm 6
    // only ever feeds /O into the *user* key derivation as opaque bytes.
    std::string o(32, '\0');
    for (size_t i = 0; i < 32; ++i) o[i] = static_cast<char>(static_cast<unsigned char>(i));

    // Independently reproduce Algorithm 2 + the R>=3 50-round stretch
    // and Algorithm 6's 19-round U computation, using only the already-
    // vector-verified Md5/Rc4 primitives (not SetupStandardSecurityHandler
    // itself), to get the /U this fixture's dict must contain.
    unsigned char p_bytes[4] = {
        static_cast<unsigned char>(p & 0xFF),
        static_cast<unsigned char>((p >> 8) & 0xFF),
        static_cast<unsigned char>((p >> 16) & 0xFF),
        static_cast<unsigned char>((p >> 24) & 0xFF),
    };
    std::string pad(
        "\x28\xBF\x4E\x5E\x4E\x75\x8A\x41\x64\x00\x4E\x56\xFF\xFA\x01\x08"
        "\x2E\x2E\x00\xB6\xD0\x68\x3E\x80\x2F\x0C\xA9\xFE\x64\x53\x69\x7A",
        32);
    std::string input = pad + o + std::string(reinterpret_cast<char *>(p_bytes), 4) + id0;
    std::string digest = pdfcrypt::Md5(input);
    for (int i = 0; i < 50; ++i) digest = pdfcrypt::Md5(digest.substr(0, static_cast<size_t>(key_len_bytes)));
    std::string file_key = digest.substr(0, static_cast<size_t>(key_len_bytes));

    std::string enc16 = pdfcrypt::Rc4(file_key, pdfcrypt::Md5(pad + id0));
    for (int i = 1; i <= 19; ++i) {
        std::string key_i = file_key;
        for (char &c : key_i) c = static_cast<char>(static_cast<unsigned char>(c) ^ static_cast<unsigned char>(i));
        enc16 = pdfcrypt::Rc4(key_i, enc16);
    }
    std::string u = enc16 + std::string(16, '\0');  // last 16 bytes of /U are arbitrary padding, unchecked by R>=3

    pdfobj::Object dict;
    dict.type = pdfobj::Type::Dict;
    dict.dict_val["Filter"] = MakeName("Standard");
    dict.dict_val["V"] = MakeInt(2);
    dict.dict_val["R"] = MakeInt(3);
    dict.dict_val["O"] = MakeString(o);
    dict.dict_val["U"] = MakeString(u);
    dict.dict_val["P"] = MakeInt(p);
    dict.dict_val["Length"] = MakeInt(128);

    pdfcrypt::EncryptionState state;
    CHECK(pdfcrypt::SetupStandardSecurityHandler(dict, id0, 7, &state));
    CHECK(state.file_key == file_key);
    CHECK(state.stream_method == pdfcrypt::CryptMethod::kRc4);
    CHECK(state.string_method == pdfcrypt::CryptMethod::kRc4);
    CHECK(state.encrypt_obj_num == 7);

    // A wrong /U (simulating a real, non-empty user password) must fail
    // authentication -- matching PDFium's own FPDF_ERR_PASSWORD outcome.
    pdfobj::Object bad_dict = dict;
    std::string bad_u = u;
    bad_u[0] = static_cast<char>(static_cast<unsigned char>(bad_u[0]) ^ 0xFF);
    bad_dict.dict_val["U"] = MakeString(bad_u);
    pdfcrypt::EncryptionState bad_state;
    CHECK(!pdfcrypt::SetupStandardSecurityHandler(bad_dict, id0, 7, &bad_state));
}

void TestSetupStandardSecurityHandlerRejectsUnsupported() {
    pdfobj::Object dict;
    dict.type = pdfobj::Type::Dict;
    dict.dict_val["Filter"] = MakeName("Standard");
    dict.dict_val["V"] = MakeInt(5);
    dict.dict_val["R"] = MakeInt(6);  // AES-256: explicitly out of scope
    dict.dict_val["O"] = MakeString(std::string(48, 'x'));
    dict.dict_val["U"] = MakeString(std::string(48, 'y'));
    pdfcrypt::EncryptionState state;
    CHECK(!pdfcrypt::SetupStandardSecurityHandler(dict, "id", 3, &state));

    pdfobj::Object non_standard;
    non_standard.type = pdfobj::Type::Dict;
    non_standard.dict_val["Filter"] = MakeName("SomeThirdPartyHandler");
    CHECK(!pdfcrypt::SetupStandardSecurityHandler(non_standard, "id", 3, &state));
}

// -- DecryptObjectStrings/DecryptStreamBytes: the Encrypt dict's own
// object number must never be decrypted (spec 7.6.1), everything else
// must be.
void TestDecryptSkipsEncryptDictObjectNumber() {
    pdfcrypt::EncryptionState state;
    state.file_key = "abcde";  // 5 bytes -> 40-bit RC4, arbitrary for this wiring test
    state.stream_method = pdfcrypt::CryptMethod::kRc4;
    state.string_method = pdfcrypt::CryptMethod::kRc4;
    state.encrypt_obj_num = 9;

    pdfobj::Object dict;
    dict.type = pdfobj::Type::Dict;
    dict.dict_val["A"] = MakeString("secret");
    pdfobj::Object arr;
    arr.type = pdfobj::Type::Array;
    arr.array_val.push_back(MakeString("nested"));
    dict.dict_val["B"] = arr;

    pdfobj::Object copy_for_obj9 = dict;
    pdfcrypt::DecryptObjectStrings(&state, 9, 0, &copy_for_obj9);
    CHECK(copy_for_obj9.dict_val["A"].str_val == "secret");             // unchanged: this IS the Encrypt dict
    CHECK(copy_for_obj9.dict_val["B"].array_val[0].str_val == "nested");  // unchanged too

    pdfobj::Object copy_for_obj10 = dict;
    pdfcrypt::DecryptObjectStrings(&state, 10, 0, &copy_for_obj10);
    CHECK(copy_for_obj10.dict_val["A"].str_val != "secret");  // actually ran through RC4
    CHECK(copy_for_obj10.dict_val["B"].array_val[0].str_val != "nested");

    std::string stream_data = "raw stream bytes";
    CHECK(pdfcrypt::DecryptStreamBytes(&state, 9, 0, stream_data) == stream_data);  // Encrypt dict object: unchanged
    CHECK(pdfcrypt::DecryptStreamBytes(&state, 10, 0, stream_data) != stream_data);

    // Null state (unencrypted document): always a no-op.
    pdfobj::Object copy_null = dict;
    pdfcrypt::DecryptObjectStrings(nullptr, 10, 0, &copy_null);
    CHECK(copy_null.dict_val["A"].str_val == "secret");
    CHECK(pdfcrypt::DecryptStreamBytes(nullptr, 10, 0, stream_data) == stream_data);
}

}  // namespace

int main() {
    TestMd5KnownVectors();
    TestRc4KnownVector();
    TestAesDecryptBlockFipsVector();
    TestAesCbcDecryptMatchesOpensslOracle();
    TestAesCbcDecryptRejectsCorruptPadding();
    TestSetupStandardSecurityHandlerR3Rc4();
    TestSetupStandardSecurityHandlerRejectsUnsupported();
    TestDecryptSkipsEncryptDictObjectNumber();
    std::printf("pdf_crypt_test: all checks passed\n");
    return 0;
}
