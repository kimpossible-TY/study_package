#include "rfb_crypto.h"

#include <algorithm>
#include <array>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <stdlib.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace mac_mirror {

void SecureZeroBuffer(void* ptr, size_t len) noexcept {
    if (!ptr || len == 0) return;
#ifdef _WIN32
    SecureZeroMemory(ptr, len);
#else
    volatile uint8_t* p = static_cast<volatile uint8_t*>(ptr);
    for (size_t i = 0; i < len; ++i) {
        p[i] = 0;
    }
#endif
}

bool FillSecureRandom(uint8_t* dst, size_t len) {
    if (len == 0) return true;
    if (!dst) return false;
#ifdef _WIN32
    NTSTATUS status = ::BCryptGenRandom(
        nullptr, dst, static_cast<ULONG>(len), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return status >= 0;
#elif defined(__APPLE__)
    ::arc4random_buf(dst, len);
    return true;
#else
    int fd = ::open("/dev/urandom", O_RDONLY);
    if (fd < 0) return false;
    size_t done = 0;
    while (done < len) {
        ssize_t n = ::read(fd, dst + done, len - done);
        if (n <= 0) {
            ::close(fd);
            return false;
        }
        done += static_cast<size_t>(n);
    }
    ::close(fd);
    return true;
#endif
}

namespace {

// ============================================================================
// Portable RFC 1321 MD5
// ============================================================================
constexpr uint32_t RotL32(uint32_t x, uint32_t c) noexcept {
    return (x << c) | (x >> (32u - c));
}

constexpr uint32_t LoadLE32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline void StoreLE32(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

void Md5CompressBlock(uint32_t state[4], const uint8_t block[64]) noexcept {
    static constexpr uint8_t kShift[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
    };
    static constexpr uint32_t kK[64] = {
        0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
        0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
        0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
        0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
        0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
        0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
        0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
        0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
        0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
        0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
        0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
        0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
        0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
        0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
        0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
        0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u,
    };

    uint32_t m[16];
    for (int i = 0; i < 16; ++i) {
        m[i] = LoadLE32(block + i * 4);
    }

    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];

    for (uint32_t i = 0; i < 64; ++i) {
        uint32_t f = 0;
        uint32_t g = 0;
        if (i < 16) {
            f = (b & c) | ((~b) & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | ((~d) & c);
            g = (5u * i + 1u) & 15u;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3u * i + 5u) & 15u;
        } else {
            f = c ^ (b | (~d));
            g = (7u * i) & 15u;
        }
        uint32_t tmp = d;
        d = c;
        c = b;
        b = b + RotL32(a + f + kK[i] + m[g], kShift[i]);
        a = tmp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    SecureZeroBuffer(m, sizeof(m));
}

#ifndef _WIN32
// ============================================================================
// Portable FIPS-197 AES-128-ECB (fallback for POSIX & unit tests)
// ============================================================================
static constexpr uint8_t kAesSBox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

static constexpr uint8_t kAesInvSBox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d,
};

constexpr uint8_t GfMul2(uint8_t x) noexcept {
    return static_cast<uint8_t>((x << 1) ^ ((x & 0x80u) ? 0x1Bu : 0x00u));
}

constexpr uint8_t GfMul(uint8_t a, uint8_t b) noexcept {
    uint8_t p = 0;
    for (int i = 0; i < 8; ++i) {
        if (b & 1u) p ^= a;
        a = GfMul2(a);
        b >>= 1;
    }
    return p;
}

void Aes128ExpandKey(const uint8_t key[16], uint8_t round_keys[176]) noexcept {
    static constexpr uint8_t kRcon[10] = {
        0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36
    };
    std::memcpy(round_keys, key, 16);
    for (int i = 4; i < 44; ++i) {
        uint8_t tmp[4];
        std::memcpy(tmp, round_keys + (i - 1) * 4, 4);
        if (i % 4 == 0) {
            uint8_t t = tmp[0];
            tmp[0] = kAesSBox[tmp[1]] ^ kRcon[(i / 4) - 1];
            tmp[1] = kAesSBox[tmp[2]];
            tmp[2] = kAesSBox[tmp[3]];
            tmp[3] = kAesSBox[t];
        }
        for (int j = 0; j < 4; ++j) {
            round_keys[i * 4 + j] = round_keys[(i - 4) * 4 + j] ^ tmp[j];
        }
    }
}

void Aes128EncryptBlockPortable(const uint8_t round_keys[176],
                                const uint8_t in[16],
                                uint8_t out[16]) noexcept {
    uint8_t s[16];
    for (int i = 0; i < 16; ++i) s[i] = in[i] ^ round_keys[i];

    for (int round = 1; round <= 10; ++round) {
        for (int i = 0; i < 16; ++i) s[i] = kAesSBox[s[i]];

        // ShiftRows (column-major: index = col * 4 + row)
        uint8_t t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;
        t = s[2]; s[2] = s[10]; s[10] = t;
        t = s[6]; s[6] = s[14]; s[14] = t;
        t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;

        if (round < 10) {
            for (int c = 0; c < 4; ++c) {
                uint8_t* col = s + c * 4;
                uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                uint8_t xr = a0 ^ a1 ^ a2 ^ a3;
                col[0] ^= xr ^ GfMul2(a0 ^ a1);
                col[1] ^= xr ^ GfMul2(a1 ^ a2);
                col[2] ^= xr ^ GfMul2(a2 ^ a3);
                col[3] ^= xr ^ GfMul2(a3 ^ a0);
            }
        }

        const uint8_t* rk = round_keys + round * 16;
        for (int i = 0; i < 16; ++i) s[i] ^= rk[i];
    }
    std::memcpy(out, s, 16);
    SecureZeroBuffer(s, sizeof(s));
}

void Aes128DecryptBlockPortable(const uint8_t round_keys[176],
                                const uint8_t in[16],
                                uint8_t out[16]) noexcept {
    uint8_t s[16];
    for (int i = 0; i < 16; ++i) s[i] = in[i] ^ round_keys[160 + i];

    for (int round = 9; round >= 0; --round) {
        // InvShiftRows
        uint8_t t = s[13]; s[13] = s[9]; s[9] = s[5]; s[5] = s[1]; s[1] = t;
        t = s[2]; s[2] = s[10]; s[10] = t;
        t = s[6]; s[6] = s[14]; s[14] = t;
        t = s[3]; s[3] = s[7]; s[7] = s[11]; s[11] = s[15]; s[15] = t;

        for (int i = 0; i < 16; ++i) s[i] = kAesInvSBox[s[i]];

        const uint8_t* rk = round_keys + round * 16;
        for (int i = 0; i < 16; ++i) s[i] ^= rk[i];

        if (round > 0) {
            for (int c = 0; c < 4; ++c) {
                uint8_t* col = s + c * 4;
                uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                col[0] = GfMul(a0, 0x0e) ^ GfMul(a1, 0x0b) ^ GfMul(a2, 0x0d) ^ GfMul(a3, 0x09);
                col[1] = GfMul(a0, 0x09) ^ GfMul(a1, 0x0e) ^ GfMul(a2, 0x0b) ^ GfMul(a3, 0x0d);
                col[2] = GfMul(a0, 0x0d) ^ GfMul(a1, 0x09) ^ GfMul(a2, 0x0e) ^ GfMul(a3, 0x0b);
                col[3] = GfMul(a0, 0x0b) ^ GfMul(a1, 0x0d) ^ GfMul(a2, 0x09) ^ GfMul(a3, 0x0e);
            }
        }
    }
    std::memcpy(out, s, 16);
    SecureZeroBuffer(s, sizeof(s));
}
#endif  // !_WIN32

// ============================================================================
// FIPS-46-3 DES (64-bit ECB block cipher for standard RFB VncAuth SecurityType 2)
// ============================================================================
static constexpr uint8_t kDesIP[64] = {
    58,50,42,34,26,18,10, 2, 60,52,44,36,28,20,12, 4,
    62,54,46,38,30,22,14, 6, 64,56,48,40,32,24,16, 8,
    57,49,41,33,25,17, 9, 1, 59,51,43,35,27,19,11, 3,
    61,53,45,37,29,21,13, 5, 63,55,47,39,31,23,15, 7,
};

static constexpr uint8_t kDesFP[64] = {
    40, 8,48,16,56,24,64,32, 39, 7,47,15,55,23,63,31,
    38, 6,46,14,54,22,62,30, 37, 5,45,13,53,21,61,29,
    36, 4,44,12,52,20,60,28, 35, 3,43,11,51,19,59,27,
    34, 2,42,10,50,18,58,26, 33, 1,41, 9,49,17,57,25,
};

static constexpr uint8_t kDesE[48] = {
    32, 1, 2, 3, 4, 5,  4, 5, 6, 7, 8, 9,
     8, 9,10,11,12,13, 12,13,14,15,16,17,
    16,17,18,19,20,21, 20,21,22,23,24,25,
    24,25,26,27,28,29, 28,29,30,31,32, 1,
};

static constexpr uint8_t kDesP[32] = {
    16, 7,20,21,29,12,28,17,  1,15,23,26, 5,18,31,10,
     2, 8,24,14,32,27, 3, 9, 19,13,30, 6,22,11, 4,25,
};

static constexpr uint8_t kDesPC1[56] = {
    57,49,41,33,25,17, 9,  1,58,50,42,34,26,18,
    10, 2,59,51,43,35,27, 19,11, 3,60,52,44,36,
    63,55,47,39,31,23,15,  7,62,54,46,38,30,22,
    14, 6,61,53,45,37,29, 21,13, 5,28,20,12, 4,
};

static constexpr uint8_t kDesPC2[48] = {
    14,17,11,24, 1, 5,  3,28,15, 6,21,10,
    23,19,12, 4,26, 8, 16, 7,27,20,13, 2,
    41,52,31,37,47,55, 30,40,51,45,33,48,
    44,49,39,56,34,53, 46,42,50,36,29,32,
};

static constexpr uint8_t kDesShifts[16] = {
    1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1
};

static constexpr uint8_t kDesS[8][64] = {
    {
        14, 4,13, 1, 2,15,11, 8, 3,10, 6,12, 5, 9, 0, 7,
         0,15, 7, 4,14, 2,13, 1,10, 6,12,11, 9, 5, 3, 8,
         4, 1,14, 8,13, 6, 2,11,15,12, 9, 7, 3,10, 5, 0,
        15,12, 8, 2, 4, 9, 1, 7, 5,11, 3,14,10, 0, 6,13,
    },
    {
        15, 1, 8,14, 6,11, 3, 4, 9, 7, 2,13,12, 0, 5,10,
         3,13, 4, 7,15, 2, 8,14,12, 0, 1,10, 6, 9,11, 5,
         0,14, 7,11,10, 4,13, 1, 5, 8,12, 6, 9, 3, 2,15,
        13, 8,10, 1, 3,15, 4, 2,11, 6, 7,12, 0, 5,14, 9,
    },
    {
        10, 0, 9,14, 6, 3,15, 5, 1,13,12, 7,11, 4, 2, 8,
        13, 7, 0, 9, 3, 4, 6,10, 2, 8, 5,14,12,11,15, 1,
        13, 6, 4, 9, 8,15, 3, 0,11, 1, 2,12, 5,10,14, 7,
         1,10,13, 0, 6, 9, 8, 7, 4,15,14, 3,11, 5, 2,12,
    },
    {
         7,13,14, 3, 0, 6, 9,10, 1, 2, 8, 5,11,12, 4,15,
        13, 8,11, 5, 6,15, 0, 3, 4, 7, 2,12, 1,10,14, 9,
        10, 6, 9, 0,12,11, 7,13,15, 1, 3,14, 5, 2, 8, 4,
         3,15, 0, 6,10, 1,13, 8, 9, 4, 5,11,12, 7, 2,14,
    },
    {
         2,12, 4, 1, 7,10,11, 6, 8, 5, 3,15,13, 0,14, 9,
        14,11, 2,12, 4, 7,13, 1, 5, 0,15,10, 3, 9, 8, 6,
         4, 2, 1,11,10,13, 7, 8,15, 9,12, 5, 6, 3, 0,14,
        11, 8,12, 7, 1,14, 2,13, 6,15, 0, 9,10, 4, 5, 3,
    },
    {
        12, 1,10,15, 9, 2, 6, 8, 0,13, 3, 4,14, 7, 5,11,
        10,15, 4, 2, 7,12, 9, 5, 6, 1,13,14, 0,11, 3, 8,
         9,14,15, 5, 2, 8,12, 3, 7, 0, 4,10, 1,13,11, 6,
         4, 3, 2,12, 9, 5,15,10,11,14, 1, 7, 6, 0, 8,13,
    },
    {
         4,11, 2,14,15, 0, 8,13, 3,12, 9, 7, 5,10, 6, 1,
        13, 0,11, 7, 4, 9, 1,10,14, 3, 5,12, 2,15, 8, 6,
         1, 4,11,13,12, 3, 7,14,10,15, 6, 8, 0, 5, 9, 2,
         6,11,13, 8, 1, 4,10, 7, 9, 5, 0,15,14, 2, 3,12,
    },
    {
        13, 2, 8, 4, 6,15,11, 1,10, 9, 3,14, 5, 0,12, 7,
         1,15,13, 8,10, 3, 7, 4,12, 5, 6,11, 0,14, 9, 2,
         7,11, 4, 1, 9,12,14, 2, 0, 6,10,13,15, 3, 5, 8,
         2, 1,14, 7, 4,10, 8,13,15,12, 9, 0, 3, 5, 6,11,
    },
};

constexpr uint64_t PermuteBits(uint64_t in,
                               int in_width,
                               const uint8_t* table,
                               int out_width) noexcept {
    uint64_t out = 0;
    for (int i = 0; i < out_width; ++i) {
        int bit_pos = in_width - static_cast<int>(table[i]);
        out = (out << 1) | ((in >> bit_pos) & 1ULL);
    }
    return out;
}

void DesGenerateSubkeys(const uint8_t key8[8], uint64_t subkeys[16]) noexcept {
    uint64_t k64 = 0;
    for (int i = 0; i < 8; ++i) {
        k64 = (k64 << 8) | static_cast<uint64_t>(key8[i]);
    }
    uint64_t k56 = PermuteBits(k64, 64, kDesPC1, 56);
    uint32_t c = static_cast<uint32_t>((k56 >> 28) & 0x0FFFFFFFu);
    uint32_t d = static_cast<uint32_t>(k56 & 0x0FFFFFFFu);

    for (int r = 0; r < 16; ++r) {
        uint8_t sh = kDesShifts[r];
        c = ((c << sh) | (c >> (28 - sh))) & 0x0FFFFFFFu;
        d = ((d << sh) | (d >> (28 - sh))) & 0x0FFFFFFFu;
        uint64_t cd = (static_cast<uint64_t>(c) << 28) | static_cast<uint64_t>(d);
        subkeys[r] = PermuteBits(cd, 56, kDesPC2, 48);
    }
}

void DesEncryptBlock(const uint64_t subkeys[16],
                     const uint8_t in8[8],
                     uint8_t out8[8]) noexcept {
    uint64_t block = 0;
    for (int i = 0; i < 8; ++i) {
        block = (block << 8) | static_cast<uint64_t>(in8[i]);
    }
    uint64_t ip = PermuteBits(block, 64, kDesIP, 64);
    uint32_t l = static_cast<uint32_t>((ip >> 32) & 0xFFFFFFFFu);
    uint32_t r = static_cast<uint32_t>(ip & 0xFFFFFFFFu);

    for (int round = 0; round < 16; ++round) {
        uint64_t er = PermuteBits(r, 32, kDesE, 48) ^ subkeys[round];
        uint32_t s_out = 0;
        for (int sb = 0; sb < 8; ++sb) {
            uint8_t chunk = static_cast<uint8_t>((er >> (42 - 6 * sb)) & 0x3Fu);
            uint8_t row = static_cast<uint8_t>(((chunk & 0x20u) >> 4) | (chunk & 0x01u));
            uint8_t col = static_cast<uint8_t>((chunk >> 1) & 0x0Fu);
            s_out = (s_out << 4) | static_cast<uint32_t>(kDesS[sb][row * 16 + col]);
        }
        uint32_t f = static_cast<uint32_t>(PermuteBits(s_out, 32, kDesP, 32));
        uint32_t next_l = r;
        r = l ^ f;
        l = next_l;
    }

    uint64_t rl = (static_cast<uint64_t>(r) << 32) | static_cast<uint64_t>(l);
    uint64_t fp = PermuteBits(rl, 64, kDesFP, 64);
    for (int i = 7; i >= 0; --i) {
        out8[i] = static_cast<uint8_t>(fp & 0xFFu);
        fp >>= 8;
    }
}

// ============================================================================
// Big-Integer Montgomery Modular Exponentiation (up to 16384 bits)
// ============================================================================
int CompareLimbs(const uint32_t* a, const uint32_t* b, size_t n) noexcept {
    for (size_t i = n; i > 0; --i) {
        if (a[i - 1] != b[i - 1]) {
            return (a[i - 1] > b[i - 1]) ? 1 : -1;
        }
    }
    return 0;
}

void SubLimbsInPlace(uint32_t* a, const uint32_t* b, size_t n) noexcept {
    uint64_t borrow = 0;
    for (size_t i = 0; i < n; ++i) {
        uint64_t av = a[i];
        uint64_t bv = static_cast<uint64_t>(b[i]) + borrow;
        if (av >= bv) {
            a[i] = static_cast<uint32_t>(av - bv);
            borrow = 0;
        } else {
            a[i] = static_cast<uint32_t>((1ULL << 32) + av - bv);
            borrow = 1;
        }
    }
}

// Doubles `r` (n limbs) modulo `mod` (n limbs), assuming `r < mod`.
void DoubleModInPlace(uint32_t* r, const uint32_t* mod, size_t n) noexcept {
    uint32_t top_carry = 0;
    for (size_t i = 0; i < n; ++i) {
        uint32_t next_carry = r[i] >> 31;
        r[i] = (r[i] << 1) | top_carry;
        top_carry = next_carry;
    }
    if (top_carry != 0 || CompareLimbs(r, mod, n) >= 0) {
        SubLimbsInPlace(r, mod, n);
    }
}

// Reduces an arbitrary big-endian byte array modulo `mod` (`n` limbs) via bitwise Horner scan.
void ReduceBigEndianMod(const uint8_t* bytes_be,
                        size_t byte_len,
                        const uint32_t* mod,
                        size_t n,
                        uint32_t* out_rem) noexcept {
    std::fill(out_rem, out_rem + n, 0u);
    for (size_t bi = 0; bi < byte_len; ++bi) {
        uint8_t b = bytes_be[bi];
        for (int bit = 7; bit >= 0; --bit) {
            DoubleModInPlace(out_rem, mod, n);
            if ((b >> bit) & 1u) {
                uint64_t carry = 1;
                for (size_t i = 0; i < n && carry != 0; ++i) {
                    uint64_t sum = static_cast<uint64_t>(out_rem[i]) + carry;
                    out_rem[i] = static_cast<uint32_t>(sum);
                    carry = sum >> 32;
                }
                if (carry != 0 || CompareLimbs(out_rem, mod, n) >= 0) {
                    SubLimbsInPlace(out_rem, mod, n);
                }
            }
        }
    }
}

// Computes -m0^{-1} mod 2^32 for odd m0 via Newton-Raphson.
uint32_t ComputeMontN0Inv(uint32_t m0) noexcept {
    uint32_t inv = 1;
    for (int i = 0; i < 5; ++i) {
        inv *= 2u - m0 * inv;
    }
    return 0u - inv;
}

// CIOS Montgomery multiplication: out = a * b * R^{-1} mod m
void MontMul(const uint32_t* a,
             const uint32_t* b,
             const uint32_t* mod,
             size_t n,
             uint32_t n0_inv,
             uint32_t* t_scratch,  // length >= n + 2
             uint32_t* out) noexcept {
    std::fill(t_scratch, t_scratch + n + 2, 0u);
    for (size_t i = 0; i < n; ++i) {
        uint64_t carry = 0;
        for (size_t j = 0; j < n; ++j) {
            uint64_t sum = static_cast<uint64_t>(t_scratch[j]) +
                           static_cast<uint64_t>(a[i]) * static_cast<uint64_t>(b[j]) +
                           carry;
            t_scratch[j] = static_cast<uint32_t>(sum);
            carry = sum >> 32;
        }
        uint64_t sum_hi = static_cast<uint64_t>(t_scratch[n]) + carry;
        t_scratch[n] = static_cast<uint32_t>(sum_hi);
        t_scratch[n + 1] = static_cast<uint32_t>(sum_hi >> 32);

        uint32_t m = t_scratch[0] * n0_inv;
        uint64_t rcarry = (static_cast<uint64_t>(t_scratch[0]) +
                           static_cast<uint64_t>(m) * static_cast<uint64_t>(mod[0])) >>
                          32;
        for (size_t j = 1; j < n; ++j) {
            uint64_t rsum = static_cast<uint64_t>(t_scratch[j]) +
                            static_cast<uint64_t>(m) * static_cast<uint64_t>(mod[j]) +
                            rcarry;
            t_scratch[j - 1] = static_cast<uint32_t>(rsum);
            rcarry = rsum >> 32;
        }
        uint64_t rsum_n = static_cast<uint64_t>(t_scratch[n]) + rcarry;
        t_scratch[n - 1] = static_cast<uint32_t>(rsum_n);
        t_scratch[n] = t_scratch[n + 1] + static_cast<uint32_t>(rsum_n >> 32);
        t_scratch[n + 1] = 0;
    }

    if (t_scratch[n] != 0 || CompareLimbs(t_scratch, mod, n) >= 0) {
        SubLimbsInPlace(t_scratch, mod, n);
    }
    std::memcpy(out, t_scratch, n * sizeof(uint32_t));
}

// Fallback shift-and-add modular multiplication for even moduli.
void ModMulEvenFallback(const uint32_t* a,
                        const uint32_t* b,
                        const uint32_t* mod,
                        size_t n,
                        uint32_t* out) noexcept {
    std::vector<uint32_t> acc(n, 0u);
    for (size_t i = n; i > 0; --i) {
        uint32_t word = b[i - 1];
        for (int bit = 31; bit >= 0; --bit) {
            DoubleModInPlace(acc.data(), mod, n);
            if ((word >> bit) & 1u) {
                uint64_t carry = 0;
                for (size_t j = 0; j < n; ++j) {
                    uint64_t sum = static_cast<uint64_t>(acc[j]) +
                                   static_cast<uint64_t>(a[j]) + carry;
                    acc[j] = static_cast<uint32_t>(sum);
                    carry = sum >> 32;
                }
                if (carry != 0 || CompareLimbs(acc.data(), mod, n) >= 0) {
                    SubLimbsInPlace(acc.data(), mod, n);
                }
            }
        }
    }
    std::memcpy(out, acc.data(), n * sizeof(uint32_t));
    SecureZeroBuffer(acc.data(), acc.size() * sizeof(uint32_t));
}

}  // namespace

void Md5HashPortable(const uint8_t* data, size_t len, uint8_t out_digest[16]) noexcept {
    uint32_t state[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
    size_t full_blocks = len / 64;
    for (size_t i = 0; i < full_blocks; ++i) {
        Md5CompressBlock(state, data + i * 64);
    }

    uint8_t tail[128];
    std::memset(tail, 0, sizeof(tail));
    size_t rem = len - full_blocks * 64;
    if (rem > 0) {
        std::memcpy(tail, data + full_blocks * 64, rem);
    }
    tail[rem] = 0x80u;
    size_t pad_blocks = (rem >= 56) ? 2 : 1;
    uint64_t bit_len = static_cast<uint64_t>(len) * 8ULL;
    StoreLE32(tail + pad_blocks * 64 - 8, static_cast<uint32_t>(bit_len & 0xFFFFFFFFULL));
    StoreLE32(tail + pad_blocks * 64 - 4, static_cast<uint32_t>(bit_len >> 32));

    for (size_t b = 0; b < pad_blocks; ++b) {
        Md5CompressBlock(state, tail + b * 64);
    }
    for (int i = 0; i < 4; ++i) {
        StoreLE32(out_digest + i * 4, state[i]);
    }
    SecureZeroBuffer(state, sizeof(state));
    SecureZeroBuffer(tail, sizeof(tail));
}

bool Md5Hash(const uint8_t* data, size_t len, uint8_t out_digest[16]) {
    if (!out_digest || (len > 0 && !data)) return false;
#ifdef _WIN32
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    if (::BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_MD5_ALGORITHM, nullptr, 0) < 0) {
        return false;
    }
    bool ok = false;
    if (::BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) >= 0) {
        PUCHAR input_ptr = len > 0 ? const_cast<PUCHAR>(data) : nullptr;
        if (::BCryptHashData(hHash, input_ptr, static_cast<ULONG>(len), 0) >= 0 &&
            ::BCryptFinishHash(hHash, out_digest, 16, 0) >= 0) {
            ok = true;
        }
        ::BCryptDestroyHash(hHash);
    }
    ::BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
#else
    Md5HashPortable(data, len, out_digest);
    return true;
#endif
}

bool Aes128EcbEncrypt(const uint8_t key[16],
                      const uint8_t* plaintext,
                      size_t len,
                      uint8_t* ciphertext) {
    if (!key || !plaintext || !ciphertext || len == 0 || (len % 16) != 0) {
        return false;
    }
#ifdef _WIN32
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    if (::BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0) < 0) {
        return false;
    }
    bool ok = false;
    if (::BCryptSetProperty(
            hAlg,
            BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_ECB)),
            sizeof(BCRYPT_CHAIN_MODE_ECB),
            0) >= 0 &&
        ::BCryptGenerateSymmetricKey(
            hAlg, &hKey, nullptr, 0, const_cast<PUCHAR>(key), 16, 0) >= 0) {
        // Copy into temporary buffer in case plaintext == ciphertext
        std::vector<uint8_t> tmp(plaintext, plaintext + len);
        ULONG cb_result = 0;
        if (::BCryptEncrypt(hKey,
                            tmp.data(),
                            static_cast<ULONG>(len),
                            nullptr,
                            nullptr,
                            0,
                            ciphertext,
                            static_cast<ULONG>(len),
                            &cb_result,
                            0) >= 0 &&
            cb_result == static_cast<ULONG>(len)) {
            ok = true;
        }
        SecureZeroBuffer(tmp.data(), tmp.size());
        ::BCryptDestroyKey(hKey);
    }
    ::BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
#else
    uint8_t round_keys[176];
    Aes128ExpandKey(key, round_keys);
    for (size_t off = 0; off < len; off += 16) {
        Aes128EncryptBlockPortable(round_keys, plaintext + off, ciphertext + off);
    }
    SecureZeroBuffer(round_keys, sizeof(round_keys));
    return true;
#endif
}

bool Aes128EcbDecrypt(const uint8_t key[16],
                      const uint8_t* ciphertext,
                      size_t len,
                      uint8_t* plaintext) {
    if (!key || !ciphertext || !plaintext || len == 0 || (len % 16) != 0) {
        return false;
    }
#ifdef _WIN32
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    if (::BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0) < 0) {
        return false;
    }
    bool ok = false;
    if (::BCryptSetProperty(
            hAlg,
            BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_ECB)),
            sizeof(BCRYPT_CHAIN_MODE_ECB),
            0) >= 0 &&
        ::BCryptGenerateSymmetricKey(
            hAlg, &hKey, nullptr, 0, const_cast<PUCHAR>(key), 16, 0) >= 0) {
        std::vector<uint8_t> tmp(ciphertext, ciphertext + len);
        ULONG cb_result = 0;
        if (::BCryptDecrypt(hKey,
                            tmp.data(),
                            static_cast<ULONG>(len),
                            nullptr,
                            nullptr,
                            0,
                            plaintext,
                            static_cast<ULONG>(len),
                            &cb_result,
                            0) >= 0 &&
            cb_result == static_cast<ULONG>(len)) {
            ok = true;
        }
        SecureZeroBuffer(tmp.data(), tmp.size());
        ::BCryptDestroyKey(hKey);
    }
    ::BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
#else
    uint8_t round_keys[176];
    Aes128ExpandKey(key, round_keys);
    for (size_t off = 0; off < len; off += 16) {
        Aes128DecryptBlockPortable(round_keys, ciphertext + off, plaintext + off);
    }
    SecureZeroBuffer(round_keys, sizeof(round_keys));
    return true;
#endif
}

bool ModExpBigEndian(const uint8_t* base_be,
                     size_t base_len,
                     const uint8_t* exp_be,
                     size_t exp_len,
                     const uint8_t* mod_be,
                     size_t key_size,
                     std::vector<uint8_t>* out_result_be) {
    if (!base_be || !exp_be || !mod_be || !out_result_be || key_size == 0 || key_size > 2048) {
        return false;
    }

    // Strip leading zeros of modulus to find effective limb count
    size_t mod_first = 0;
    while (mod_first < key_size && mod_be[mod_first] == 0) {
        ++mod_first;
    }
    size_t mod_sig_bytes = key_size - mod_first;
    if (mod_sig_bytes == 0) {
        return false;  // Division by zero
    }

    // Modulus == 1 -> result is 0
    if (mod_sig_bytes == 1 && mod_be[mod_first] == 1) {
        out_result_be->assign(key_size, 0u);
        return true;
    }

    size_t n = (mod_sig_bytes + 3) / 4;
    std::vector<uint32_t> mod_limbs(n, 0u);
    for (size_t i = 0; i < mod_sig_bytes; ++i) {
        uint8_t b = mod_be[key_size - 1 - i];
        mod_limbs[i / 4] |= static_cast<uint32_t>(b) << ((i % 4) * 8);
    }

    std::vector<uint32_t> base_rem(n, 0u);
    ReduceBigEndianMod(base_be, base_len, mod_limbs.data(), n, base_rem.data());

    // Find first non-zero bit of exponent
    size_t exp_first = 0;
    while (exp_first < exp_len && exp_be[exp_first] == 0) {
        ++exp_first;
    }

    std::vector<uint32_t> res_limbs(n, 0u);
    if (exp_first == exp_len) {
        // x^0 mod m == 1 (since m > 1)
        res_limbs[0] = 1u;
    } else if ((mod_limbs[0] & 1u) != 0u) {
        // Fast CIOS Montgomery exponentiation for odd modulus
        uint32_t n0_inv = ComputeMontN0Inv(mod_limbs[0]);

        std::vector<uint32_t> r2(n, 0u);
        r2[0] = 1u;
        size_t total_shifts = 2 * n * 32;
        for (size_t s = 0; s < total_shifts; ++s) {
            DoubleModInPlace(r2.data(), mod_limbs.data(), n);
        }

        std::vector<uint32_t> one_limbs(n, 0u);
        one_limbs[0] = 1u;

        std::vector<uint32_t> scratch(n + 2, 0u);
        std::vector<uint32_t> base_mont(n, 0u);
        std::vector<uint32_t> acc_mont(n, 0u);
        MontMul(base_rem.data(), r2.data(), mod_limbs.data(), n, n0_inv, scratch.data(), base_mont.data());
        MontMul(one_limbs.data(), r2.data(), mod_limbs.data(), n, n0_inv, scratch.data(), acc_mont.data());

        bool started = false;
        for (size_t bi = exp_first; bi < exp_len; ++bi) {
            uint8_t byte_val = exp_be[bi];
            for (int bit = 7; bit >= 0; --bit) {
                bool bit_set = ((byte_val >> bit) & 1u) != 0;
                if (!started) {
                    if (bit_set) {
                        std::memcpy(acc_mont.data(), base_mont.data(), n * sizeof(uint32_t));
                        started = true;
                    }
                    continue;
                }
                MontMul(acc_mont.data(), acc_mont.data(), mod_limbs.data(), n, n0_inv, scratch.data(), acc_mont.data());
                if (bit_set) {
                    MontMul(acc_mont.data(), base_mont.data(), mod_limbs.data(), n, n0_inv, scratch.data(), acc_mont.data());
                }
            }
        }

        MontMul(acc_mont.data(), one_limbs.data(), mod_limbs.data(), n, n0_inv, scratch.data(), res_limbs.data());

        SecureZeroBuffer(scratch.data(), scratch.size() * sizeof(uint32_t));
        SecureZeroBuffer(base_mont.data(), base_mont.size() * sizeof(uint32_t));
        SecureZeroBuffer(acc_mont.data(), acc_mont.size() * sizeof(uint32_t));
    } else {
        // Fallback for even modulus
        res_limbs[0] = 1u;
        for (size_t bi = exp_first; bi < exp_len; ++bi) {
            uint8_t byte_val = exp_be[bi];
            for (int bit = 7; bit >= 0; --bit) {
                ModMulEvenFallback(res_limbs.data(), res_limbs.data(), mod_limbs.data(), n, res_limbs.data());
                if ((byte_val >> bit) & 1u) {
                    ModMulEvenFallback(res_limbs.data(), base_rem.data(), mod_limbs.data(), n, res_limbs.data());
                }
            }
        }
    }

    // Export little-endian limbs into fixed `key_size`-byte big-endian output with leading zeros
    out_result_be->assign(key_size, 0u);
    for (size_t i = 0; i < key_size && i < n * 4; ++i) {
        uint32_t word = res_limbs[i / 4];
        (*out_result_be)[key_size - 1 - i] = static_cast<uint8_t>((word >> ((i % 4) * 8)) & 0xFFu);
    }

    SecureZeroBuffer(base_rem.data(), base_rem.size() * sizeof(uint32_t));
    SecureZeroBuffer(res_limbs.data(), res_limbs.size() * sizeof(uint32_t));
    return true;
}

bool EncryptVncAuthChallenge(const uint8_t challenge[16],
                             const std::string& password,
                             uint8_t out_response[16]) {
    if (!challenge || !out_response) return false;

    uint8_t key8[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    size_t copy_len = std::min<size_t>(password.size(), 8u);
    for (size_t i = 0; i < copy_len; ++i) {
        key8[i] = ReverseByteBits(static_cast<uint8_t>(password[i]));
    }

    uint64_t subkeys[16];
    DesGenerateSubkeys(key8, subkeys);
    DesEncryptBlock(subkeys, challenge, out_response);
    DesEncryptBlock(subkeys, challenge + 8, out_response + 8);

    SecureZeroBuffer(key8, sizeof(key8));
    SecureZeroBuffer(subkeys, sizeof(subkeys));
    return true;
}

bool BuildAppleDhResponse(uint16_t generator,
                          uint16_t key_size,
                          const uint8_t* prime_be,
                          const uint8_t* server_pubkey_be,
                          const std::string& username,
                          const std::string& password,
                          std::vector<uint8_t>* out_packet,
                          std::string* error_out,
                          const std::vector<uint8_t>* deterministic_priv_be,
                          const uint8_t* deterministic_pad_128) {
    if (!prime_be || !server_pubkey_be || !out_packet) {
        if (error_out) *error_out = "Null parameter in BuildAppleDhResponse";
        return false;
    }
    if (generator < 2) {
        if (error_out) *error_out = "Invalid Apple DH generator (< 2)";
        return false;
    }
    if (key_size < 16 || key_size > 2048) {
        if (error_out) *error_out = "Unsupported Apple DH key_size: " + std::to_string(key_size);
        return false;
    }

    // Reject degenerate server public keys (0 or 1 or >= prime)
    bool pub_gt_one = false;
    for (size_t i = 0; i + 1 < key_size; ++i) {
        if (server_pubkey_be[i] != 0) {
            pub_gt_one = true;
            break;
        }
    }
    if (!pub_gt_one && server_pubkey_be[key_size - 1] <= 1) {
        if (error_out) *error_out = "Degenerate server DH public key";
        return false;
    }
    if (std::memcmp(server_pubkey_be, prime_be, key_size) >= 0) {
        if (error_out) *error_out = "Server DH public key >= prime modulus";
        return false;
    }

    // Generate ephemeral DH secret exponent (512-bit = 64 bytes per RFC 3526 §8, or key_size-1 if smaller)
    std::vector<uint8_t> priv_be;
    if (deterministic_priv_be && !deterministic_priv_be->empty()) {
        priv_be = *deterministic_priv_be;
    } else {
        size_t priv_len = std::min<size_t>(64u, static_cast<size_t>(key_size) - 1u);
        priv_be.resize(priv_len, 0u);
        if (!FillSecureRandom(priv_be.data(), priv_be.size())) {
            if (error_out) *error_out = "CSPRNG failure generating Apple DH private key";
            return false;
        }
        // Force MSB high bit set so priv >= 2^(8*priv_len - 1) > 1 and < prime (since priv_len < key_size)
        priv_be[0] |= 0x80u;
        priv_be.back() |= 0x01u;
    }

    uint8_t gen_be[2] = {
        static_cast<uint8_t>((generator >> 8) & 0xFFu),
        static_cast<uint8_t>(generator & 0xFFu),
    };

    std::vector<uint8_t> client_pub_be;
    std::vector<uint8_t> shared_secret_be;
    bool math_ok =
        ModExpBigEndian(gen_be, sizeof(gen_be), priv_be.data(), priv_be.size(),
                        prime_be, key_size, &client_pub_be) &&
        ModExpBigEndian(server_pubkey_be, key_size, priv_be.data(), priv_be.size(),
                        prime_be, key_size, &shared_secret_be);

    SecureZeroBuffer(priv_be.data(), priv_be.size());
    if (!math_ok) {
        if (!shared_secret_be.empty()) {
            SecureZeroBuffer(shared_secret_be.data(), shared_secret_be.size());
        }
        if (error_out) *error_out = "Apple DH modular exponentiation failed";
        return false;
    }

    uint8_t aes_key[16];
    bool hash_ok = Md5Hash(shared_secret_be.data(), shared_secret_be.size(), aes_key);
    SecureZeroBuffer(shared_secret_be.data(), shared_secret_be.size());
    if (!hash_ok) {
        SecureZeroBuffer(aes_key, sizeof(aes_key));
        if (error_out) *error_out = "Apple DH MD5 key derivation failed";
        return false;
    }

    // Build 128-byte padded credentials block:
    //   [0..63]:  UTF-8 username + '\0' + random bytes
    //   [64..127]: UTF-8 password + '\0' + random bytes
    uint8_t plain_creds[128];
    if (deterministic_pad_128) {
        std::memcpy(plain_creds, deterministic_pad_128, 128);
    } else if (!FillSecureRandom(plain_creds, sizeof(plain_creds))) {
        SecureZeroBuffer(aes_key, sizeof(aes_key));
        SecureZeroBuffer(plain_creds, sizeof(plain_creds));
        if (error_out) *error_out = "CSPRNG failure generating credential padding";
        return false;
    }

    size_t u_copy = std::min<size_t>(username.size(), 63u);
    if (u_copy > 0) {
        std::memcpy(plain_creds, username.data(), u_copy);
    }
    plain_creds[u_copy] = '\0';

    size_t p_copy = std::min<size_t>(password.size(), 63u);
    if (p_copy > 0) {
        std::memcpy(plain_creds + 64, password.data(), p_copy);
    }
    plain_creds[64 + p_copy] = '\0';

    out_packet->resize(128u + static_cast<size_t>(key_size));
    bool enc_ok = Aes128EcbEncrypt(aes_key, plain_creds, 128, out_packet->data());

    SecureZeroBuffer(aes_key, sizeof(aes_key));
    SecureZeroBuffer(plain_creds, sizeof(plain_creds));

    if (!enc_ok) {
        out_packet->clear();
        if (error_out) *error_out = "Apple DH AES-128-ECB encryption failed";
        return false;
    }

    std::memcpy(out_packet->data() + 128, client_pub_be.data(), key_size);
    return true;
}

}  // namespace mac_mirror
