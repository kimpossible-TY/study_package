#include "rfb_decoder.h"

#include <algorithm>
#include <cstring>

namespace mac_mirror {

namespace {

constexpr size_t kMaxCompressedRectBytes = 64u * 1024u * 1024u;    // 64 MB sanity cap
constexpr size_t kMaxUncompressedRectBytes = 128u * 1024u * 1024u; // 128 MB sanity cap
constexpr uint32_t kMaxFramebufferDim = 16384u;

constexpr uint16_t ReadBE16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) |
                                 static_cast<uint16_t>(p[1]));
}

constexpr uint32_t ReadBE32(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

inline void WriteBE16(uint8_t* p, uint16_t v) noexcept {
    p[0] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[1] = static_cast<uint8_t>(v & 0xFFu);
}

// Packs [B, G, R, A] so that std::memcpy to memory yields byte[0]=B, byte[1]=G, byte[2]=R, byte[3]=A
// on little-endian architectures (x86_64 Windows & ARM64/x86_64 macOS).
constexpr uint32_t PackBgraLe(uint8_t b, uint8_t g, uint8_t r, uint8_t a = 0xFFu) noexcept {
    return static_cast<uint32_t>(b) |
           (static_cast<uint32_t>(g) << 8) |
           (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(a) << 24);
}

uint8_t ScaleChannelTo8Bit(uint32_t val, uint16_t max_val) noexcept {
    if (max_val == 0) return 0;
    if (max_val == 255) return static_cast<uint8_t>(val & 0xFFu);
    uint32_t clamped = std::min<uint32_t>(val, max_val);
    return static_cast<uint8_t>((clamped * 255u + (max_val / 2u)) / max_val);
}

struct OptionalLockGuard {
    explicit OptionalLockGuard(std::mutex* m) : mtx(m) {
        if (mtx) mtx->lock();
    }
    ~OptionalLockGuard() {
        if (mtx) mtx->unlock();
    }
    OptionalLockGuard(const OptionalLockGuard&) = delete;
    OptionalLockGuard& operator=(const OptionalLockGuard&) = delete;
    std::mutex* mtx = nullptr;
};

}  // namespace

RfbPixelFormat RfbPixelFormat::Bgrx8888() noexcept {
    RfbPixelFormat f{};
    f.bits_per_pixel = 32;
    f.depth = 24;
    f.big_endian_flag = 0;
    f.true_colour_flag = 1;
    f.red_max = 255;
    f.green_max = 255;
    f.blue_max = 255;
    f.red_shift = 16;
    f.green_shift = 8;
    f.blue_shift = 0;
    return f;
}

RfbPixelFormat RfbPixelFormat::Rgb565() noexcept {
    RfbPixelFormat f{};
    f.bits_per_pixel = 16;
    f.depth = 16;
    f.big_endian_flag = 0;
    f.true_colour_flag = 1;
    f.red_max = 31;
    f.green_max = 63;
    f.blue_max = 31;
    f.red_shift = 11;
    f.green_shift = 5;
    f.blue_shift = 0;
    return f;
}

void RfbPixelFormat::Serialize(uint8_t out16[16]) const noexcept {
    std::memset(out16, 0, 16);
    out16[0] = bits_per_pixel;
    out16[1] = depth;
    out16[2] = big_endian_flag ? 1u : 0u;
    out16[3] = true_colour_flag ? 1u : 0u;
    WriteBE16(out16 + 4, red_max);
    WriteBE16(out16 + 6, green_max);
    WriteBE16(out16 + 8, blue_max);
    out16[10] = red_shift;
    out16[11] = green_shift;
    out16[12] = blue_shift;
}

RfbPixelFormat RfbPixelFormat::Parse(const uint8_t in16[16]) noexcept {
    RfbPixelFormat f{};
    f.bits_per_pixel = in16[0];
    f.depth = in16[1];
    f.big_endian_flag = in16[2];
    f.true_colour_flag = in16[3];
    f.red_max = ReadBE16(in16 + 4);
    f.green_max = ReadBE16(in16 + 6);
    f.blue_max = ReadBE16(in16 + 8);
    f.red_shift = in16[10];
    f.green_shift = in16[11];
    f.blue_shift = in16[12];
    return f;
}

RfbDecoder::RfbDecoder() {
    for (uint32_t v = 0; v < 65536u; ++v) {
        uint8_t r5 = static_cast<uint8_t>((v >> 11) & 0x1Fu);
        uint8_t g6 = static_cast<uint8_t>((v >> 5) & 0x3Fu);
        uint8_t b5 = static_cast<uint8_t>(v & 0x1Fu);
        uint8_t r8 = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
        uint8_t g8 = static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
        uint8_t b8 = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
        rgb565_lut_[v] = PackBgraLe(b8, g8, r8, 0xFFu);
    }
    for (uint32_t i = 0; i < 256u; ++i) {
        uint8_t c = static_cast<uint8_t>(i);
        colour_map_[i] = PackBgraLe(c, c, c, 0xFFu);
    }
    SetPixelFormat(RfbPixelFormat::Bgrx8888());
}

RfbDecoder::~RfbDecoder() {
    if (zrle_initialized_) {
        mz_inflateEnd(&zrle_stream_);
        zrle_initialized_ = false;
    }
    if (zlib_initialized_) {
        mz_inflateEnd(&zlib_stream_);
        zlib_initialized_ = false;
    }
}

bool RfbDecoder::ResetSession(uint32_t width,
                              uint32_t height,
                              const RfbPixelFormat& initial_format,
                              std::mutex* fb_mutex) {
    if (width == 0 || height == 0 || width > kMaxFramebufferDim || height > kMaxFramebufferDim) {
        return false;
    }

    if (zrle_initialized_) {
        mz_inflateEnd(&zrle_stream_);
        zrle_initialized_ = false;
    }
    std::memset(&zrle_stream_, 0, sizeof(zrle_stream_));

    if (zlib_initialized_) {
        mz_inflateEnd(&zlib_stream_);
        zlib_initialized_ = false;
    }
    std::memset(&zlib_stream_, 0, sizeof(zlib_stream_));

    fb_mutex_ = fb_mutex;
    SetPixelFormat(initial_format);

    OptionalLockGuard guard(fb_mutex_);
    width_ = width;
    height_ = height;
    size_t total_bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    fb_bgra_.assign(total_bytes, 0u);
    for (size_t off = 3; off < total_bytes; off += 4) {
        fb_bgra_[off] = 0xFFu;
    }
    return true;
}

void RfbDecoder::SetPixelFormat(const RfbPixelFormat& fmt) noexcept {
    format_ = fmt;
    UpdateFormatMetadata();
}

void RfbDecoder::SetColourMapEntry(uint16_t index,
                                   uint16_t r16,
                                   uint16_t g16,
                                   uint16_t b16) noexcept {
    if (index < 256u) {
        colour_map_[index] = PackBgraLe(static_cast<uint8_t>(b16 >> 8),
                                        static_cast<uint8_t>(g16 >> 8),
                                        static_cast<uint8_t>(r16 >> 8),
                                        0xFFu);
    }
}

void RfbDecoder::UpdateFormatMetadata() noexcept {
    bytes_per_pixel_ = std::max<uint8_t>(1u, static_cast<uint8_t>((format_.bits_per_pixel + 7u) / 8u));
    fast_bgrx8888_ = format_.IsFastBgrx8888();
    fast_rgb565_ = format_.IsFastRgb565();

    if (fast_bgrx8888_) {
        bytes_per_cpixel_ = 3;
        cpixel_mode_ = CpixelMode::FastBgrLs3;
        return;
    }

    if (format_.true_colour_flag != 0 && format_.bits_per_pixel == 32 && format_.depth <= 24) {
        uint32_t max_rgb =
            (static_cast<uint32_t>(format_.red_max) << format_.red_shift) |
            (static_cast<uint32_t>(format_.green_max) << format_.green_shift) |
            (static_cast<uint32_t>(format_.blue_max) << format_.blue_shift);
        bool fits_ls3 = (max_rgb & 0xFF000000u) == 0u;
        bool fits_ms3 = (max_rgb & 0x000000FFu) == 0u;
        if (fits_ls3) {
            bytes_per_cpixel_ = 3;
            cpixel_mode_ = CpixelMode::GenericLs3;
            return;
        }
        if (fits_ms3) {
            bytes_per_cpixel_ = 3;
            cpixel_mode_ = CpixelMode::GenericMs3;
            return;
        }
    }

    bytes_per_cpixel_ = bytes_per_pixel_;
    cpixel_mode_ = CpixelMode::SameAsPixel;
}

uint32_t RfbDecoder::DecodePixelToBgraU32(const uint8_t* src) const noexcept {
    if (fast_bgrx8888_) {
        return PackBgraLe(src[0], src[1], src[2], 0xFFu);
    }
    if (fast_rgb565_) {
        uint16_t v = static_cast<uint16_t>(src[0]) | (static_cast<uint16_t>(src[1]) << 8);
        return rgb565_lut_[v];
    }

    uint32_t raw = 0;
    if (bytes_per_pixel_ == 1) {
        raw = src[0];
    } else if (bytes_per_pixel_ == 2) {
        raw = format_.big_endian_flag
                  ? ((static_cast<uint32_t>(src[0]) << 8) | src[1])
                  : (static_cast<uint32_t>(src[0]) | (static_cast<uint32_t>(src[1]) << 8));
    } else {
        raw = format_.big_endian_flag
                  ? ReadBE32(src)
                  : (static_cast<uint32_t>(src[0]) |
                     (static_cast<uint32_t>(src[1]) << 8) |
                     (static_cast<uint32_t>(src[2]) << 16) |
                     (static_cast<uint32_t>(src[3]) << 24));
    }

    if (format_.true_colour_flag == 0) {
        return colour_map_[raw & 0xFFu];
    }

    uint8_t r = ScaleChannelTo8Bit((raw >> format_.red_shift) & format_.red_max, format_.red_max);
    uint8_t g = ScaleChannelTo8Bit((raw >> format_.green_shift) & format_.green_max, format_.green_max);
    uint8_t b = ScaleChannelTo8Bit((raw >> format_.blue_shift) & format_.blue_max, format_.blue_max);
    return PackBgraLe(b, g, r, 0xFFu);
}

uint32_t RfbDecoder::DecodeCpixelToBgraU32(const uint8_t* src) const noexcept {
    if (cpixel_mode_ == CpixelMode::FastBgrLs3) {
        return PackBgraLe(src[0], src[1], src[2], 0xFFu);
    }
    if (fast_rgb565_) {
        uint16_t v = static_cast<uint16_t>(src[0]) | (static_cast<uint16_t>(src[1]) << 8);
        return rgb565_lut_[v];
    }
    if (cpixel_mode_ == CpixelMode::SameAsPixel) {
        return DecodePixelToBgraU32(src);
    }

    uint32_t raw = 0;
    if (cpixel_mode_ == CpixelMode::GenericLs3) {
        raw = format_.big_endian_flag
                  ? ((static_cast<uint32_t>(src[0]) << 16) |
                     (static_cast<uint32_t>(src[1]) << 8) |
                     static_cast<uint32_t>(src[2]))
                  : (static_cast<uint32_t>(src[0]) |
                     (static_cast<uint32_t>(src[1]) << 8) |
                     (static_cast<uint32_t>(src[2]) << 16));
    } else {
        raw = format_.big_endian_flag
                  ? ((static_cast<uint32_t>(src[0]) << 24) |
                     (static_cast<uint32_t>(src[1]) << 16) |
                     (static_cast<uint32_t>(src[2]) << 8))
                  : ((static_cast<uint32_t>(src[0]) << 8) |
                     (static_cast<uint32_t>(src[1]) << 16) |
                     (static_cast<uint32_t>(src[2]) << 24));
    }

    uint8_t r = ScaleChannelTo8Bit((raw >> format_.red_shift) & format_.red_max, format_.red_max);
    uint8_t g = ScaleChannelTo8Bit((raw >> format_.green_shift) & format_.green_max, format_.green_max);
    uint8_t b = ScaleChannelTo8Bit((raw >> format_.blue_shift) & format_.blue_max, format_.blue_max);
    return PackBgraLe(b, g, r, 0xFFu);
}

bool RfbDecoder::InflatePacket(mz_stream* stream,
                               bool* stream_init_flag,
                               const uint8_t* compressed,
                               size_t compressed_len,
                               std::vector<uint8_t>* out_uncompressed,
                               std::string* error_out) {
    if (!*stream_init_flag) {
        std::memset(stream, 0, sizeof(*stream));
        if (mz_inflateInit(stream) != MZ_OK) {
            if (error_out) *error_out = "mz_inflateInit failed";
            return false;
        }
        *stream_init_flag = true;
    }

    out_uncompressed->clear();
    if (compressed_len == 0) {
        return true;
    }

    size_t out_offset = 0;
    size_t initial_cap = std::max<size_t>(compressed_len * 4u, 16384u);
    initial_cap = std::min<size_t>(initial_cap, kMaxUncompressedRectBytes);
    out_uncompressed->resize(initial_cap);

    stream->next_in = compressed;
    stream->avail_in = static_cast<unsigned int>(compressed_len);

    while (true) {
        if (out_offset == out_uncompressed->size()) {
            if (out_uncompressed->size() >= kMaxUncompressedRectBytes) {
                if (error_out) *error_out = "Decompressed RFB rectangle exceeds 128 MB safety limit";
                return false;
            }
            size_t next_cap = std::min<size_t>(out_uncompressed->size() * 2u, kMaxUncompressedRectBytes);
            out_uncompressed->resize(next_cap);
        }

        unsigned int avail_before = static_cast<unsigned int>(
            std::min<size_t>(out_uncompressed->size() - out_offset, 0x40000000u));
        stream->next_out = out_uncompressed->data() + out_offset;
        stream->avail_out = avail_before;

        int rc = mz_inflate(stream, MZ_SYNC_FLUSH);
        size_t produced = static_cast<size_t>(avail_before - stream->avail_out);
        out_offset += produced;

        if (rc == MZ_STREAM_END) {
            break;
        }
        if (rc == MZ_BUF_ERROR) {
            if (stream->avail_in == 0 && produced == 0) {
                break;
            }
            if (produced == 0 && out_offset < out_uncompressed->size()) {
                if (error_out) *error_out = "Truncated zlib stream in RFB rectangle";
                return false;
            }
            continue;
        }
        if (rc != MZ_OK) {
            if (error_out) *error_out = "Corrupt zlib stream in RFB rectangle (code " + std::to_string(rc) + ")";
            return false;
        }
        // Note: continue calling mz_inflate even when avail_in == 0 until MZ_BUF_ERROR
        // so any buffered output crossing miniz's 32 KB ring-buffer boundary is fully drained!
    }

    out_uncompressed->resize(out_offset);
    return true;
}

void RfbDecoder::BlitStageToFramebuffer(uint16_t x, uint16_t y, uint16_t w, uint16_t h) noexcept {
    if (w == 0 || h == 0) return;
    OptionalLockGuard guard(fb_mutex_);
    if (static_cast<uint32_t>(x) + w > width_ || static_cast<uint32_t>(y) + h > height_) {
        return;
    }
    const size_t row_bytes = static_cast<size_t>(w) * 4u;
    const size_t fb_stride = static_cast<size_t>(width_) * 4u;
    for (uint16_t row = 0; row < h; ++row) {
        uint8_t* dst = fb_bgra_.data() + static_cast<size_t>(y + row) * fb_stride +
                       static_cast<size_t>(x) * 4u;
        const uint32_t* src = stage_bgra32_.data() + static_cast<size_t>(row) * w;
        std::memcpy(dst, src, row_bytes);
    }
}

bool RfbDecoder::DecodeRawRect(uint16_t x,
                               uint16_t y,
                               uint16_t w,
                               uint16_t h,
                               const ByteReadFn& reader,
                               std::string* error_out) {
    if (w == 0 || h == 0) return true;
    size_t pixel_count = static_cast<size_t>(w) * static_cast<size_t>(h);
    size_t wire_bytes = pixel_count * bytes_per_pixel_;
    net_scratch_.resize(wire_bytes);
    if (!reader(net_scratch_.data(), wire_bytes, error_out)) {
        return false;
    }

    stage_bgra32_.resize(pixel_count);
    const uint8_t* src = net_scratch_.data();
    if (fast_bgrx8888_) {
        for (size_t i = 0; i < pixel_count; ++i) {
            stage_bgra32_[i] = PackBgraLe(src[i * 4], src[i * 4 + 1], src[i * 4 + 2], 0xFFu);
        }
    } else if (fast_rgb565_) {
        for (size_t i = 0; i < pixel_count; ++i) {
            uint16_t v = static_cast<uint16_t>(src[i * 2]) |
                         (static_cast<uint16_t>(src[i * 2 + 1]) << 8);
            stage_bgra32_[i] = rgb565_lut_[v];
        }
    } else {
        for (size_t i = 0; i < pixel_count; ++i) {
            stage_bgra32_[i] = DecodePixelToBgraU32(src + i * bytes_per_pixel_);
        }
    }

    BlitStageToFramebuffer(x, y, w, h);
    return true;
}

bool RfbDecoder::DecodeCopyRect(uint16_t x,
                                uint16_t y,
                                uint16_t w,
                                uint16_t h,
                                const ByteReadFn& reader,
                                std::string* error_out) {
    uint8_t hdr[4];
    if (!reader(hdr, sizeof(hdr), error_out)) {
        return false;
    }
    if (w == 0 || h == 0) return true;

    uint16_t src_x = ReadBE16(hdr);
    uint16_t src_y = ReadBE16(hdr + 2);

    OptionalLockGuard guard(fb_mutex_);
    if (static_cast<uint32_t>(x) + w > width_ ||
        static_cast<uint32_t>(y) + h > height_ ||
        static_cast<uint32_t>(src_x) + w > width_ ||
        static_cast<uint32_t>(src_y) + h > height_) {
        if (error_out) *error_out = "CopyRect coordinates out of framebuffer bounds";
        return false;
    }

    if (x == src_x && y == src_y) {
        return true;
    }

    const size_t fb_stride = static_cast<size_t>(width_) * 4u;
    const size_t row_bytes = static_cast<size_t>(w) * 4u;
    if (y > src_y) {
        // Copy bottom-to-top when moving downward so overlapping rows are never overwritten early
        for (uint32_t row = h; row > 0; --row) {
            uint32_t r = row - 1;
            uint8_t* dst_row = fb_bgra_.data() + static_cast<size_t>(y + r) * fb_stride +
                               static_cast<size_t>(x) * 4u;
            const uint8_t* src_row = fb_bgra_.data() + static_cast<size_t>(src_y + r) * fb_stride +
                                     static_cast<size_t>(src_x) * 4u;
            std::memmove(dst_row, src_row, row_bytes);
        }
    } else {
        // Copy top-to-bottom (std::memmove handles horizontal overlap when y == src_y)
        for (uint32_t r = 0; r < h; ++r) {
            uint8_t* dst_row = fb_bgra_.data() + static_cast<size_t>(y + r) * fb_stride +
                               static_cast<size_t>(x) * 4u;
            const uint8_t* src_row = fb_bgra_.data() + static_cast<size_t>(src_y + r) * fb_stride +
                                     static_cast<size_t>(src_x) * 4u;
            std::memmove(dst_row, src_row, row_bytes);
        }
    }
    return true;
}

bool RfbDecoder::DecodeHextileRect(uint16_t x,
                                   uint16_t y,
                                   uint16_t w,
                                   uint16_t h,
                                   const ByteReadFn& reader,
                                   std::string* error_out) {
    if (w == 0 || h == 0) return true;
    stage_bgra32_.assign(static_cast<size_t>(w) * static_cast<size_t>(h), PackBgraLe(0, 0, 0, 0xFFu));

    uint32_t bg = PackBgraLe(0, 0, 0, 0xFFu);
    uint32_t fg = PackBgraLe(0, 0, 0, 0xFFu);
    uint8_t px_buf[4];

    for (uint16_t ty = 0; ty < h; ty = static_cast<uint16_t>(ty + 16)) {
        uint16_t th = std::min<uint16_t>(16u, static_cast<uint16_t>(h - ty));
        for (uint16_t tx = 0; tx < w; tx = static_cast<uint16_t>(tx + 16)) {
            uint16_t tw = std::min<uint16_t>(16u, static_cast<uint16_t>(w - tx));

            uint8_t subenc = 0;
            if (!reader(&subenc, 1, error_out)) return false;

            if ((subenc & 0x01u) != 0u) {
                // Raw 16x16 tile
                size_t tile_px = static_cast<size_t>(tw) * static_cast<size_t>(th);
                net_scratch_.resize(tile_px * bytes_per_pixel_);
                if (!reader(net_scratch_.data(), net_scratch_.size(), error_out)) return false;
                const uint8_t* s = net_scratch_.data();
                for (uint16_t r = 0; r < th; ++r) {
                    uint32_t* dst = stage_bgra32_.data() + static_cast<size_t>(ty + r) * w + tx;
                    for (uint16_t c = 0; c < tw; ++c) {
                        dst[c] = DecodePixelToBgraU32(s);
                        s += bytes_per_pixel_;
                    }
                }
                continue;
            }

            if ((subenc & 0x02u) != 0u) {
                if (!reader(px_buf, bytes_per_pixel_, error_out)) return false;
                bg = DecodePixelToBgraU32(px_buf);
            }
            for (uint16_t r = 0; r < th; ++r) {
                uint32_t* dst = stage_bgra32_.data() + static_cast<size_t>(ty + r) * w + tx;
                std::fill(dst, dst + tw, bg);
            }

            if ((subenc & 0x04u) != 0u) {
                if (!reader(px_buf, bytes_per_pixel_, error_out)) return false;
                fg = DecodePixelToBgraU32(px_buf);
            }

            if ((subenc & 0x08u) != 0u) {
                uint8_t n_subrects = 0;
                if (!reader(&n_subrects, 1, error_out)) return false;
                bool coloured = (subenc & 0x10u) != 0u;
                for (uint32_t i = 0; i < n_subrects; ++i) {
                    uint32_t sub_color = fg;
                    if (coloured) {
                        if (!reader(px_buf, bytes_per_pixel_, error_out)) return false;
                        sub_color = DecodePixelToBgraU32(px_buf);
                    }
                    uint8_t xy_wh[2];
                    if (!reader(xy_wh, 2, error_out)) return false;
                    uint16_t sx = static_cast<uint16_t>((xy_wh[0] >> 4) & 0x0Fu);
                    uint16_t sy = static_cast<uint16_t>(xy_wh[0] & 0x0Fu);
                    uint16_t sw = static_cast<uint16_t>(((xy_wh[1] >> 4) & 0x0Fu) + 1u);
                    uint16_t sh = static_cast<uint16_t>((xy_wh[1] & 0x0Fu) + 1u);
                    if (sx + sw > tw || sy + sh > th) {
                        if (error_out) *error_out = "Hextile subrect out of tile bounds";
                        return false;
                    }
                    for (uint16_t sr = 0; sr < sh; ++sr) {
                        uint32_t* dst = stage_bgra32_.data() +
                                        static_cast<size_t>(ty + sy + sr) * w + (tx + sx);
                        std::fill(dst, dst + sw, sub_color);
                    }
                }
            }
        }
    }

    BlitStageToFramebuffer(x, y, w, h);
    return true;
}

bool RfbDecoder::DecodeZlibRect(uint16_t x,
                                uint16_t y,
                                uint16_t w,
                                uint16_t h,
                                const ByteReadFn& reader,
                                std::string* error_out) {
    uint8_t len_hdr[4];
    if (!reader(len_hdr, 4, error_out)) return false;
    uint32_t comp_len = ReadBE32(len_hdr);
    if (comp_len > kMaxCompressedRectBytes) {
        if (error_out) *error_out = "Zlib compressed length exceeds 64 MB safety limit";
        return false;
    }

    net_scratch_.resize(comp_len);
    if (comp_len > 0 && !reader(net_scratch_.data(), comp_len, error_out)) {
        return false;
    }
    if (!InflatePacket(&zlib_stream_, &zlib_initialized_,
                       net_scratch_.data(), comp_len, &inflate_scratch_, error_out)) {
        return false;
    }
    if (w == 0 || h == 0) return true;

    size_t pixel_count = static_cast<size_t>(w) * static_cast<size_t>(h);
    size_t expected = pixel_count * bytes_per_pixel_;
    if (inflate_scratch_.size() < expected) {
        if (error_out) *error_out = "Zlib rectangle decompressed fewer bytes than expected";
        return false;
    }

    stage_bgra32_.resize(pixel_count);
    const uint8_t* src = inflate_scratch_.data();
    for (size_t i = 0; i < pixel_count; ++i) {
        stage_bgra32_[i] = DecodePixelToBgraU32(src + i * bytes_per_pixel_);
    }

    BlitStageToFramebuffer(x, y, w, h);
    return true;
}

bool RfbDecoder::DecodeZrleRect(uint16_t x,
                                uint16_t y,
                                uint16_t w,
                                uint16_t h,
                                const ByteReadFn& reader,
                                std::string* error_out) {
    uint8_t len_hdr[4];
    if (!reader(len_hdr, 4, error_out)) return false;
    uint32_t comp_len = ReadBE32(len_hdr);
    if (comp_len > kMaxCompressedRectBytes) {
        if (error_out) *error_out = "ZRLE compressed length exceeds 64 MB safety limit";
        return false;
    }

    net_scratch_.resize(comp_len);
    if (comp_len > 0 && !reader(net_scratch_.data(), comp_len, error_out)) {
        return false;
    }
    if (!InflatePacket(&zrle_stream_, &zrle_initialized_,
                       net_scratch_.data(), comp_len, &inflate_scratch_, error_out)) {
        return false;
    }
    if (w == 0 || h == 0) return true;

    stage_bgra32_.resize(static_cast<size_t>(w) * static_cast<size_t>(h));

    const uint8_t* cur = inflate_scratch_.data();
    const uint8_t* end = cur + inflate_scratch_.size();
    const size_t cp_bytes = bytes_per_cpixel_;
    std::array<uint32_t, 128> palette{};

    for (uint16_t ty = 0; ty < h; ty = static_cast<uint16_t>(ty + 64)) {
        uint16_t th = std::min<uint16_t>(64u, static_cast<uint16_t>(h - ty));
        for (uint16_t tx = 0; tx < w; tx = static_cast<uint16_t>(tx + 64)) {
            uint16_t tw = std::min<uint16_t>(64u, static_cast<uint16_t>(w - tx));
            size_t tile_px = static_cast<size_t>(tw) * static_cast<size_t>(th);

            if (cur >= end) {
                if (error_out) *error_out = "Truncated ZRLE tile header";
                return false;
            }
            uint8_t subenc = *cur++;

            if (subenc == 0u) {
                // Raw CPIXEL tile
                size_t needed = tile_px * cp_bytes;
                if (static_cast<size_t>(end - cur) < needed) {
                    if (error_out) *error_out = "Truncated ZRLE raw tile";
                    return false;
                }
                if (cpixel_mode_ == CpixelMode::FastBgrLs3) {
                    for (uint16_t r = 0; r < th; ++r) {
                        uint32_t* dst = stage_bgra32_.data() + static_cast<size_t>(ty + r) * w + tx;
                        for (uint16_t c = 0; c < tw; ++c) {
                            dst[c] = PackBgraLe(cur[0], cur[1], cur[2], 0xFFu);
                            cur += 3;
                        }
                    }
                } else if (fast_rgb565_) {
                    for (uint16_t r = 0; r < th; ++r) {
                        uint32_t* dst = stage_bgra32_.data() + static_cast<size_t>(ty + r) * w + tx;
                        for (uint16_t c = 0; c < tw; ++c) {
                            uint16_t v = static_cast<uint16_t>(cur[0]) |
                                         (static_cast<uint16_t>(cur[1]) << 8);
                            dst[c] = rgb565_lut_[v];
                            cur += 2;
                        }
                    }
                } else {
                    for (uint16_t r = 0; r < th; ++r) {
                        uint32_t* dst = stage_bgra32_.data() + static_cast<size_t>(ty + r) * w + tx;
                        for (uint16_t c = 0; c < tw; ++c) {
                            dst[c] = DecodeCpixelToBgraU32(cur);
                            cur += cp_bytes;
                        }
                    }
                }
            } else if (subenc == 1u) {
                // Solid-color tile
                if (static_cast<size_t>(end - cur) < cp_bytes) {
                    if (error_out) *error_out = "Truncated ZRLE solid tile";
                    return false;
                }
                uint32_t color = DecodeCpixelToBgraU32(cur);
                cur += cp_bytes;
                for (uint16_t r = 0; r < th; ++r) {
                    uint32_t* dst = stage_bgra32_.data() + static_cast<size_t>(ty + r) * w + tx;
                    std::fill(dst, dst + tw, color);
                }
            } else if (subenc >= 2u && subenc <= 16u) {
                // Packed palette tile (1, 2, or 4 bits per pixel, byte-padded per row)
                size_t pal_size = subenc;
                if (static_cast<size_t>(end - cur) < pal_size * cp_bytes) {
                    if (error_out) *error_out = "Truncated ZRLE packed palette";
                    return false;
                }
                for (size_t p = 0; p < pal_size; ++p) {
                    palette[p] = DecodeCpixelToBgraU32(cur);
                    cur += cp_bytes;
                }

                uint32_t bpp_idx = (pal_size == 2) ? 1u : ((pal_size <= 4) ? 2u : 4u);
                size_t row_packed_bytes = (static_cast<size_t>(tw) * bpp_idx + 7u) / 8u;
                size_t total_packed = row_packed_bytes * th;
                if (static_cast<size_t>(end - cur) < total_packed) {
                    if (error_out) *error_out = "Truncated ZRLE packed palette indices";
                    return false;
                }

                uint8_t idx_mask = static_cast<uint8_t>((1u << bpp_idx) - 1u);
                for (uint16_t r = 0; r < th; ++r) {
                    uint32_t* dst = stage_bgra32_.data() + static_cast<size_t>(ty + r) * w + tx;
                    uint8_t cur_byte = 0;
                    int bits_left = 0;
                    for (uint16_t c = 0; c < tw; ++c) {
                        if (bits_left == 0) {
                            cur_byte = *cur++;
                            bits_left = 8;
                        }
                        bits_left -= static_cast<int>(bpp_idx);
                        uint8_t idx = static_cast<uint8_t>((cur_byte >> bits_left) & idx_mask);
                        if (idx >= pal_size) {
                            if (error_out) *error_out = "ZRLE packed palette index out of range";
                            return false;
                        }
                        dst[c] = palette[idx];
                    }
                }
            } else if (subenc == 128u) {
                // Plain RLE
                size_t emitted = 0;
                while (emitted < tile_px) {
                    if (static_cast<size_t>(end - cur) < cp_bytes + 1u) {
                        if (error_out) *error_out = "Truncated ZRLE plain RLE run";
                        return false;
                    }
                    uint32_t color = DecodeCpixelToBgraU32(cur);
                    cur += cp_bytes;

                    size_t run_len = 1u;
                    uint8_t b = 0;
                    do {
                        if (cur >= end) {
                            if (error_out) *error_out = "Truncated ZRLE plain RLE length";
                            return false;
                        }
                        b = *cur++;
                        run_len += b;
                    } while (b == 255u);

                    if (emitted + run_len > tile_px) {
                        if (error_out) *error_out = "ZRLE plain RLE run exceeds tile size";
                        return false;
                    }
                    for (size_t k = 0; k < run_len; ++k) {
                        size_t pos = emitted + k;
                        stage_bgra32_[static_cast<size_t>(ty + (pos / tw)) * w + (tx + (pos % tw))] = color;
                    }
                    emitted += run_len;
                }
            } else if (subenc >= 130u) {
                // Palette RLE (palette size = subenc - 128, 2..127)
                size_t pal_size = static_cast<size_t>(subenc - 128u);
                if (static_cast<size_t>(end - cur) < pal_size * cp_bytes) {
                    if (error_out) *error_out = "Truncated ZRLE RLE palette";
                    return false;
                }
                for (size_t p = 0; p < pal_size; ++p) {
                    palette[p] = DecodeCpixelToBgraU32(cur);
                    cur += cp_bytes;
                }

                size_t emitted = 0;
                while (emitted < tile_px) {
                    if (cur >= end) {
                        if (error_out) *error_out = "Truncated ZRLE palette RLE index";
                        return false;
                    }
                    uint8_t idx_byte = *cur++;
                    uint8_t idx = static_cast<uint8_t>(idx_byte & 0x7Fu);
                    if (idx >= pal_size) {
                        if (error_out) *error_out = "ZRLE palette RLE index out of range";
                        return false;
                    }
                    uint32_t color = palette[idx];
                    size_t run_len = 1u;
                    if ((idx_byte & 0x80u) != 0u) {
                        uint8_t b = 0;
                        do {
                            if (cur >= end) {
                                if (error_out) *error_out = "Truncated ZRLE palette RLE length";
                                return false;
                            }
                            b = *cur++;
                            run_len += b;
                        } while (b == 255u);
                    }

                    if (emitted + run_len > tile_px) {
                        if (error_out) *error_out = "ZRLE palette RLE run exceeds tile size";
                        return false;
                    }
                    for (size_t k = 0; k < run_len; ++k) {
                        size_t pos = emitted + k;
                        stage_bgra32_[static_cast<size_t>(ty + (pos / tw)) * w + (tx + (pos % tw))] = color;
                    }
                    emitted += run_len;
                }
            } else {
                // 17..127 and 129 are unused/reserved in RFC 6143 §7.7.6
                if (error_out) {
                    *error_out = "Invalid ZRLE subencoding: " + std::to_string(subenc);
                }
                return false;
            }
        }
    }

    BlitStageToFramebuffer(x, y, w, h);
    return true;
}

bool RfbDecoder::DecodeRichCursor(uint16_t hotspot_x,
                                  uint16_t hotspot_y,
                                  uint16_t w,
                                  uint16_t h,
                                  const ByteReadFn& reader,
                                  CursorState* out_cursor,
                                  std::string* error_out) {
    CursorState cs{};
    cs.has_server_cursor = true;
    cs.hotspot_x = hotspot_x;
    cs.hotspot_y = hotspot_y;
    cs.width = w;
    cs.height = h;

    if (w == 0 || h == 0) {
        cs.cursor_is_blank = true;
        if (out_cursor) *out_cursor = std::move(cs);
        return true;
    }

    size_t pixel_count = static_cast<size_t>(w) * static_cast<size_t>(h);
    size_t pixel_bytes = pixel_count * bytes_per_pixel_;
    size_t mask_stride = (static_cast<size_t>(w) + 7u) / 8u;
    size_t mask_bytes = mask_stride * static_cast<size_t>(h);

    net_scratch_.resize(pixel_bytes + mask_bytes);
    if (!reader(net_scratch_.data(), net_scratch_.size(), error_out)) {
        return false;
    }

    const uint8_t* px_src = net_scratch_.data();
    const uint8_t* mask_src = net_scratch_.data() + pixel_bytes;

    cs.pixels_bgra.assign(pixel_count * 4u, 0u);
    bool any_visible = false;

    for (uint16_t row = 0; row < h; ++row) {
        for (uint16_t col = 0; col < w; ++col) {
            size_t idx = static_cast<size_t>(row) * w + col;
            uint8_t mbyte = mask_src[static_cast<size_t>(row) * mask_stride + (col / 8u)];
            bool opaque = ((mbyte & (0x80u >> (col % 8u))) != 0u);
            if (opaque) {
                uint32_t bgra = DecodePixelToBgraU32(px_src + idx * bytes_per_pixel_);
                std::memcpy(cs.pixels_bgra.data() + idx * 4u, &bgra, 4u);
                any_visible = true;
            }
        }
    }

    cs.cursor_is_blank = !any_visible;
    if (out_cursor) *out_cursor = std::move(cs);
    return true;
}

bool RfbDecoder::HandleDesktopSize(uint16_t w, uint16_t h, std::string* error_out) {
    if (w == 0 || h == 0 || w > kMaxFramebufferDim || h > kMaxFramebufferDim) {
        if (error_out) *error_out = "Invalid DesktopSize dimensions";
        return false;
    }

    OptionalLockGuard guard(fb_mutex_);
    width_ = w;
    height_ = h;
    size_t total_bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
    fb_bgra_.assign(total_bytes, 0u);
    for (size_t off = 3; off < total_bytes; off += 4) {
        fb_bgra_[off] = 0xFFu;
    }
    return true;
}

bool RfbDecoder::DecodeRectangle(uint16_t x,
                                 uint16_t y,
                                 uint16_t w,
                                 uint16_t h,
                                 int32_t encoding,
                                 const ByteReadFn& reader,
                                 DecodeOutcome* out_outcome,
                                 CursorState* out_cursor_if_updated,
                                 std::string* error_out) {
    if (encoding == rfb_encoding::kCursor) {
        if (out_outcome) *out_outcome = DecodeOutcome::CursorUpdated;
        return DecodeRichCursor(x, y, w, h, reader, out_cursor_if_updated, error_out);
    }
    if (encoding == rfb_encoding::kDesktopSize) {
        if (out_outcome) *out_outcome = DecodeOutcome::DesktopResized;
        return HandleDesktopSize(w, h, error_out);
    }

    // Bounds check pixel rectangles before decoding into framebuffer
    if (static_cast<uint32_t>(x) + w > width_ || static_cast<uint32_t>(y) + h > height_) {
        if (error_out) {
            *error_out = "RFB rectangle (" + std::to_string(x) + "," + std::to_string(y) + " " +
                         std::to_string(w) + "x" + std::to_string(h) +
                         ") exceeds framebuffer (" + std::to_string(width_) + "x" +
                         std::to_string(height_) + ")";
        }
        return false;
    }

    if (out_outcome) *out_outcome = DecodeOutcome::PixelRectUpdated;
    switch (encoding) {
        case rfb_encoding::kRaw:
            return DecodeRawRect(x, y, w, h, reader, error_out);
        case rfb_encoding::kCopyRect:
            return DecodeCopyRect(x, y, w, h, reader, error_out);
        case rfb_encoding::kHextile:
            return DecodeHextileRect(x, y, w, h, reader, error_out);
        case rfb_encoding::kZlib:
            return DecodeZlibRect(x, y, w, h, reader, error_out);
        case rfb_encoding::kZRLE:
            return DecodeZrleRect(x, y, w, h, reader, error_out);
        default:
            if (error_out) {
                *error_out = "Unsupported RFB encoding type: " + std::to_string(encoding);
            }
            return false;
    }
}

}  // namespace mac_mirror
