// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// A Wallpaper Engine texture, a .tex file (docs/phase4/spec_tex.md). The bytes are untrusted: the
// whole layout is checked, and every size bounded, when the file is parsed; pixels are decoded one
// level at a time, so the memory a decode takes is one level's, not the whole texture's.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace wallpaperx::we {

// Far above any real texture (the bases are in PLAN.md).
inline constexpr std::size_t kMaxTextureFileBytes = 512u << 20;
inline constexpr std::uint32_t kMaxTextureDimension = 16384;
inline constexpr std::uint32_t kMaxTextureImages = 64;
// A chain halving from kMaxTextureDimension to 1.
inline constexpr std::uint32_t kMaxTextureLevels = 15;
inline constexpr std::uint32_t kMaxTextureFrames = 65536;
// The shortest frame of a timed frame table.
inline constexpr float kMinTextureFrameSeconds = 0.001f;
// One decoded level: kMaxTextureDimension squared in RGBA8. LZ4 and stb_image take sizes as int. An
// embedded picture's level is held lower by decode()'s 2 GiB cap: about 134 million pixels (11550
// squared), half that for an interlaced 16-bit PNG, both shrinking as the picture's file grows.
inline constexpr std::uint64_t kMaxTextureLevelBytes = 1ull << 30;
// All levels of all images together, decoded.
inline constexpr std::uint64_t kMaxTextureDecodedBytes = 2ull << 30;

// The layout of a decoded level's bytes. Embedded PNG and JPEG pictures decode to Rgba8; the block
// formats stay compressed, since the GPU samples them as they are.
enum class TextureFormat {
    Rgba8, // R, G, B, A
    Rg8,
    R8,
    Bc1, // 8 bytes per 4x4 block
    Bc2, // 16 bytes per 4x4 block, explicit 4-bit alpha
    Bc3, // 16 bytes per 4x4 block, interpolated alpha
};

struct TextureLevel {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // 1 unless the texture is a volume; a volume's slices follow each other in the decoded bytes.
    std::uint32_t depth = 1;
    // What decode() returns for it.
    std::uint64_t decodedBytes = 0;
};

struct TextureImage {
    // Level 0 first; each level is the one before halved, rounded down, at least 1.
    std::vector<TextureLevel> levels;
};

// One frame of an animated texture: the frame pixel at (u, v), both 0..1 from the top left, is at
// texel (x + u*ux + v*vx, y + u*uy + v*vy) of level 0 of images[image]. The vectors may be rotated.
// A frame may reach past that level's edges, even lie wholly outside it (built-in particle sheets
// list cells past the image's edge), so a reader clips; each corner is within the level widened by
// its own width and height on every side.
struct TextureFrame {
    std::uint32_t image = 0;
    float seconds = 0;
    float x = 0;
    float y = 0;
    float ux = 0;
    float uy = 0;
    float vx = 0;
    float vy = 0;
};

struct TextureFrames {
    struct Size {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };
    // Where the file states it; not every frame table does. Within level 0 of every image a frame is in.
    std::optional<Size> frameSize;
    // Never empty. Either every frame lasts at least kMinTextureFrameSeconds, or every one lasts 0: the
    // table holds no times and whoever plays it times the frames itself (built-in particle sheets).
    std::vector<TextureFrame> frames;

    bool timed() const { return frames.front().seconds != 0; }
};

struct Texture {
    TextureFormat format = TextureFormat::Rgba8;
    // The texture's size, and the picture's within it, each 1..kMaxTextureDimension: a padded still
    // occupies the top-left pictureWidth x pictureHeight of level 0 of every image. For an animated
    // texture, see spec_tex.md "Sizes".
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t pictureWidth = 0;
    std::uint32_t pictureHeight = 0;
    // As stored; the bits below are named, the rest are kept for later readers.
    std::uint32_t flags = 0;
    // Never empty.
    std::vector<TextureImage> images;
    // Present exactly when the texture is animated.
    std::optional<TextureFrames> frames;

    bool nearestFilter() const { return flags & 0x1; }
    bool clampToEdge() const { return flags & 0x2; }
    bool volume() const { return flags & 0x40; }
};

// A parsed .tex file: its description, and its levels to decode on demand. Errors are sentences that
// do not name the file; the caller adds it.
class TextureFile {
public:
    static std::expected<TextureFile, std::string> parse(std::string bytes);

    const Texture& texture() const { return m_texture; }

    // One level's pixels in texture().format, texture().images[image].levels[level].decodedBytes long.
    // Fails for data that does not decode: LZ4 that does not expand exactly, a picture that is not what
    // its level says, or one whose decoding would hold more than twice the picture's bytes plus about
    // 16 bytes per pixel of the level (32 for an interlaced 16-bit PNG). A picture for which that comes
    // to over 2 GiB is refused without decoding, so the decoder never holds more; parse() already refuses
    // every such level but an LZ4-compressed interlaced 16-bit PNG, whose header it does not expand to
    // read, so only that one can parse and then be refused here. Besides that it holds the
    // result and, for an LZ4 level, the expanded bytes: never more than 255 times the level's stored
    // bytes, which parse() checks. Throws std::bad_alloc only when memory runs out within those bounds.
    // Safe to call from several threads at once.
    std::expected<std::vector<std::uint8_t>, std::string> decode(std::size_t image, std::size_t level) const;

private:
    TextureFile() = default;

    struct Stored {
        std::size_t offset = 0;
        std::uint32_t size = 0;
        bool lz4 = false;
        std::uint32_t expandedSize = 0;
    };

    std::string m_bytes;
    Texture m_texture;
    // -1 for raw pixels, else the embedded picture kind.
    std::int32_t m_embedded = -1;
    // Shaped as m_texture.images; neither changes after parse().
    std::vector<std::vector<Stored>> m_stored;
};

} // namespace wallpaperx::we
