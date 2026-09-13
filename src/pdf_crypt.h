#pragma once

// mep's own in-house PDF Standard Security Handler -- see
// PDFIUM_REMOVAL_PLAN.md Phase 12 (encryption, empty-password case).
// Computes the document-wide file encryption key (spec 7.6.3.3
// Algorithm 2) from an empty user password and authenticates it against
// the encryption dict's own /U entry (Algorithm 6); a real, non-empty
// user password fails that authentication, matching PDFium's own
// FPDF_ERR_PASSWORD outcome (this module is deliberately read-only:
// there's no way to *supply* a password here at all, since mep has no
// UI for one yet -- a real password-protected PDF is simply reported as
// unsupported, same as PDFium's own behavior when none is given).
//
// Scoping decision -- no OpenSSL: the plan's own original checklist text
// named "AES-128-CBC (via OpenSSL's EVP_aes_128_cbc)", but MD5/RC4/AES-
// 128 (decrypt-only -- this module never needs to encrypt) are all hand-
// rolled here instead, matching this codebase's own precedent throughout
// this entire plan of avoiding third-party dependencies wherever
// feasible (hand-rolled deflate/LZW, JPEG codec, TrueType/CFF outline
// engines, 2D rasterizer, font encoding tables) and Phase 10's own
// documented departure from its original "AFM-derived metrics" text when
// a better-fitting approach emerged. Pulling in OpenSSL here would also
// tie the entire new parser/renderer (already used standalone, on track
// for Phase 13's swap-in behind mep_core itself) to a "native-only"
// dependency (see CMakeLists.txt's own `if(NOT EMSCRIPTEN)` guard around
// OpenSSL, currently scoped to the collab/websocket subsystem only) for
// no real benefit -- none of these 3 primitives are performance-
// sensitive here (a handful of small documents' worth of strings/
// streams, not bulk data), and all 3 are small, extremely well-specified
// fixed algorithms with abundant independent test vectors (this module's
// own pdf_crypt_test.cpp verifies each against published vectors before
// ever touching a real PDF).

#include "pdf_object.h"

#include <string>

namespace pdfcrypt {

// -- Small standalone primitives (exposed mainly for unit testing against
// independent published test vectors; DecryptObjectStrings/
// DecryptStreamBytes below are the actual integration points). --

// RFC 1321 MD5, returning the raw 16-byte digest (not hex-encoded).
std::string Md5(const std::string &input);

// RC4 stream cipher (symmetric: the same function encrypts and
// decrypts). `key` must be non-empty.
std::string Rc4(const std::string &key, const std::string &data);

// Decrypts one 16-byte AES-128 block (FIPS-197 InvCipher, standard --
// not equivalent-inverse-cipher -- round order), exposed mainly to test
// the hand-rolled round transforms directly against a published single-
// block test vector, independent of AesCbcDecrypt's own CBC chaining/
// PKCS#7-unpadding wrapper around it.
void AesDecryptBlock(const std::string &key16, const unsigned char in[16], unsigned char out[16]);

// AES-128-CBC decryption with PKCS#7 unpadding (the PDF spec's own
// AESV2 crypt filter convention, spec 7.6.2: ciphertext is a 16-byte IV
// followed by the actual CBC-encrypted, PKCS#7-padded data -- callers
// pass just the ciphertext-after-IV part here, with `iv` split out
// separately). Returns false (leaving *out untouched) for a wrong key
// length/non-block-multiple ciphertext/invalid padding -- tolerated by
// the caller as "leave this string/stream's bytes as-is" rather than
// failing the whole document.
bool AesCbcDecrypt(const std::string &key16, const std::string &iv16, const std::string &ciphertext,
                    std::string *out);

// -- Standard Security Handler --------------------------------------

enum class CryptMethod {
    kIdentity,  // /Identity crypt filter, or an unrecognized /CFM: left unencrypted
    kRc4,
    kAesV2,
};

// The document-wide state SetupStandardSecurityHandler computes once:
// the derived file encryption key plus which cipher applies to strings
// vs. streams (only ever different from each other under a /V 4 crypt-
// filter dict; V1-3 always use RC4 for both).
struct EncryptionState {
    std::string file_key;
    CryptMethod stream_method = CryptMethod::kRc4;
    CryptMethod string_method = CryptMethod::kRc4;
    // The /Encrypt dictionary's own indirect object number, or -1 if it
    // was embedded directly in the trailer -- its own string values
    // (e.g. /O, /U) are never themselves encrypted (spec 7.6.1) and must
    // be excluded from decryption, even though they're perfectly valid
    // indirectly-referenced strings otherwise.
    int encrypt_obj_num = -1;
};

// Implements spec 7.6.3.3 Algorithm 2 (key derivation, empty user
// password stands in for the padded password directly) followed by
// Algorithm 6 (authenticate by recomputing /U and comparing) for
// standard-handler revisions 2-4 (RC4-40/128, or R4's /CF-selected RC4/
// AESV2 crypt filters). `id0` is the trailer's own /ID array's first
// element (raw bytes, unencrypted -- spec 7.6.3.3 step (b)).
// `encrypt_obj_num` is the /Encrypt dict's own object number (-1 if
// embedded directly in the trailer).
//
// Returns false (leaving *out untouched) if: `encrypt_dict` isn't
// `/Filter /Standard` (only the standard security handler is
// supported -- a custom/third-party one is out of scope); /R is 5 or 6
// (AES-256, /V 5 -- out of scope, a real Scoping decision: no target
// fixture or realistic use case for this codebase needs it, and it uses
// SHA-256-based key derivation rather than Algorithm 2 at all); or the
// empty password's derived key doesn't authenticate against /U -- the
// overwhelmingly common real-world outcome for this last case being a
// **real, non-empty user password**, which this module has no way to
// prompt for or supply, matching PDFium's own FPDF_ERR_PASSWORD result
// for the same situation.
bool SetupStandardSecurityHandler(const pdfobj::Object &encrypt_dict, const std::string &id0, int encrypt_obj_num,
                                   EncryptionState *out);

// Recursively decrypts every String-type value nested in `*obj` in
// place (Dict values and Array elements walked; Name/Number/Reference/
// Null left alone, since only actual PDF string objects are
// individually encrypted per spec) using `num`/`gen`'s own per-object
// key (spec 7.6.2 Algorithm 1). `state` may be null (document isn't
// encrypted, or its encryption couldn't be set up) -- a no-op in that
// case, so callers can call this unconditionally rather than checking
// first. Also a no-op when `num` is the Encrypt dictionary's own object
// number (spec 7.6.1's own carve-out).
void DecryptObjectStrings(const EncryptionState *state, int num, int gen, pdfobj::Object *obj);

// Decrypts one stream's already-extracted raw (not yet filter-decoded)
// bytes using `num`/`gen`'s own per-object key, returning the decrypted
// bytes (or `data` unchanged if `state` is null or `num` is the Encrypt
// dictionary's own object number).
std::string DecryptStreamBytes(const EncryptionState *state, int num, int gen, const std::string &data);

}  // namespace pdfcrypt
