#pragma once

#include "../include/rfb_session.h"
#include "miniz.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace mac_mirror {

namespace rfb_encoding {
constexpr int32_t kRaw         = 0;
constexpr int32_t kCopyRect    = 1;
constexpr int32_t kHextile     = 5;
constexpr int32_t kZlib        = 6;
constexpr int32_t kZRLE        = 16;
constexpr int32_t kCursor      = -239;  // RichCursor pseudo-encoding
constexpr int32_t kDesktopSize = -223;  // DesktopSize pseudo-encoding
}  // namespace rfb_encoding

// RFC 6143 §7.4 16-byte PIXEL_FORMAT descriptor.
struct RfbPixelFormat {
    uint8_t bits_per_pixel = 32;
    uint8_t depth = 24;
    uint8_t big_endian_flag = 0;
    uint8_t true_colour_flag = 1;
    uint16_t red_max = 255;
    uint16_t green_max = 255;
    uint16_t blue_max = 255;
    uint8_t red_shift = 16;
    uint8_t green_shift = 8;
    uint8_t blue_shift = 0;

    static RfbPixelFormat Bgrx8888() noexcept;
    static RfbPixelFormat Rgb565() noexcept;

    void Serialize(uint8_t out16[16]) const noexcept;
    static RfbPixelFormat Parse(const uint8_t in16[16]) noexcept;

    bool IsFastBgrx8888() const noexcept {
        return bits_per_pixel == 32 && depth == 24 && big_endian_flag == 0 &&
               true_colour_flag != 0 && red_max == 255 && green_max == 255 &&
               blue_max == 255 && red_shift == 16 && green_shift == 8 && blue_shift == 0;
    }

    bool IsFastRgb565() const noexcept {
        return bits_per_pixel == 16 && depth == 16 && big_endian_flag == 0 &&
               true_colour_flag != 0 && red_max == 31 && green_max == 63 &&
               blue_max == 31 && red_shift == 11 && green_shift == 5 && blue_shift == 0;
    }
};

enum class DecodeOutcome : uint8_t {
    PixelRectUpdated = 0,
    CursorUpdated    = 1,
    DesktopResized   = 2,
};

// High-throughput stateful RFB rectangle & RichCursor decoder.
// Keeps `fb_mutex` unlocked during network reads and zlib decompression, acquiring locks
// ONLY during fast in-memory BGRA32 framebuffer blits so the 120Hz UI render thread never stalls.
class RfbDecoder {
public:
    using ByteReadFn = std::function<bool(void* dst, size_t len, std::string* error_out)>;

    RfbDecoder();
    ~RfbDecoder();

    RfbDecoder(const RfbDecoder&) = delete;
    RfbDecoder& operator=(const RfbDecoder&) = delete;

    // Resets zlib streams and allocates initial BGRA32 framebuffer (`width * height * 4` bytes).
    bool ResetSession(uint32_t width,
                      uint32_t height,
                      const RfbPixelFormat& initial_format,
                      std::mutex* fb_mutex = nullptr);

    void SetPixelFormat(const RfbPixelFormat& fmt) noexcept;
    const RfbPixelFormat& GetPixelFormat() const noexcept { return format_; }

    void SetColourMapEntry(uint16_t index, uint16_t r16, uint16_t g16, uint16_t b16) noexcept;

    // Decodes a single rectangle `(x, y, w, h, encoding)` from `reader`.
    // If `fb_mutex` was passed to ResetSession, locks `fb_mutex` ONLY around in-memory pixel writes.
    bool DecodeRectangle(uint16_t x,
                         uint16_t y,
                         uint16_t w,
                         uint16_t h,
                         int32_t encoding,
                         const ByteReadFn& reader,
                         DecodeOutcome* out_outcome,
                         CursorState* out_cursor_if_updated,
                         std::string* error_out);

    // Direct framebuffer inspection (caller must hold `fb_mutex` if concurrent).
    const uint8_t* PixelsBgra() const noexcept { return fb_bgra_.data(); }
    uint32_t Width() const noexcept { return width_; }
    uint32_t Height() const noexcept { return height_; }
    uint32_t StrideBytes() const noexcept { return width_ * 4u; }

private:
    enum class CpixelMode : uint8_t {
        SameAsPixel  = 0,
        FastBgrLs3   = 1,  // 32bpp LE BGRX -> 3 bytes [B, G, R]
        GenericLs3   = 2,  // 3 bytes from least-significant 24 bits
        GenericMs3   = 3,  // 3 bytes from most-significant 24 bits
    };

    void UpdateFormatMetadata() noexcept;
    uint32_t DecodePixelToBgraU32(const uint8_t* src) const noexcept;
    uint32_t DecodeCpixelToBgraU32(const uint8_t* src) const noexcept;

    bool InflatePacket(mz_stream* stream,
                       bool* stream_init_flag,
                       const uint8_t* compressed,
                       size_t compressed_len,
                       std::vector<uint8_t>* out_uncompressed,
                       std::string* error_out);

    bool DecodeRawRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                       const ByteReadFn& reader, std::string* error_out);
    bool DecodeCopyRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                        const ByteReadFn& reader, std::string* error_out);
    bool DecodeHextileRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           const ByteReadFn& reader, std::string* error_out);
    bool DecodeZlibRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                        const ByteReadFn& reader, std::string* error_out);
    bool DecodeZrleTilesBuffer(uint16_t w, uint16_t h, bool require_exact_end,
                               std::string* error_out);
    bool DecodeZrleRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                        const ByteReadFn& reader, std::string* error_out);
    bool DecodeRichCursor(uint16_t hotspot_x, uint16_t hotspot_y, uint16_t w, uint16_t h,
                          const ByteReadFn& reader, CursorState* out_cursor, std::string* error_out);
    bool HandleDesktopSize(uint16_t w, uint16_t h, std::string* error_out);

    void BlitStageToFramebuffer(uint16_t x, uint16_t y, uint16_t w, uint16_t h) noexcept;

    std::mutex* fb_mutex_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    std::vector<uint8_t> fb_bgra_;

    RfbPixelFormat format_{};
    RfbPixelFormat prev_format_{};
    bool has_prev_format_ = false;
    uint32_t prev_format_grace_rects_ = 0;
    uint8_t bytes_per_pixel_ = 4;
    uint8_t bytes_per_cpixel_ = 3;
    CpixelMode cpixel_mode_ = CpixelMode::FastBgrLs3;
    bool fast_bgrx8888_ = true;
    bool fast_rgb565_ = false;

    std::array<uint32_t, 65536> rgb565_lut_{};
    std::array<uint32_t, 256> colour_map_{};

    mz_stream zrle_stream_{};
    bool zrle_initialized_ = false;
    mz_stream zlib_stream_{};
    bool zlib_initialized_ = false;

    // Reusable scratch buffers to avoid per-frame heap allocations
    std::vector<uint8_t> net_scratch_;
    std::vector<uint8_t> inflate_scratch_;
    std::vector<uint32_t> stage_bgra32_;
};

}  // namespace mac_mirror
