// SPDX-License-Identifier: GPL-2.0-or-later

#include "texture.h"

#include "byte_cursor.h"

#include <lz4.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>
#include <string_view>

namespace wallpaperx::we {
namespace {
void* stbAllocate(std::size_t size);
void* stbReallocate(void* block, std::size_t size);
void stbRelease(void* block);
} // namespace
} // namespace wallpaperx::we

// A private, PNG- and JPEG-only copy of stb_image: the renderer links the full one with its own
// limits, and STB_IMAGE_STATIC keeps the two from colliding when both end up in the daemon. Its
// allocations go through a per-decode budget: stb's inflate grows its output with no regard to the
// size the PNG states, so a small file can otherwise ask for gigabytes.
#define STBI_MALLOC(size) wallpaperx::we::stbAllocate(size)
#define STBI_REALLOC(block, size) wallpaperx::we::stbReallocate(block, size)
#define STBI_FREE(block) wallpaperx::we::stbRelease(block)
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include <stb_image.h>
#pragma GCC diagnostic pop

namespace wallpaperx::we {

namespace {

static_assert(kMaxTextureFileBytes <= INT_MAX && kMaxTextureLevelBytes <= INT_MAX,
              "LZ4 and stb_image take sizes as int");

// The bytes stb_image may still take on this thread, or null outside a decode. Each block carries
// its size in front, since stb frees without saying how much.
thread_local std::size_t* t_stbLeft = nullptr;
constexpr std::size_t kStbHeader = alignof(std::max_align_t);

std::size_t* stbBase(void* block)
{
    return reinterpret_cast<std::size_t*>(static_cast<unsigned char*>(block) - kStbHeader);
}

void* stbAllocate(std::size_t size)
{
    if (t_stbLeft == nullptr || size > *t_stbLeft) return nullptr;
    auto* base = static_cast<std::size_t*>(std::malloc(kStbHeader + size));
    if (base == nullptr) return nullptr;
    *base = size;
    *t_stbLeft -= size;
    return reinterpret_cast<unsigned char*>(base) + kStbHeader;
}

// stb passes a null block for the first IDAT chunk.
void* stbReallocate(void* block, std::size_t size)
{
    if (block == nullptr) return stbAllocate(size);
    const std::size_t old = *stbBase(block);
    if (t_stbLeft == nullptr || (size > old && size - old > *t_stbLeft)) return nullptr;
    auto* base = static_cast<std::size_t*>(std::realloc(stbBase(block), kStbHeader + size));
    if (base == nullptr) return nullptr;
    *base = size;
    *t_stbLeft = *t_stbLeft + old - size;
    return reinterpret_cast<unsigned char*>(base) + kStbHeader;
}

void stbRelease(void* block)
{
    if (block == nullptr) return;
    if (t_stbLeft != nullptr) *t_stbLeft += *stbBase(block);
    std::free(stbBase(block));
}

std::unexpected<std::string> refuse(std::string_view what)
{
    return std::unexpected(std::format("The texture {}", what));
}

// What one picture's decode may hold at once: twice the compressed data, which stb copies and grows by
// doubling, and a share per pixel padded to a 32-pixel block. 16 per pixel is stb's worst for any JPEG
// and any PNG but an interlaced 16-bit one, which peaks near 28; 32 leaves room for data unlike that measured.
constexpr std::uint64_t kStbPixelBytes = 16;
constexpr std::uint64_t kStbInterlaced16PixelBytes = 32;
constexpr std::uint64_t kMaxStbBudget = 2ull << 30;

// The budget, or the refusal of a picture over kMaxStbBudget: parse() and decode() both ask, so a level
// parse() accepts is one decode() will try.
std::expected<std::uint64_t, std::string> stbBudget(std::uint64_t pictureBytes, std::uint64_t pixelBytes,
                                                     std::uint32_t width, std::uint64_t height, bool png)
{
    const std::uint64_t needs = 2 * pictureBytes + pixelBytes * (width + 32ull) * (height + 32) + (1u << 20);
    if (needs > kMaxStbBudget) {
        return refuse(std::format("holds a {}x{} {} picture that would take more than {} GiB to decode.", width, height,
                                  png ? "PNG" : "JPEG", kMaxStbBudget >> 30));
    }
    return needs;
}

// Read from the IHDR chunk, which a PNG must open with, so a picture that lies here only picks the
// larger budget for itself.
std::uint64_t stbPixelBytes(std::string_view picture, bool png)
{
    constexpr std::size_t kBitDepth = 24;
    constexpr std::size_t kInterlace = 28;
    if (!png || picture.size() <= kInterlace) return kStbPixelBytes;
    return picture[kBitDepth] == 16 && picture[kInterlace] == 1 ? kStbInterlaced16PixelBytes : kStbPixelBytes;
}

class StbBudgetScope {
public:
    explicit StbBudgetScope(std::size_t bytes) : m_left(bytes) { t_stbLeft = &m_left; }
    ~StbBudgetScope() { t_stbLeft = nullptr; }
    StbBudgetScope(const StbBudgetScope&) = delete;
    StbBudgetScope& operator=(const StbBudgetScope&) = delete;

private:
    std::size_t m_left;
};

// No LZ4 block byte yields more output than a match-length extension byte's 255.
constexpr std::uint64_t kLz4MaxExpansion = 255;
constexpr std::uint32_t kFlagAnimated = 0x4;
constexpr std::int32_t kEmbeddedNone = -1;
constexpr std::int32_t kEmbeddedJpeg = 2;
constexpr std::int32_t kEmbeddedPng = 13;
constexpr std::string_view kPngSignature = "\x89PNG\r\n\x1a\n";
constexpr std::string_view kJpegSignature = "\xff\xd8\xff";

// "XXXXnnnn" and a zero byte; the version number, or nothing when the tag does not match.
std::optional<unsigned> versionTag(ByteCursor& in, std::string_view tag)
{
    const auto raw = in.take(9);
    if (!raw || !raw->starts_with(tag) || raw->back() != '\0') return std::nullopt;
    unsigned version = 0;
    for (const char c : raw->substr(4, 4)) {
        if (c < '0' || c > '9') return std::nullopt;
        version = version * 10 + unsigned(c - '0');
    }
    return version;
}

struct Raw {
    TextureFormat format;
    // Per block of 4x4 pixels for a block format, else per pixel.
    std::uint32_t bytes;
    bool blocks;
};

std::optional<Raw> rawFormat(std::uint32_t value)
{
    switch (value) {
    case 0: return Raw{TextureFormat::Rgba8, 4, false};
    case 4: return Raw{TextureFormat::Bc3, 16, true};
    case 6: return Raw{TextureFormat::Bc2, 16, true};
    case 7: return Raw{TextureFormat::Bc1, 8, true};
    case 8: return Raw{TextureFormat::Rg8, 2, false};
    case 9: return Raw{TextureFormat::R8, 1, false};
    default: return std::nullopt;
    }
}

std::uint64_t rawBytes(const Raw& raw, std::uint32_t w, std::uint32_t h, std::uint32_t d)
{
    if (raw.blocks) return std::uint64_t((w + 3) / 4) * ((h + 3) / 4) * d * raw.bytes;
    return std::uint64_t(w) * h * d * raw.bytes;
}

// Expands LZ4 block data into exactly `out`'s size.
bool expandInto(std::string_view stored, char* out, std::size_t size)
{
    return LZ4_decompress_safe(stored.data(), out, int(stored.size()), int(size)) == int(size);
}

bool inRange(std::uint32_t v)
{
    return v >= 1 && v <= kMaxTextureDimension;
}

// The frame's four corners, each within its image's level 0 widened by the level's own size on every
// side (texture.h). In double, so the sums cannot overflow.
bool frameNear(const TextureFrame& frame, const TextureLevel& base)
{
    const double xs[] = {frame.x, frame.x + double(frame.ux), frame.x + double(frame.vx),
                         frame.x + double(frame.ux) + frame.vx};
    const double ys[] = {frame.y, frame.y + double(frame.uy), frame.y + double(frame.vy),
                         frame.y + double(frame.uy) + frame.vy};
    const double w = base.width;
    const double h = base.height;
    return std::ranges::all_of(xs, [w](double x) { return x >= -w && x <= 2 * w; }) &&
           std::ranges::all_of(ys, [h](double y) { return y >= -h && y <= 2 * h; });
}

std::expected<TextureFrames, std::string> readFrames(ByteCursor& in, const Texture& texture)
{
    const auto version = versionTag(in, "TEXS");
    if (!version || *version < 1 || *version > 3) return refuse("is animated but has no frame table it can read.");
    const auto count = in.u32();
    if (!count) return refuse("ends inside its frame table.");
    TextureFrames frames;
    if (*version == 3) {
        const auto w = in.u32();
        const auto h = in.u32();
        if (!h) return refuse("ends inside its frame table.");
        if (!inRange(*w) || !inRange(*h)) return refuse(std::format("has frames of {}x{} pixels.", *w, *h));
        frames.frameSize = TextureFrames::Size{*w, *h};
    } else if (*version == 1) {
        // The oldest table: every frame is the header's size (spec_tex.md, "Sizes").
        frames.frameSize = TextureFrames::Size{texture.width, texture.height};
    }
    constexpr std::size_t kFrameBytes = 32;
    if (*count == 0 || *count > kMaxTextureFrames || in.remaining() != std::size_t(*count) * kFrameBytes) {
        return refuse(std::format("has a frame table of {} frames that does not fit the file.", *count));
    }
    frames.frames.reserve(*count);
    const bool whole = *version == 1;
    for (std::uint32_t i = 0; i < *count; ++i) {
        // Each read is in range: the remaining bytes were checked above.
        TextureFrame frame;
        frame.image = *in.u32();
        frame.seconds = *in.f32();
        float* const coordinates[] = {&frame.x, &frame.y, &frame.ux, &frame.uy, &frame.vx, &frame.vy};
        for (float* c : coordinates) *c = whole ? float(*in.i32()) : *in.f32();
        if (frame.image >= texture.images.size()) return refuse("has a frame in an image it does not hold.");
        if (!std::isfinite(frame.seconds) ||
            !std::ranges::all_of(coordinates, [](const float* c) { return std::isfinite(*c); })) {
            return refuse("has a frame whose time or place is not a usable number.");
        }
        const bool timed = (i == 0 ? frame : frames.frames.front()).seconds != 0;
        if (timed ? frame.seconds < kMinTextureFrameSeconds : frame.seconds != 0) {
            return refuse("has frames whose times are neither all zero nor all a usable length.");
        }
        const TextureLevel& base = texture.images[frame.image].levels[0];
        if (!frameNear(frame, base)) return refuse("has a frame that lies far outside its image.");
        if (frames.frameSize && (frames.frameSize->width > base.width || frames.frameSize->height > base.height)) {
            return refuse("has frames larger than an image that holds one.");
        }
        frames.frames.push_back(frame);
    }
    return frames;
}

} // namespace

std::expected<TextureFile, std::string> TextureFile::parse(std::string bytes)
{
    if (bytes.size() > kMaxTextureFileBytes) {
        return refuse(std::format("is {} MiB, over the {} MiB allowed.", bytes.size() >> 20, kMaxTextureFileBytes >> 20));
    }
    TextureFile file;
    Texture& texture = file.m_texture;
    ByteCursor in(bytes);
    if (versionTag(in, "TEXV") != 5u) return refuse("does not start with texture version TEXV0005.");
    if (versionTag(in, "TEXI") != 1u) return refuse("has a header other than TEXI0001.");

    // A run of reads is checked by its last (ByteCursor).
    const auto format = in.u32();
    const auto flags = in.u32();
    if (!flags) return refuse("ends inside its header.");
    texture.flags = *flags;
    const auto width = in.u32();
    const auto height = in.u32();
    // A volume has one more header word here, whose meaning is unknown.
    if (texture.volume() && !in.u32()) return refuse("ends inside its header.");
    const auto pictureWidth = in.u32();
    const auto pictureHeight = in.u32();
    const auto colour = in.u32();
    if (!colour) return refuse("ends inside its header.");
    texture.width = *width;
    texture.height = *height;
    texture.pictureWidth = *pictureWidth;
    texture.pictureHeight = *pictureHeight;

    const auto container = versionTag(in, "TEXB");
    if (!container || *container < 1 || *container > 4) return refuse("has no image block it can read.");
    const auto imageCount = in.u32();
    if (!imageCount) return refuse("ends inside its image block.");
    if (*imageCount == 0 || *imageCount > kMaxTextureImages) {
        return refuse(std::format("claims {} images.", *imageCount));
    }
    if (*container >= 3) {
        const auto embedded = in.i32();
        if (!embedded) return refuse("ends inside its image block.");
        file.m_embedded = *embedded;
        if (*container == 4 && !in.u32()) return refuse("ends inside its image block.");
    }
    const auto raw = rawFormat(*format);
    if (file.m_embedded == kEmbeddedNone) {
        if (!raw) return refuse(std::format("is in pixel format {}, which is not supported.", *format));
        texture.format = raw->format;
    } else if (file.m_embedded == kEmbeddedPng || file.m_embedded == kEmbeddedJpeg) {
        texture.format = TextureFormat::Rgba8;
    } else {
        return refuse(std::format("embeds pictures of kind {}, which is not supported.", file.m_embedded));
    }

    std::uint64_t decodedBytes = 0;
    texture.images.reserve(*imageCount);
    file.m_stored.reserve(*imageCount);
    for (std::uint32_t image = 0; image < *imageCount; ++image) {
        const auto levelCount = in.u32();
        if (!levelCount) return refuse("ends inside its image block.");
        if (*levelCount == 0 || *levelCount > kMaxTextureLevels) {
            return refuse(std::format("claims {} levels in one image.", *levelCount));
        }
        auto& levels = texture.images.emplace_back().levels;
        auto& stored = file.m_stored.emplace_back();
        levels.reserve(*levelCount);
        stored.reserve(*levelCount);
        for (std::uint32_t i = 0; i < *levelCount; ++i) {
            const auto w = in.u32();
            const auto h = in.u32();
            const auto d = texture.volume() ? in.u32() : std::optional<std::uint32_t>(1);
            const auto lz4 = *container >= 2 ? in.u32() : std::optional<std::uint32_t>(0);
            const auto expandedSize = *container >= 2 ? in.u32() : std::optional<std::uint32_t>(0);
            const auto storedSize = in.u32();
            if (!storedSize) return refuse("ends inside its image block.");
            if (!inRange(*w) || !inRange(*h) || !inRange(*d)) {
                return refuse(texture.volume() ? std::format("has a level of {}x{}x{} pixels.", *w, *h, *d)
                                               : std::format("has a level of {}x{} pixels.", *w, *h));
            }
            if (i > 0) {
                const TextureLevel& above = levels.back();
                const auto half = [](std::uint32_t v) { return std::max<std::uint32_t>(v / 2, 1); };
                if (*w != half(above.width) || *h != half(above.height) || *d != half(above.depth)) {
                    return refuse("has a level that is not half the size of the one before.");
                }
            }
            if (*lz4 > 1) return refuse("has a level whose compression is neither none nor LZ4.");
            // decode() allocates the claimed size before LZ4 can say it is false.
            if (*lz4 && std::uint64_t(*expandedSize) > kLz4MaxExpansion * *storedSize) {
                return refuse("has a compressed level that claims more than its data could expand to.");
            }
            const std::size_t offset = in.position();
            const auto data = in.take(*storedSize);
            if (!data) return refuse("ends inside the data of a level.");

            TextureLevel level{*w, *h, *d, 0};
            if (file.m_embedded == kEmbeddedNone) {
                level.decodedBytes = rawBytes(*raw, *w, *h, *d);
                const std::uint64_t holds = *lz4 ? *expandedSize : *storedSize;
                if (holds != level.decodedBytes) {
                    return refuse(std::format("has a {}x{} level of {} bytes, where its format needs {}.", *w, *h,
                                              holds, level.decodedBytes));
                }
            } else {
                level.decodedBytes = std::uint64_t(*w) * *h * *d * 4;
                if (*lz4 && (*expandedSize == 0 || *expandedSize > kMaxTextureFileBytes)) {
                    return refuse("has a compressed picture of an impossible size.");
                }
                // An LZ4 picture's header is not read until decode(), which checks again with its own figure.
                const bool png = file.m_embedded == kEmbeddedPng;
                const auto budget = stbBudget(*lz4 ? *expandedSize : *storedSize,
                                              *lz4 ? kStbPixelBytes : stbPixelBytes(*data, png), *w,
                                              std::uint64_t(*h) * *d, png);
                if (!budget) return std::unexpected(budget.error());
            }
            if (level.decodedBytes > kMaxTextureLevelBytes) {
                return refuse(std::format("has a level over the {} MiB allowed.", kMaxTextureLevelBytes >> 20));
            }
            decodedBytes += level.decodedBytes;
            if (decodedBytes > kMaxTextureDecodedBytes) {
                return refuse(std::format("would take more than {} MiB once decoded.", kMaxTextureDecodedBytes >> 20));
            }
            levels.push_back(level);
            stored.push_back({offset, *storedSize, *lz4 == 1, *expandedSize});
        }
    }

    if (!inRange(texture.width) || !inRange(texture.height) || !inRange(texture.pictureWidth) ||
        !inRange(texture.pictureHeight) || texture.pictureWidth > texture.width ||
        texture.pictureHeight > texture.height) {
        return refuse(std::format("has a header size of {}x{} with a picture of {}x{}.", texture.width, texture.height,
                                  texture.pictureWidth, texture.pictureHeight));
    }
    // An animated texture's level 0 is an atlas, whose size need not follow the header's.
    if (!(texture.flags & kFlagAnimated) &&
        !std::ranges::all_of(texture.images, [&texture](const TextureImage& image) {
            return texture.pictureWidth <= image.levels[0].width && texture.pictureHeight <= image.levels[0].height;
        })) {
        return refuse("has a picture larger than its largest level.");
    }

    if (texture.flags & kFlagAnimated) {
        auto frames = readFrames(in, texture);
        if (!frames) return std::unexpected(std::move(frames.error()));
        texture.frames = std::move(*frames);
    } else if (!in.atEnd()) {
        return refuse("has bytes after its images that it does not describe.");
    }
    file.m_bytes = std::move(bytes);
    return file;
}

std::expected<std::vector<std::uint8_t>, std::string> TextureFile::decode(std::size_t image, std::size_t level) const
{
    if (image >= m_stored.size() || level >= m_stored[image].size()) return refuse("has no such level.");
    const Stored& stored = m_stored[image][level];
    const TextureLevel& size = m_texture.images[image].levels[level];
    const std::string_view data = std::string_view(m_bytes).substr(stored.offset, stored.size);

    if (m_embedded == kEmbeddedNone) {
        if (!stored.lz4) return std::vector<std::uint8_t>(data.begin(), data.end());
        std::vector<std::uint8_t> out(size.decodedBytes);
        if (!expandInto(data, reinterpret_cast<char*>(out.data()), out.size())) {
            return refuse("holds compressed data that does not expand to its stated size.");
        }
        return out;
    }

    std::string expanded;
    std::string_view picture = data;
    if (stored.lz4) {
        expanded.resize(stored.expandedSize);
        if (!expandInto(data, expanded.data(), expanded.size())) {
            return refuse("holds compressed data that does not expand to its stated size.");
        }
        picture = expanded;
    }
    const bool png = m_embedded == kEmbeddedPng;
    const char* kind = png ? "PNG" : "JPEG";
    if (!picture.starts_with(png ? kPngSignature : kJpegSignature)) {
        return refuse(std::format("holds a level that is not the {} picture it should be.", kind));
    }
    // A volume's slices are stacked top to bottom in one picture.
    const std::uint64_t wantHeight = std::uint64_t(size.height) * size.depth;
    const auto* bytes = reinterpret_cast<const stbi_uc*>(picture.data());
    int w = 0;
    int h = 0;
    int channels = 0;
    const auto needs = stbBudget(picture.size(), stbPixelBytes(picture, png), size.width, wantHeight, png);
    if (!needs) return std::unexpected(needs.error());
    const StbBudgetScope budget(*needs);
    // stb_image's JPEG header read checks no size, so the stated one is compared before it allocates.
    if (!stbi_info_from_memory(bytes, int(picture.size()), &w, &h, &channels)) {
        return refuse(std::format("holds a {} picture that cannot be read.", kind));
    }
    if (std::uint64_t(w) != size.width || std::uint64_t(h) != wantHeight) {
        return refuse(std::format("holds a {}x{} picture where its level says {}x{}.", w, h, size.width, wantHeight));
    }
    const std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
        stbi_load_from_memory(bytes, int(picture.size()), &w, &h, &channels, STBI_rgb_alpha), &stbi_image_free);
    // A budget overrun is stb's "outofmem".
    if (!pixels) {
        const char* why = stbi_failure_reason();
        return refuse(std::format("holds a {} picture that does not decode ({}).", kind, why ? why : "no reason given"));
    }
    if (std::uint64_t(w) != size.width || std::uint64_t(h) != wantHeight) {
        return refuse("holds a picture whose header and data disagree about its size.");
    }
    return std::vector<std::uint8_t>(pixels.get(), pixels.get() + size.decodedBytes);
}

} // namespace wallpaperx::we
