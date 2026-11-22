#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mac_mirror {

// Volatile memory wipe resistant to dead-store elimination.
void SecureZeroBuffer(void* ptr, size_t len) noexcept;

// Cryptographically secure random byte generator (BCryptGenRandom on Windows, arc4random_buf on macOS).
bool FillSecureRandom(uint8_t* dst, size_t len);

// Reverses bit order within a single byte (MSB <-> LSB), used by standard RFB VncAuth DES keys.
constexpr uint8_t ReverseByteBits(uint8_t b) noexcept {
    b = static_cast<uint8_t>(((b & 0xF0u) >> 4) | ((b & 0x0Fu) << 4));
    b = static_cast<uint8_t>(((b & 0xCCu) >> 2) | ((b & 0x33u) << 2));
    b = static_cast<uint8_t>(((b & 0xAAu) >> 1) | ((b & 0x55u) << 1));
    return b;
}

// MD5 128-bit digest (uses Win32 CNG bcrypt.dll on Windows; portable RFC 1321 on POSIX).
bool Md5Hash(const uint8_t* data, size_t len, uint8_t out_digest[16]);

// Portable RFC 1321 MD5 exposed for cross-checking in unit tests.
void Md5HashPortable(const uint8_t* data, size_t len, uint8_t out_digest[16]) noexcept;

// AES-128-ECB encryption/decryption over `len` bytes (`len` MUST be a multiple of 16, no padding).
// Uses Win32 CNG bcrypt.dll on Windows; portable FIPS-197 implementation on POSIX.
bool Aes128EcbEncrypt(const uint8_t key[16],
                      const uint8_t* plaintext,
                      size_t len,
                      uint8_t* ciphertext);

bool Aes128EcbDecrypt(const uint8_t key[16],
                      const uint8_t* ciphertext,
                      size_t len,
                      uint8_t* plaintext);

// Big-endian unsigned modular exponentiation:
//   out_result = (base ^ exp) mod modulus
// serialized as a fixed `key_size`-byte big-endian buffer with leading zero padding.
bool ModExpBigEndian(const uint8_t* base_be,
                     size_t base_len,
                     const uint8_t* exp_be,
                     size_t exp_len,
                     const uint8_t* mod_be,
                     size_t key_size,
                     std::vector<uint8_t>* out_result_be);

// Standard RFB SecurityType 2 (VncAuth):
// Encrypts 16-byte server challenge with 8-byte bit-reversed DES-ECB password key.
bool EncryptVncAuthChallenge(const uint8_t challenge[16],
                             const std::string& password,
                             uint8_t out_response[16]);

// Apple Remote Desktop / macOS Screen Sharing Diffie-Hellman (SecurityType 30):
// Wire challenge from screensharingd:
//   uint16_t generator (BE, e.g. 5)
//   uint16_t key_size  (BE, e.g. 512 bytes = 4096-bit RFC 3526 Group 16)
//   uint8_t  prime[key_size]
//   uint8_t  server_pubkey[key_size]
// Wire response produced (`out_packet`, 128 + key_size bytes):
//   uint8_t  encrypted_credentials[128]  // AES-128-ECB(MD5(shared_secret), user[64] || pass[64])
//   uint8_t  client_pubkey[key_size]     // g^priv mod p (zero-padded big-endian)
// Optional `deterministic_priv_be` & `deterministic_pad_128` are for unit test vectors only.
bool BuildAppleDhResponse(uint16_t generator,
                          uint16_t key_size,
                          const uint8_t* prime_be,
                          const uint8_t* server_pubkey_be,
                          const std::string& username,
                          const std::string& password,
                          std::vector<uint8_t>* out_packet,
                          std::string* error_out = nullptr,
                          const std::vector<uint8_t>* deterministic_priv_be = nullptr,
                          const uint8_t* deterministic_pad_128 = nullptr);

}  // namespace mac_mirror
