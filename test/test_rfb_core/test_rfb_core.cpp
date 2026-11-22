#include "../../mac_mirror_viewer/include/rfb_session.h"
#include "../../mac_mirror_viewer/core_client/miniz.h"
#include "../../mac_mirror_viewer/core_client/rfb_adaptive.h"
#include "../../mac_mirror_viewer/core_client/rfb_crypto.h"
#include "../../mac_mirror_viewer/core_client/rfb_decoder.h"
#include "../../mac_mirror_viewer/core_client/rfb_transport.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using namespace mac_mirror;

int g_failures = 0;
int g_checks = 0;

#define CHECK_TRUE(expr)                                                                  \
    do {                                                                                  \
        ++g_checks;                                                                       \
        if (!(expr)) {                                                                    \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << ": " #expr << "\n";  \
            ++g_failures;                                                                 \
            return;                                                                       \
        }                                                                                 \
    } while (0)

#define CHECK_EQ(a, b)                                                                    \
    do {                                                                                  \
        ++g_checks;                                                                       \
        auto _va = (a);                                                                   \
        auto _vb = (b);                                                                   \
        if (!(_va == _vb)) {                                                              \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << ": " #a " == " #b    \
                      << " (" << _va << " vs " << _vb << ")\n";                           \
            ++g_failures;                                                                 \
            return;                                                                       \
        }                                                                                 \
    } while (0)

std::vector<uint8_t> HexToBytes(const std::string& hex) {
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    auto nibble = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(10 + (c - 'a'));
        if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(10 + (c - 'A'));
        return 0;
    };
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<uint8_t>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
    }
    return out;
}

std::string BytesToHex(const uint8_t* data, size_t len) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string s;
    s.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        s.push_back(kHex[(data[i] >> 4) & 0x0F]);
        s.push_back(kHex[data[i] & 0x0F]);
    }
    return s;
}

// Exact 4096-bit (512-byte) RFC 3526 Group 16 MODP prime advertised by macOS screensharingd!
const std::vector<uint8_t>& GetMacRfc3526Prime4096() {
    static const std::vector<uint8_t> kPrime = HexToBytes(
        "ffffffffffffffffc90fdaa22168c234c4c6628b80dc1cd1"
        "29024e088a67cc74020bbea63b139b22514a08798e3404dd"
        "ef9519b3cd3a431b302b0a6df25f14374fe1356d6d51c245"
        "e485b576625e7ec6f44c42e9a637ed6b0bff5cb6f406b7ed"
        "ee386bfb5a899fa5ae9f24117c4b1fe649286651ece45b3d"
        "c2007cb8a163bf0598da48361c55d39a69163fa8fd24cf5f"
        "83655d23dca3ad961c62f356208552bb9ed529077096966d"
        "670c354e4abc9804f1746c08ca18217c32905e462e36ce3b"
        "e39e772c180e86039b2783a2ec07a28fb5c55df06f4c52c9"
        "de2bcbf6955817183995497cea956ae515d2261898fa0510"
        "15728e5a8AAAC42DAD33170D04507A33A85521ABDF1CBA64"
        "ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7"
        "ABF5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6B"
        "F12FFA06D98A0864D87602733EC86A64521F2B18177B200C"
        "BBE117577A615D6C770988C0BAD946E208E24FA074E5AB31"
        "43DB5BFCE0FD108E4B82D120A92108011A723C12A787E6D7"
        "88719A10BDBA5B2699C327186AF4E23C1A946834B6150BDA"
        "2583E9CA2AD44CE8DBBBC2DB04DE8EF92E8EFC141FBECAA6"
        "287C59474E6BC05D99B2964FA090C3A2233BA186515BE7ED"
        "1F612970CEE2D7AFB81BDD762170481CD0069127D5B05AA9"
        "93B4EA988D8FDDC186FFB7DC90A6C08F4DF435C934063199"
        "FFFFFFFFFFFFFFFF");
    return kPrime;
}

class MemoryByteReader {
public:
    explicit MemoryByteReader(std::vector<uint8_t> data) : data_(std::move(data)) {}

    RfbDecoder::ByteReadFn Fn() {
        return [this](void* dst, size_t len, std::string* err) -> bool {
            if (pos_ + len > data_.size()) {
                if (err) *err = "MemoryByteReader EOF";
                return false;
            }
            if (len > 0) {
                std::memcpy(dst, data_.data() + pos_, len);
                pos_ += len;
            }
            return true;
        };
    }

private:
    std::vector<uint8_t> data_;
    size_t pos_ = 0;
};

void AppendBE32(std::vector<uint8_t>* buf, uint32_t v) {
    buf->push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
    buf->push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    buf->push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    buf->push_back(static_cast<uint8_t>(v & 0xFFu));
}

void AppendBE16(std::vector<uint8_t>* buf, uint16_t v) {
    buf->push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    buf->push_back(static_cast<uint8_t>(v & 0xFFu));
}

std::vector<uint8_t> DeflateSyncFlush(mz_stream* zs, const std::vector<uint8_t>& plain) {
    std::vector<uint8_t> out;
    out.resize(plain.size() + 256u + (plain.size() / 16u));
    zs->next_in = plain.data();
    zs->avail_in = static_cast<unsigned int>(plain.size());
    size_t written = 0;
    while (true) {
        if (written == out.size()) {
            out.resize(out.size() * 2u);
        }
        unsigned int space = static_cast<unsigned int>(out.size() - written);
        zs->next_out = out.data() + written;
        zs->avail_out = space;
        int rc = mz_deflate(zs, MZ_SYNC_FLUSH);
        written += (space - zs->avail_out);
        if (rc != MZ_OK || (zs->avail_in == 0 && zs->avail_out > 0)) {
            break;
        }
    }
    out.resize(written);

    std::vector<uint8_t> framed;
    framed.reserve(4u + out.size());
    AppendBE32(&framed, static_cast<uint32_t>(out.size()));
    framed.insert(framed.end(), out.begin(), out.end());
    return framed;
}

// ============================================================================
// 1. SOCKS5 Framing & Endpoint Parsing Tests
// ============================================================================
void TestSocks5Framing() {
    Endpoint ep{};
    std::string err;

    CHECK_TRUE(ParseHostPortSpec("127.0.0.1:1055", 0, &ep, &err));
    CHECK_EQ(ep.host, std::string("127.0.0.1"));
    CHECK_EQ(ep.port, 1055);

    CHECK_TRUE(ParseHostPortSpec("  [fd7a:115c:a1e0::42]:1080  ", 0, &ep, &err));
    CHECK_EQ(ep.host, std::string("fd7a:115c:a1e0::42"));
    CHECK_EQ(ep.port, 1080);

    CHECK_TRUE(ParseHostPortSpec("100.80.74.94", 5900, &ep, &err));
    CHECK_EQ(ep.host, std::string("100.80.74.94"));
    CHECK_EQ(ep.port, 5900);

    CHECK_TRUE(!ParseHostPortSpec("127.0.0.1:0", 0, &ep, &err));
    CHECK_TRUE(!ParseHostPortSpec("127.0.0.1:65536", 0, &ep, &err));
    CHECK_TRUE(!ParseHostPortSpec("[::1:1055", 0, &ep, &err));

    // IPv4 ATYP=0x01
    std::vector<uint8_t> pkt;
    CHECK_TRUE(BuildSocks5ConnectRequest("100.80.74.94", 5900, &pkt, &err));
    std::vector<uint8_t> expected_v4 = {0x05, 0x01, 0x00, 0x01, 100, 80, 74, 94, 0x17, 0x0C};
    CHECK_TRUE(pkt == expected_v4);

    // IPv6 ATYP=0x04
    CHECK_TRUE(BuildSocks5ConnectRequest("[::1]", 5900, &pkt, &err));
    CHECK_EQ(pkt.size(), static_cast<size_t>(4 + 16 + 2));
    CHECK_EQ(pkt[0], 0x05);
    CHECK_EQ(pkt[3], 0x04);
    for (size_t i = 4; i < 19; ++i) CHECK_EQ(pkt[i], 0x00);
    CHECK_EQ(pkt[19], 0x01);
    CHECK_EQ(pkt[20], 0x17);
    CHECK_EQ(pkt[21], 0x0C);

    // Domain ATYP=0x03
    std::string domain = "kimtaeyoungs-macbook-air.tailnet.ts.net";
    CHECK_TRUE(BuildSocks5ConnectRequest(domain, 5900, &pkt, &err));
    CHECK_EQ(pkt.size(), 4u + 1u + domain.size() + 2u);
    CHECK_EQ(pkt[3], 0x03);
    CHECK_EQ(pkt[4], static_cast<uint8_t>(domain.size()));
    CHECK_EQ(std::string(pkt.begin() + 5, pkt.end() - 2), domain);
    CHECK_EQ(pkt[pkt.size() - 2], 0x17);
    CHECK_EQ(pkt[pkt.size() - 1], 0x0C);

    // Reject >255 byte domain
    std::string huge_domain(256, 'a');
    CHECK_TRUE(!BuildSocks5ConnectRequest(huge_domain, 5900, &pkt, &err));
}

// ============================================================================
// 2. Apple DH (SecurityType 30) & VncAuth (SecurityType 2) Crypto Tests
// ============================================================================
void TestCryptoAndAppleDh() {
    // RFC 1321 MD5 vectors
    uint8_t md5_out[16];
    const char* abc = "abc";
    CHECK_TRUE(Md5Hash(reinterpret_cast<const uint8_t*>(abc), 3, md5_out));
    CHECK_EQ(BytesToHex(md5_out, 16), std::string("900150983cd24fb0d6963f7d28e17f72"));

    // NIST FIPS-197 AES-128-ECB vector + 128-byte round-trip
    auto aes_key = HexToBytes("2b7e151628aed2a6abf7158809cf4f3c");
    auto aes_pt = HexToBytes("3243f6a8885a308d313198a2e0370734");
    uint8_t aes_ct[16] = {};
    uint8_t aes_dec[16] = {};
    CHECK_TRUE(Aes128EcbEncrypt(aes_key.data(), aes_pt.data(), 16, aes_ct));
    CHECK_EQ(BytesToHex(aes_ct, 16), std::string("3925841d02dc09fbdc118597196a0b32"));
    CHECK_TRUE(Aes128EcbDecrypt(aes_key.data(), aes_ct, 16, aes_dec));
    CHECK_EQ(BytesToHex(aes_dec, 16), std::string("3243f6a8885a308d313198a2e0370734"));

    // VncAuth bit-reversal + DES-ECB vectors
    CHECK_EQ(ReverseByteBits(0x01u), 0x80u);
    CHECK_EQ(ReverseByteBits(0x80u), 0x01u);
    CHECK_EQ(ReverseByteBits(0x42u), 0x42u);
    CHECK_EQ(ReverseByteBits(0x56u), 0x6Au);

    auto ch1 = HexToBytes("318e90d502a379a7b1583c35db772138");
    uint8_t vnc_resp1[16] = {};
    CHECK_TRUE(EncryptVncAuthChallenge(ch1.data(), "BBBBBBBB", vnc_resp1));
    CHECK_EQ(BytesToHex(vnc_resp1, 16), std::string("1d214ef554123712a0dc9bcf003c27f4"));

    auto ch2 = HexToBytes("0123456789abcdefdeadbeefcafebabe");
    uint8_t vnc_resp2[16] = {};
    CHECK_TRUE(EncryptVncAuthChallenge(ch2.data(), "VncPass!ExtraIgnoredChars", vnc_resp2));
    CHECK_EQ(BytesToHex(vnc_resp2, 16), std::string("3dc64e8efd741f85036d585bae43394e"));

    // Apple Diffie-Hellman (SecurityType 30) full 4096-bit Group 16 server<->client round-trip!
    const auto& prime = GetMacRfc3526Prime4096();
    CHECK_EQ(prime.size(), static_cast<size_t>(512));

    uint8_t gen_be[2] = {0x00, 0x05};  // g = 5 (exact generator used by macOS screensharingd!)
    std::vector<uint8_t> server_priv(64, 0xA5u);
    server_priv[0] = 0x93u;
    server_priv[63] = 0x37u;

    std::vector<uint8_t> server_pub;
    CHECK_TRUE(ModExpBigEndian(gen_be, 2, server_priv.data(), server_priv.size(),
                               prime.data(), 512, &server_pub));
    CHECK_EQ(server_pub.size(), static_cast<size_t>(512));

    std::vector<uint8_t> dh_packet;
    std::string err;
    CHECK_TRUE(BuildAppleDhResponse(5, 512, prime.data(), server_pub.data(),
                                    "taeyoung", "AppleScreenShare!2026",
                                    &dh_packet, &err));
    CHECK_EQ(dh_packet.size(), static_cast<size_t>(128 + 512));

    // Simulate macOS screensharingd decrypting client's response
    const uint8_t* enc_creds = dh_packet.data();
    const uint8_t* client_pub = dh_packet.data() + 128;

    std::vector<uint8_t> server_shared;
    CHECK_TRUE(ModExpBigEndian(client_pub, 512, server_priv.data(), server_priv.size(),
                               prime.data(), 512, &server_shared));
    CHECK_EQ(server_shared.size(), static_cast<size_t>(512));

    uint8_t derived_aes_key[16];
    CHECK_TRUE(Md5Hash(server_shared.data(), server_shared.size(), derived_aes_key));

    uint8_t decrypted_creds[128] = {};
    CHECK_TRUE(Aes128EcbDecrypt(derived_aes_key, enc_creds, 128, decrypted_creds));

    std::string got_user(reinterpret_cast<const char*>(decrypted_creds));
    std::string got_pass(reinterpret_cast<const char*>(decrypted_creds + 64));
    CHECK_EQ(got_user, std::string("taeyoung"));
    CHECK_EQ(got_pass, std::string("AppleScreenShare!2026"));

    // Verify random padding after NUL terminator is populated (not all zeros)
    bool user_pad_nonzero = false;
    for (size_t i = got_user.size() + 1; i < 64; ++i) {
        if (decrypted_creds[i] != 0) user_pad_nonzero = true;
    }
    bool pass_pad_nonzero = false;
    for (size_t i = 64 + got_pass.size() + 1; i < 128; ++i) {
        if (decrypted_creds[i] != 0) pass_pad_nonzero = true;
    }
    CHECK_TRUE(user_pad_nonzero);
    CHECK_TRUE(pass_pad_nonzero);

    // Reject degenerate server public keys (0, 1, or == prime)
    std::vector<uint8_t> bad_pub(512, 0u);
    CHECK_TRUE(!BuildAppleDhResponse(5, 512, prime.data(), bad_pub.data(), "u", "p", &dh_packet, &err));
    bad_pub.back() = 1u;
    CHECK_TRUE(!BuildAppleDhResponse(5, 512, prime.data(), bad_pub.data(), "u", "p", &dh_packet, &err));
    CHECK_TRUE(!BuildAppleDhResponse(5, 512, prime.data(), prime.data(), "u", "p", &dh_packet, &err));
}

// ============================================================================
// 3. ZRLE (Solid, Packed Palette, Plain RLE, Palette RLE, Raw), CopyRect & RichCursor
// ============================================================================
void TestDecodersAndCursor() {
    RfbDecoder dec;
    CHECK_TRUE(dec.ResetSession(140, 130, RfbPixelFormat::Bgrx8888()));

    mz_stream zs{};
    CHECK_EQ(mz_deflateInit(&zs, MZ_DEFAULT_COMPRESSION), MZ_OK);

    // Frame 1: 4 tiles across a 128x128 ZRLE rect (each tile 64x64) in 32-bit BGRX8888 (3-byte CPIXEL [B,G,R])
    //   Tile (0,0): subenc 1  -> Solid color (B=0x10, G=0x20, R=0x30)
    //   Tile (64,0): subenc 2 -> 2-color Packed Palette (checkerboard)
    //   Tile (0,64): subenc 128 -> Plain RLE (2000 pixels color A + 2096 pixels color B, testing >255 runs!)
    //   Tile (64,64): subenc 0 -> Raw CPIXEL tile (gradient)
    std::vector<uint8_t> zrle_uncomp1;

    // Tile (0,0): Solid
    zrle_uncomp1.push_back(1u);
    zrle_uncomp1.insert(zrle_uncomp1.end(), {0x10u, 0x20u, 0x30u});

    // Tile (64,0): Packed 2-color palette (1 bit per pixel -> 8 bytes per 64px row)
    zrle_uncomp1.push_back(2u);
    zrle_uncomp1.insert(zrle_uncomp1.end(), {0xAAu, 0xBBu, 0xCCu});  // idx 0
    zrle_uncomp1.insert(zrle_uncomp1.end(), {0x11u, 0x22u, 0x33u});  // idx 1
    for (int r = 0; r < 64; ++r) {
        uint8_t pattern = (r % 2 == 0) ? 0xAAu : 0x55u;  // alternating 1,0,1,0...
        for (int b = 0; b < 8; ++b) zrle_uncomp1.push_back(pattern);
    }

    // Tile (0,64): Plain RLE (total 64*64 = 4096 pixels: 2000 + 2096)
    zrle_uncomp1.push_back(128u);
    auto append_rle_len = [](std::vector<uint8_t>* out, size_t len) {
        size_t rem = len - 1u;
        while (rem >= 255u) {
            out->push_back(255u);
            rem -= 255u;
        }
        out->push_back(static_cast<uint8_t>(rem));
    };
    zrle_uncomp1.insert(zrle_uncomp1.end(), {0x01u, 0x02u, 0x03u});
    append_rle_len(&zrle_uncomp1, 2000u);
    zrle_uncomp1.insert(zrle_uncomp1.end(), {0xFDu, 0xFEu, 0xFFu});
    append_rle_len(&zrle_uncomp1, 2096u);

    // Tile (64,64): Raw 64x64 CPIXEL
    zrle_uncomp1.push_back(0u);
    for (int r = 0; r < 64; ++r) {
        for (int c = 0; c < 64; ++c) {
            zrle_uncomp1.push_back(static_cast<uint8_t>(c));
            zrle_uncomp1.push_back(static_cast<uint8_t>(r));
            zrle_uncomp1.push_back(0x7Fu);
        }
    }

    auto pkt1 = DeflateSyncFlush(&zs, zrle_uncomp1);
    MemoryByteReader rdr1(pkt1);
    DecodeOutcome out1{};
    std::string err;
    CHECK_TRUE(dec.DecodeRectangle(0, 0, 128, 128, rfb_encoding::kZRLE, rdr1.Fn(), &out1, nullptr, &err));
    CHECK_EQ(static_cast<int>(out1), static_cast<int>(DecodeOutcome::PixelRectUpdated));

    auto check_px = [&](uint32_t x, uint32_t y, uint8_t b, uint8_t g, uint8_t r) {
        const uint8_t* p = dec.PixelsBgra() + (y * dec.StrideBytes()) + x * 4u;
        CHECK_EQ(p[0], b);
        CHECK_EQ(p[1], g);
        CHECK_EQ(p[2], r);
        CHECK_EQ(p[3], 0xFFu);
    };

    check_px(0, 0, 0x10u, 0x20u, 0x30u);
    check_px(63, 63, 0x10u, 0x20u, 0x30u);
    check_px(64, 0, 0x11u, 0x22u, 0x33u);   // MSB of 0xAA is 1 -> palette[1]
    check_px(65, 0, 0xAAu, 0xBBu, 0xCCu);   // next bit of 0xAA is 0 -> palette[0]
    check_px(0, 64, 0x01u, 0x02u, 0x03u);   // Plain RLE run 1
    check_px(63, 127, 0xFDu, 0xFEu, 0xFFu); // Plain RLE run 2
    check_px(74, 84, 10u, 20u, 0x7Fu);      // Raw tile (c=10, r=20)

    // Frame 2 on SAME continuous zlib stream: odd-width 13x5 tile testing 4-bit packed palette + Palette RLE!
    std::vector<uint8_t> zrle_uncomp2;
    zrle_uncomp2.push_back(131u);  // Palette RLE with 3 colors (131 - 128 = 3)
    zrle_uncomp2.insert(zrle_uncomp2.end(), {0x40u, 0x50u, 0x60u});  // pal 0
    zrle_uncomp2.insert(zrle_uncomp2.end(), {0x70u, 0x80u, 0x90u});  // pal 1
    zrle_uncomp2.insert(zrle_uncomp2.end(), {0xA0u, 0xB0u, 0xC0u});  // pal 2
    zrle_uncomp2.push_back(0x00u);        // 1 pixel of pal 0
    zrle_uncomp2.push_back(0x80u | 0x01u); // RLE run of pal 1, length = 60
    zrle_uncomp2.push_back(59u);
    zrle_uncomp2.push_back(0x80u | 0x02u); // RLE run of pal 2, length = 4 (total 1 + 60 + 4 = 65 = 13*5)
    zrle_uncomp2.push_back(3u);

    auto pkt2 = DeflateSyncFlush(&zs, zrle_uncomp2);
    mz_deflateEnd(&zs);

    MemoryByteReader rdr2(pkt2);
    CHECK_TRUE(dec.DecodeRectangle(127, 120, 13, 5, rfb_encoding::kZRLE, rdr2.Fn(), &out1, nullptr, &err));
    check_px(127, 120, 0x40u, 0x50u, 0x60u);
    check_px(128, 120, 0x70u, 0x80u, 0x90u);
    check_px(139, 124, 0xA0u, 0xB0u, 0xC0u);

    // CopyRect overlapping downward & rightward scroll test:
    // Copy 32x32 from (50, 50) -> (58, 58) (overlaps both horizontally and vertically!)
    uint8_t expected_overlap[32][32][4];
    for (uint32_t ry = 0; ry < 32; ++ry) {
        for (uint32_t rx = 0; rx < 32; ++rx) {
            const uint8_t* sp = dec.PixelsBgra() + (50 + ry) * dec.StrideBytes() + (50 + rx) * 4u;
            std::memcpy(expected_overlap[ry][rx], sp, 4);
        }
    }
    std::vector<uint8_t> copy_payload;
    AppendBE16(&copy_payload, 50);
    AppendBE16(&copy_payload, 50);
    MemoryByteReader copy_rdr(copy_payload);
    CHECK_TRUE(dec.DecodeRectangle(58, 58, 32, 32, rfb_encoding::kCopyRect, copy_rdr.Fn(), &out1, nullptr, &err));
    for (uint32_t ry = 0; ry < 32; ++ry) {
        for (uint32_t rx = 0; rx < 32; ++rx) {
            check_px(58 + rx, 58 + ry,
                     expected_overlap[ry][rx][0],
                     expected_overlap[ry][rx][1],
                     expected_overlap[ry][rx][2]);
        }
    }

    // 16-bit RGB565 Raw decode test: pure Red (31 << 11 = 0xF800) -> B=0, G=0, R=255
    dec.SetPixelFormat(RfbPixelFormat::Rgb565());
    std::vector<uint8_t> raw565 = {0x00u, 0xF8u, 0xE0u, 0x07u, 0x1Fu, 0x00u};  // Red, Green, Blue
    MemoryByteReader raw565_rdr(raw565);
    CHECK_TRUE(dec.DecodeRectangle(0, 129, 3, 1, rfb_encoding::kRaw, raw565_rdr.Fn(), &out1, nullptr, &err));
    check_px(0, 129, 0u, 0u, 255u);
    check_px(1, 129, 0u, 255u, 0u);
    check_px(2, 129, 255u, 0u, 0u);

    // RichCursor (-239): blank 0x0 cursor, all-transparent 8x8 cursor, and visible 9x2 cursor
    dec.SetPixelFormat(RfbPixelFormat::Bgrx8888());
    CursorState cur{};
    MemoryByteReader empty_rdr({});
    CHECK_TRUE(dec.DecodeRectangle(0, 0, 0, 0, rfb_encoding::kCursor, empty_rdr.Fn(), &out1, &cur, &err));
    CHECK_EQ(static_cast<int>(out1), static_cast<int>(DecodeOutcome::CursorUpdated));
    CHECK_TRUE(cur.has_server_cursor);
    CHECK_TRUE(cur.cursor_is_blank);

    std::vector<uint8_t> trans_cur(8 * 8 * 4 + 8, 0u);  // 8x8 pixels + 8 zero mask bytes
    MemoryByteReader trans_rdr(trans_cur);
    CHECK_TRUE(dec.DecodeRectangle(2, 3, 8, 8, rfb_encoding::kCursor, trans_rdr.Fn(), &out1, &cur, &err));
    CHECK_TRUE(cur.cursor_is_blank);

    // 9x2 cursor (mask stride = 2 bytes/row): pixel (0,0) and pixel (8,1) opaque!
    std::vector<uint8_t> vis_cur(9 * 2 * 4 + 2 * 2, 0u);
    vis_cur[0] = 0x12u; vis_cur[1] = 0x34u; vis_cur[2] = 0x56u; vis_cur[3] = 0xFFu;
    size_t idx_8_1 = (1 * 9 + 8) * 4;
    vis_cur[idx_8_1 + 0] = 0x9Au; vis_cur[idx_8_1 + 1] = 0xBCu; vis_cur[idx_8_1 + 2] = 0xDEu;
    size_t mask_off = 9 * 2 * 4;
    vis_cur[mask_off + 0] = 0x80u;  // row 0 col 0
    vis_cur[mask_off + 3] = 0x80u;  // row 1 col 8 (MSB of 2nd byte in row 1)
    MemoryByteReader vis_rdr(vis_cur);
    CHECK_TRUE(dec.DecodeRectangle(1, 1, 9, 2, rfb_encoding::kCursor, vis_rdr.Fn(), &out1, &cur, &err));
    CHECK_TRUE(!cur.cursor_is_blank);
    CHECK_EQ(cur.width, 9);
    CHECK_EQ(cur.height, 2);
    CHECK_EQ(cur.pixels_bgra[0], 0x12u);
    CHECK_EQ(cur.pixels_bgra[3], 0xFFu);
    CHECK_EQ(cur.pixels_bgra[4 + 3], 0x00u);  // pixel (1,0) transparent
    CHECK_EQ(cur.pixels_bgra[idx_8_1 + 2], 0xDEu);
    CHECK_EQ(cur.pixels_bgra[idx_8_1 + 3], 0xFFu);
}

// ============================================================================
// 4. Adaptive Quality & 6ms Pointer Rate-Limiter Transitions
// ============================================================================
void TestAdaptiveQualityTransitions() {
    using Clock = AdaptiveQualityController::Clock;
    AdaptiveQualityController ctrl(QualityMode::Adaptive);
    auto t0 = Clock::now();

    // Idle start -> 32bpp BGRX8888
    auto d0 = ctrl.PeekDecision(t0, 32);
    CHECK_EQ(d0.target_bpp, 32);
    CHECK_TRUE(!d0.format_changed);

    // First pointer event sends immediately (0ms delay), sub-6ms pure moves coalesce
    auto p1 = ctrl.OnPointerInput(t0, 100, 100, 0);
    CHECK_TRUE(p1.has_value());
    auto p2 = ctrl.OnPointerInput(t0 + std::chrono::milliseconds(2), 120, 110, 0);
    CHECK_TRUE(!p2.has_value());
    auto p3 = ctrl.OnPointerInput(t0 + std::chrono::milliseconds(4), 140, 120, 0);
    CHECK_TRUE(!p3.has_value());
    CHECK_TRUE(ctrl.HasPendingPointer());

    // Rapid pointer burst (3 moves within 50ms) -> switches to 16bpp RGB565!
    auto d_burst = ctrl.PeekDecision(t0 + std::chrono::milliseconds(4), 32);
    CHECK_EQ(d_burst.target_bpp, 16);
    CHECK_TRUE(d_burst.format_changed);
    ctrl.CommitAppliedFormat(16, false);

    // At +6ms, FlushPendingPointer emits the latest coalesced coordinate (140, 120)
    auto flushed = ctrl.FlushPendingPointer(t0 + std::chrono::milliseconds(6));
    CHECK_TRUE(flushed.has_value());
    CHECK_EQ(flushed->x, 140);
    CHECK_EQ(flushed->y, 120);

    // Button press ALWAYS bypasses 6ms rate-limiting immediately!
    auto click = ctrl.OnPointerInput(t0 + std::chrono::milliseconds(7), 140, 120, rfb_button::kLeft);
    CHECK_TRUE(click.has_value());
    CHECK_EQ(click->button_mask, rfb_button::kLeft);

    // At +240ms after last burst activity (t0 + 7ms + 240ms = t0 + 247ms): still 16bpp
    auto d_hold = ctrl.PeekDecision(t0 + std::chrono::milliseconds(247), 16);
    CHECK_EQ(d_hold.target_bpp, 16);

    // At >= 250ms idle (t0 + 7ms + 251ms = t0 + 258ms): switches back to 32bpp + full refresh!
    auto d_sharp = ctrl.PeekDecision(t0 + std::chrono::milliseconds(258), 16);
    CHECK_EQ(d_sharp.target_bpp, 32);
    CHECK_TRUE(d_sharp.format_changed);
    CHECK_TRUE(d_sharp.request_full_refresh);
    ctrl.CommitAppliedFormat(32, true);

    // Wheel scroll also triggers 16bpp burst for 250ms
    auto tw = t0 + std::chrono::milliseconds(500);
    ctrl.OnWheelInput(tw);
    CHECK_EQ(ctrl.PeekDecision(tw + std::chrono::milliseconds(10), 32).target_bpp, 16);
    CHECK_EQ(ctrl.PeekDecision(tw + std::chrono::milliseconds(260), 16).target_bpp, 32);

    // Explicit Sharp / Turbo overrides
    ctrl.SetQualityMode(QualityMode::Turbo, tw + std::chrono::milliseconds(300));
    CHECK_EQ(ctrl.PeekDecision(tw + std::chrono::milliseconds(1000), 32).target_bpp, 16);
    ctrl.SetQualityMode(QualityMode::Sharp, tw + std::chrono::milliseconds(1100));
    CHECK_EQ(ctrl.PeekDecision(tw + std::chrono::milliseconds(1100), 16).target_bpp, 32);

    // RTT & FPS telemetry check
    auto tf = tw + std::chrono::milliseconds(2000);
    ctrl.OnUpdateRequestSent(tf, false);
    ctrl.OnUpdateCompleted(tf + std::chrono::milliseconds(14));
    CHECK_TRUE(ctrl.GetRttMs() >= 13.5 && ctrl.GetRttMs() <= 14.5);
    CHECK_EQ(ctrl.GetFramesReceived(), static_cast<uint64_t>(1));
}

// ============================================================================
// 5. End-to-End Mock RFB 003.889 + SOCKS5 Proxy Session Integration Test
// ============================================================================
#ifndef _WIN32
bool RecvAllPosix(int fd, void* dst, size_t len) {
    auto* p = static_cast<uint8_t*>(dst);
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::recv(fd, p + got, len - got, 0);
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

bool SendAllPosix(int fd, const void* src, size_t len) {
    const auto* p = static_cast<const uint8_t*>(src);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, p + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

int CreateLoopbackListener(uint16_t* out_port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(fd, 4) != 0) {
        ::close(fd);
        return -1;
    }
    socklen_t alen = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &alen);
    *out_port = ntohs(addr.sin_port);
    return fd;
}

void TestEndToEndRfbSessionOverSocks5() {
    uint16_t rfb_port = 0;
    uint16_t socks_port = 0;
    int rfb_listen = CreateLoopbackListener(&rfb_port);
    int socks_listen = CreateLoopbackListener(&socks_port);
    CHECK_TRUE(rfb_listen >= 0 && socks_listen >= 0);

    std::atomic<bool> auth_verified_on_server{false};

    // Mock Tailscale SOCKS5 server forwarding to mock screensharingd
    std::thread socks_thread([&]() {
        int client_fd = ::accept(socks_listen, nullptr, nullptr);
        if (client_fd < 0) return;
        uint8_t greet[3];
        if (!RecvAllPosix(client_fd, greet, 3)) { ::close(client_fd); return; }
        uint8_t greet_ok[2] = {0x05, 0x00};
        SendAllPosix(client_fd, greet_ok, 2);

        uint8_t req_hdr[4];
        if (!RecvAllPosix(client_fd, req_hdr, 4)) { ::close(client_fd); return; }
        if (req_hdr[3] == 0x01) {
            uint8_t rest[6];
            RecvAllPosix(client_fd, rest, 6);
        } else if (req_hdr[3] == 0x03) {
            uint8_t dlen = 0;
            RecvAllPosix(client_fd, &dlen, 1);
            std::vector<uint8_t> rest(dlen + 2);
            RecvAllPosix(client_fd, rest.data(), rest.size());
        }

        int upstream = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        dst.sin_port = htons(rfb_port);
        if (::connect(upstream, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) != 0) {
            ::close(upstream);
            ::close(client_fd);
            return;
        }

        // Reply with ATYP=0x01 success
        uint8_t rep[10] = {0x05, 0x00, 0x00, 0x01, 127, 0, 0, 1, 0x17, 0x0C};
        SendAllPosix(client_fd, rep, 10);

        std::thread t_up([client_fd, upstream]() {
            uint8_t buf[4096];
            while (true) {
                ssize_t n = ::recv(client_fd, buf, sizeof(buf), 0);
                if (n <= 0) break;
                if (!SendAllPosix(upstream, buf, static_cast<size_t>(n))) break;
            }
            ::shutdown(upstream, SHUT_RDWR);
        });
        uint8_t buf[4096];
        while (true) {
            ssize_t n = ::recv(upstream, buf, sizeof(buf), 0);
            if (n <= 0) break;
            if (!SendAllPosix(client_fd, buf, static_cast<size_t>(n))) break;
        }
        ::shutdown(client_fd, SHUT_RDWR);
        t_up.join();
        ::close(upstream);
        ::close(client_fd);
    });

    // Mock macOS screensharingd (RFB 003.889 + SecurityType 30 + ZRLE frame)
    std::thread rfb_thread([&]() {
        int cfd = ::accept(rfb_listen, nullptr, nullptr);
        if (cfd < 0) return;

        SendAllPosix(cfd, "RFB 003.889\n", 12);
        char client_ver[12];
        if (!RecvAllPosix(cfd, client_ver, 12)) { ::close(cfd); return; }

        // Exact security list from this Mac: [5, 30, 33, 36, 2, 35]
        uint8_t sec_list[6] = {5, 30, 33, 36, 2, 35};
        SendAllPosix(cfd, sec_list, sizeof(sec_list));

        uint8_t chosen = 0;
        if (!RecvAllPosix(cfd, &chosen, 1) || chosen != 30) { ::close(cfd); return; }

        // Send 1024-bit (128-byte) fast DH group for integration test speed
        const auto& p4096 = GetMacRfc3526Prime4096();
        uint16_t key_sz = 512;
        std::vector<uint8_t> srv_priv(32, 0x42u);
        uint8_t gen_be[2] = {0x00, 0x05};
        std::vector<uint8_t> srv_pub;
        ModExpBigEndian(gen_be, 2, srv_priv.data(), srv_priv.size(), p4096.data(), key_sz, &srv_pub);

        std::vector<uint8_t> dh_challenge;
        AppendBE16(&dh_challenge, 5);
        AppendBE16(&dh_challenge, key_sz);
        dh_challenge.insert(dh_challenge.end(), p4096.begin(), p4096.end());
        dh_challenge.insert(dh_challenge.end(), srv_pub.begin(), srv_pub.end());
        SendAllPosix(cfd, dh_challenge.data(), dh_challenge.size());

        std::vector<uint8_t> dh_reply(128 + key_sz);
        if (!RecvAllPosix(cfd, dh_reply.data(), dh_reply.size())) { ::close(cfd); return; }

        std::vector<uint8_t> shared;
        ModExpBigEndian(dh_reply.data() + 128, key_sz, srv_priv.data(), srv_priv.size(),
                        p4096.data(), key_sz, &shared);
        uint8_t md5_key[16];
        Md5Hash(shared.data(), shared.size(), md5_key);
        uint8_t plain[128];
        Aes128EcbDecrypt(md5_key, dh_reply.data(), 128, plain);
        if (std::strcmp(reinterpret_cast<char*>(plain), "macuser") == 0 &&
            std::strcmp(reinterpret_cast<char*>(plain + 64), "macpass") == 0) {
            auth_verified_on_server.store(true);
        }

        uint8_t auth_ok[4] = {0, 0, 0, 0};
        SendAllPosix(cfd, auth_ok, 4);

        uint8_t client_init = 0;
        RecvAllPosix(cfd, &client_init, 1);

        // ServerInit: 64x40 desktop named "MacBook-Air"
        std::vector<uint8_t> sinit(24, 0u);
        sinit[0] = 0; sinit[1] = 64;
        sinit[2] = 0; sinit[3] = 40;
        RfbPixelFormat::Bgrx8888().Serialize(sinit.data() + 4);
        std::string name = "MacBook-Air";
        sinit[23] = static_cast<uint8_t>(name.size());
        sinit.insert(sinit.end(), name.begin(), name.end());
        SendAllPosix(cfd, sinit.data(), sinit.size());

        // Drain SetEncodings + SetPixelFormat + FramebufferUpdateRequest, then send 1 ZRLE solid frame!
        while (true) {
            uint8_t cmsg = 0;
            if (!RecvAllPosix(cfd, &cmsg, 1)) break;
            if (cmsg == 0) {
                uint8_t rest[19];
                if (!RecvAllPosix(cfd, rest, 19)) break;
            } else if (cmsg == 2) {
                uint8_t hdr[3];
                if (!RecvAllPosix(cfd, hdr, 3)) break;
                uint16_t nenc = static_cast<uint16_t>((hdr[1] << 8) | hdr[2]);
                std::vector<uint8_t> encs(nenc * 4u);
                if (!RecvAllPosix(cfd, encs.data(), encs.size())) break;
            } else if (cmsg == 3) {
                uint8_t rest[9];
                if (!RecvAllPosix(cfd, rest, 9)) break;
                if (rest[0] == 0) {
                    // Non-incremental request -> send ZRLE solid frame (B=0x33, G=0x66, R=0x99)
                    mz_stream zs{};
                    mz_deflateInit(&zs, MZ_DEFAULT_COMPRESSION);
                    std::vector<uint8_t> tile = {1u, 0x33u, 0x66u, 0x99u};
                    auto comp = DeflateSyncFlush(&zs, tile);
                    mz_deflateEnd(&zs);

                    std::vector<uint8_t> fbu = {0u, 0u, 0u, 1u};  // FramebufferUpdate, 1 rect
                    AppendBE16(&fbu, 0);
                    AppendBE16(&fbu, 0);
                    AppendBE16(&fbu, 64);
                    AppendBE16(&fbu, 40);
                    AppendBE32(&fbu, static_cast<uint32_t>(rfb_encoding::kZRLE));
                    fbu.insert(fbu.end(), comp.begin(), comp.end());
                    SendAllPosix(cfd, fbu.data(), fbu.size());
                }
            } else {
                break;
            }
        }
        ::close(cfd);
    });

    std::mutex cv_mtx;
    std::condition_variable cv;
    bool got_frame = false;
    std::string connected_name;
    std::string seen_banner;

    auto session = RfbSession::Create();
    ConnectionConfig cfg{};
    cfg.host = "kimtaeyoungs-macbook-air.tailnet";
    cfg.port = rfb_port;
    cfg.socks5_proxy = "127.0.0.1:" + std::to_string(socks_port);
    cfg.initial_quality = QualityMode::Adaptive;

    SessionCallbacks cbs{};
    cbs.on_auth_required = [&](const AuthRequest& req) -> AuthCredentials {
        seen_banner = req.server_banner;
        AuthCredentials c{};
        c.username = "macuser";
        c.password = "macpass";
        return c;
    };
    cbs.on_connected = [&](uint32_t, uint32_t, const std::string& dname) {
        connected_name = dname;
    };
    cbs.on_frame_updated = [&](uint64_t gen) {
        if (gen >= 1) {
            std::lock_guard<std::mutex> lk(cv_mtx);
            got_frame = true;
            cv.notify_all();
        }
    };

    CHECK_TRUE(session->Start(cfg, cbs));
    {
        std::unique_lock<std::mutex> lk(cv_mtx);
        cv.wait_for(lk, std::chrono::seconds(5), [&]() { return got_frame; });
    }
    CHECK_TRUE(got_frame);
    CHECK_TRUE(auth_verified_on_server.load());
    CHECK_EQ(seen_banner, std::string("RFB 003.889"));
    CHECK_EQ(connected_name, std::string("MacBook-Air"));

    {
        std::vector<Rect> dirty;
        bool resized = false;
        auto lock = session->LockFramebuffer(&dirty, &resized);
        CHECK_EQ(lock.width, 64u);
        CHECK_EQ(lock.height, 40u);
        CHECK_TRUE(resized);
        CHECK_TRUE(!dirty.empty());
        CHECK_EQ(lock.pixels_bgra[0], 0x33u);
        CHECK_EQ(lock.pixels_bgra[1], 0x66u);
        CHECK_EQ(lock.pixels_bgra[2], 0x99u);
        CHECK_EQ(lock.pixels_bgra[3], 0xFFu);
    }

    auto stats = session->GetStats();
    CHECK_EQ(stats.fb_width, 64u);
    CHECK_EQ(stats.fb_height, 40u);
    CHECK_TRUE(stats.frames_received >= 1u);

    session->Stop();
    ::close(rfb_listen);
    ::close(socks_listen);
    rfb_thread.join();
    socks_thread.join();
}
#endif

}  // namespace

int main() {
    std::cout << "[test_rfb_core] Running SOCKS5 framing tests...\n";
    TestSocks5Framing();

    std::cout << "[test_rfb_core] Running Apple DH 4096-bit & VncAuth DES crypto tests...\n";
    TestCryptoAndAppleDh();

    std::cout << "[test_rfb_core] Running ZRLE / CopyRect / Raw / RichCursor decoder tests...\n";
    TestDecodersAndCursor();

    std::cout << "[test_rfb_core] Running Adaptive Quality & 6ms rate-limiter tests...\n";
    TestAdaptiveQualityTransitions();

#ifndef _WIN32
    std::cout << "[test_rfb_core] Running end-to-end RFB 003.889 + SOCKS5 session test...\n";
    TestEndToEndRfbSessionOverSocks5();
#endif

    if (g_failures > 0) {
        std::cerr << "[test_rfb_core] FAILED (" << g_failures << " failure(s) out of "
                  << g_checks << " checks)\n";
        return 1;
    }
    std::cout << "[test_rfb_core] ALL " << g_checks << " CHECKS PASSED!\n";
    return 0;
}
