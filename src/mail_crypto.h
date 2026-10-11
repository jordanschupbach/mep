#ifndef MEP_MAIL_CRYPTO_H
#define MEP_MAIL_CRYPTO_H

// The mail vault's primitives (mail_net.cpp's "vault" secret store): an
// encrypted file of account secrets for machines with no OS credential
// store (Linux without a Secret Service keyring), unlocked with a master
// passphrase. Standard constructions only, each checked against its RFC's
// test vectors in mail_test.cpp:
//
//   * ChaCha20-Poly1305 AEAD (RFC 8439) -- authenticated encryption, so a
//     wrong passphrase or a tampered file is detected, never decrypted to
//     garbage;
//   * PBKDF2-HMAC-SHA256 (RFC 8018 / RFC 7914 vectors) -- turns the
//     passphrase into the key, slowly on purpose;
//   * the OS random generator (getentropy / arc4random / BCryptGenRandom)
//     for salts, nonces and OAuth state.

#include <cstdint>
#include <string>
#include <string_view>

namespace mail {

/** @brief `n` bytes from the operating system's cryptographic random generator. */
std::string SecureRandom(size_t n);
/** @brief HMAC-SHA256 (RFC 2104). */
std::string HmacSha256(std::string_view key, std::string_view data);
/** @brief PBKDF2 with HMAC-SHA256: `length` bytes from `password`, `salt`, `iterations`. */
std::string Pbkdf2Sha256(std::string_view password, std::string_view salt, uint32_t iterations, size_t length);
/** @brief ChaCha20-Poly1305 encryption: ciphertext followed by the 16-byte tag. `key` 32 bytes, `nonce` 12. */
std::string AeadSeal(std::string_view key, std::string_view nonce, std::string_view plaintext, std::string_view aad);
/** @brief Verifies and decrypts AeadSeal's output; false (and `*plaintext` untouched) if anything was altered. */
bool AeadOpen(std::string_view key, std::string_view nonce, std::string_view sealed, std::string_view aad,
              std::string *plaintext);
/** @brief Overwrites a string's bytes before it is released (best effort against lingering secrets). */
void Wipe(std::string *s);

}  // namespace mail

#endif  // MEP_MAIL_CRYPTO_H
