// SPDX-License-Identifier: GPL-2.0-or-later

// The Wallpaper Engine package reader and texture decoder (backend/daemon/we_compat/) on synthetic
// files built here, and on the real items under WALLPAPERX_TEST_STEAMAPPS when it names a folder.

#include "daemon/we_compat/package.h"
#include "daemon/we_compat/texture.h"
#include "tests/test_phase.h"
#include "tests/test_report.h"

#include <lz4.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include <stb_image_write.h>
#pragma GCC diagnostic pop

#include <fcntl.h>
#include <malloc.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using namespace wallpaperx::we;
using wallpaperx::testing::check;
using wallpaperx::testing::guardedPhase;
using wallpaperx::testing::harnessFailure;
using wallpaperx::testing::phaseSelected;

namespace {

void u32(std::string& out, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i) out += char((v >> (8 * i)) & 0xff);
}

void f32(std::string& out, float v)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, 4);
    u32(out, bits);
}

void tag(std::string& out, const char* text)
{
    out += text;
    out += '\0';
}

bool contains(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

// Empty when no folder could be made; main() reports that as a harness fault before any phase runs.
fs::path scratchDir()
{
    static const fs::path dir = [] {
        std::error_code ec;
        const fs::path tmp = fs::temp_directory_path(ec);
        if (ec) return fs::path();
        std::string pattern = (tmp / "we_compat.XXXXXX").string();
        if (::mkdtemp(pattern.data()) == nullptr) return fs::path();
        return fs::path(pattern);
    }();
    return dir;
}

// A fixture that was not written is a harness fault: the refusal checks would otherwise grade a
// missing file as a refused one.
// CONTROL: test side, the path pointed into a missing folder -> "FAIL (harness): could not write the
//       fixture ..." per write and the run red with the fault summary. Restored.
fs::path writeFile(const std::string& name, const std::string& bytes)
{
    const fs::path path = scratchDir() / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes;
    out.close();
    std::error_code ec;
    if (!out || fs::file_size(path, ec) != bytes.size() || ec) harnessFailure("could not write the fixture " + path.string());
    return path;
}

// Package::open takes a descriptor; O_NONBLOCK so a named pipe cannot park this open.
std::expected<Package, std::string> openPackage(const fs::path& path)
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
    if (fd < 0) return std::unexpected(std::string("could not be opened: ") + std::strerror(errno));
    return Package::open(wallpaperx::UniqueFd(fd));
}

// What this process has read through read/pread so far (/proc/self/io), or nothing if it cannot say.
std::optional<std::uint64_t> bytesReadSoFar()
{
    std::ifstream in("/proc/self/io");
    std::string key;
    std::uint64_t value = 0;
    while (in >> key >> value) {
        if (key == "rchar:") return value;
    }
    return std::nullopt;
}

struct PkgFile {
    std::string name;
    std::string bytes;
    // Overrides of what the table states; the data written is always `bytes`.
    std::optional<std::uint32_t> statedSize = {};
    std::optional<std::uint32_t> statedOffset = {};
};

std::string package(const std::vector<PkgFile>& files, const char* version = "PKGV0018",
                    std::optional<std::uint32_t> statedCount = {})
{
    std::string out;
    u32(out, std::uint32_t(std::strlen(version)));
    out += version;
    u32(out, statedCount.value_or(std::uint32_t(files.size())));
    std::uint32_t offset = 0;
    for (const PkgFile& f : files) {
        u32(out, std::uint32_t(f.name.size()));
        out += f.name;
        u32(out, f.statedOffset.value_or(offset));
        u32(out, f.statedSize.value_or(std::uint32_t(f.bytes.size())));
        offset += std::uint32_t(f.bytes.size());
    }
    for (const PkgFile& f : files) out += f.bytes;
    return out;
}

struct Level {
    std::uint32_t w, h, d = 1;
    std::string data;
    bool lz4 = false;
    // Overrides of what is written: the stated expanded size, and the stored bytes.
    std::optional<std::uint32_t> rawSize;
    std::optional<std::string> stored;
};

std::string compressed(const std::string& raw)
{
    std::string out(std::size_t(LZ4_compressBound(int(raw.size()))), '\0');
    out.resize(std::size_t(LZ4_compress_default(raw.data(), out.data(), int(raw.size()), int(out.size()))));
    return out;
}

struct TexSpec {
    std::uint32_t format = 0;
    std::uint32_t flags = 2;
    std::uint32_t texW = 0, texH = 0, imgW = 0, imgH = 0;
    unsigned container = 3;
    std::int32_t embedded = -1;
    std::vector<std::vector<Level>> images;
    std::string frameTable;
};

std::string texture(const TexSpec& t)
{
    std::string out;
    tag(out, "TEXV0005");
    tag(out, "TEXI0001");
    u32(out, t.format);
    u32(out, t.flags);
    u32(out, t.texW);
    u32(out, t.texH);
    if (t.flags & 0x40) u32(out, t.texH * t.texH);
    u32(out, t.imgW);
    u32(out, t.imgH);
    u32(out, 0xff000000u);
    tag(out, ("TEXB000" + std::to_string(t.container)).c_str());
    u32(out, std::uint32_t(t.images.size()));
    if (t.container >= 3) u32(out, std::uint32_t(t.embedded));
    if (t.container == 4) u32(out, 0);
    for (const auto& levels : t.images) {
        u32(out, std::uint32_t(levels.size()));
        for (const Level& l : levels) {
            u32(out, l.w);
            u32(out, l.h);
            if (t.flags & 0x40) u32(out, l.d);
            const std::string stored = l.stored ? *l.stored : l.lz4 ? compressed(l.data) : l.data;
            if (t.container >= 2) {
                u32(out, l.lz4 ? 1 : 0);
                u32(out, l.rawSize ? *l.rawSize : std::uint32_t(l.data.size()));
            }
            u32(out, std::uint32_t(stored.size()));
            out += stored;
        }
    }
    return out + t.frameTable;
}

std::string pattern(std::size_t n, unsigned seed)
{
    std::string out(n, '\0');
    for (std::size_t i = 0; i < n; ++i) out[i] = char((i * 7 + seed * 13 + i / 5) & 0xff);
    return out;
}

// A raw texture of `levels` levels from w x h, in `format` of `bytes` per pixel or per 4x4 block.
TexSpec rawSpec(std::uint32_t format, std::uint32_t bytes, bool blocks, std::uint32_t w, std::uint32_t h,
                std::uint32_t levels, bool lz4)
{
    TexSpec t{.format = format, .texW = w, .texH = h, .imgW = w, .imgH = h};
    std::vector<Level> chain;
    for (std::uint32_t i = 0; i < levels; ++i) {
        const std::size_t n = blocks ? std::size_t((w + 3) / 4) * ((h + 3) / 4) * bytes : std::size_t(w) * h * bytes;
        chain.push_back({w, h, 1, pattern(n, i), lz4});
        w = std::max(w / 2, 1u);
        h = std::max(h / 2, 1u);
    }
    t.images.push_back(std::move(chain));
    return t;
}

struct Decoded {
    Texture texture;
    // [image][level]
    std::vector<std::vector<std::vector<std::uint8_t>>> levels;
};

// Parses, then decodes every level, checking each is the size the description gives.
std::expected<Decoded, std::string> decodeAll(std::string bytes)
{
    auto file = TextureFile::parse(std::move(bytes));
    if (!file) return std::unexpected(file.error());
    Decoded out{file->texture(), {}};
    for (std::size_t i = 0; i < out.texture.images.size(); ++i) {
        auto& image = out.levels.emplace_back();
        for (std::size_t l = 0; l < out.texture.images[i].levels.size(); ++l) {
            auto px = file->decode(i, l);
            if (!px) return std::unexpected(px.error());
            check(px->size() == out.texture.images[i].levels[l].decodedBytes, "a level decodes to its stated size");
            image.push_back(std::move(*px));
        }
    }
    check(!file->decode(out.texture.images.size(), 0) && !file->decode(0, out.texture.images[0].levels.size()),
          "a level the texture does not hold is refused");
    return out;
}

// A refusal for the named reason: a refusal for any other reason fails, so an input that trips a
// second rule cannot stand in for the one under test.
void refusedFor(const std::expected<Decoded, std::string>& tex, const std::string& what, std::string_view reason)
{
    check(!tex.has_value() && contains(tex.error(), reason), what.c_str());
    if (tex) {
        std::fprintf(stderr, "  %s -> decoded\n", what.c_str());
    } else {
        std::printf("  %s -> %s\n", what.c_str(), tex.error().c_str());
    }
}

void parseRefusedFor(const TexSpec& t, const std::string& what, std::string_view reason)
{
    const auto file = TextureFile::parse(texture(t));
    check(!file.has_value() && contains(file.error(), reason), what.c_str());
    if (file) {
        std::fprintf(stderr, "  %s -> parsed\n", what.c_str());
    } else {
        std::printf("  %s -> %s\n", what.c_str(), file.error().c_str());
    }
}

void parses(const TexSpec& t, const std::string& what)
{
    const auto tex = decodeAll(texture(t));
    check(tex.has_value(), what.c_str());
    if (!tex) std::fprintf(stderr, "  %s -> %s\n", what.c_str(), tex.error().c_str());
}

void stbWrite(void* context, void* data, int size)
{
    static_cast<std::string*>(context)->append(static_cast<const char*>(data), std::size_t(size));
}

std::string rgbaPicture(int w, int h)
{
    std::string px(std::size_t(w) * h * 4, '\0');
    for (std::size_t i = 0; i < px.size(); ++i) px[i] = char((i * 31) & 0xff);
    return px;
}

std::string png(int w, int h)
{
    std::string out;
    const std::string px = rgbaPicture(w, h);
    stbi_write_png_to_func(stbWrite, &out, w, h, 4, px.data(), w * 4);
    return out;
}

std::string jpeg(int w, int h)
{
    std::string out;
    const std::string px = rgbaPicture(w, h);
    stbi_write_jpg_to_func(stbWrite, &out, w, h, 4, px.data(), 90);
    return out;
}

void packageReads()
{
    const std::string scene = R"({"objects":[]})";
    const std::string tex = pattern(300, 1);
    const fs::path path = writeFile("good.pkg", package({{"scene.json", scene},
                                                          {"materials/a b/\xe6\x98\x9f.tex", tex},
                                                          {"empty.txt", ""}}));
    auto pkg = openPackage(path);
    check(pkg.has_value(), "a well-formed package opens");
    if (!pkg) {
        std::fprintf(stderr, "  %s\n", pkg.error().c_str());
        return;
    }
    check(pkg->version() == "PKGV0018", "the package version is reported");
    check(pkg->entries().size() == 3, "every entry is listed");
    check(pkg->entries()[0].name == "empty.txt" && pkg->entries()[2].name == "scene.json",
          "entries are listed by name");
    check(pkg->read("scene.json", 1 << 20) == scene, "an entry reads back its bytes");
    check(pkg->read("materials/a b/\xe6\x98\x9f.tex", 1 << 20) == tex, "a non-ASCII name reads back");
    check(pkg->read("empty.txt", 0) == std::string(), "an empty entry reads as empty");
    check(!pkg->read("Scene.json", 1 << 20), "names are matched with case");
    check(!pkg->read("scene.json", scene.size() - 1), "an entry over the caller's limit is refused");
    check(pkg->find("missing") == nullptr, "a missing name is not found");
}

void packageRefused(const std::string& bytes, const std::string& what, std::string_view reason)
{
    const auto pkg = openPackage(writeFile("bad.pkg", bytes));
    check(!pkg.has_value() && contains(pkg.error(), reason), what.c_str());
    if (pkg) {
        std::fprintf(stderr, "  %s -> opened\n", what.c_str());
    } else {
        std::printf("  %s -> %s\n", what.c_str(), pkg.error().c_str());
    }
}

void packageRefusals()
{
    const std::string good = package({{"scene.json", "{}"}, {"materials/x.tex", pattern(40, 2)}});
    // Every prefix of a good package is refused, never read past.
    int truncatedOpened = 0;
    for (std::size_t n = 0; n < good.size(); ++n) {
        if (openPackage(writeFile("cut.pkg", good.substr(0, n)))) ++truncatedOpened;
    }
    check(truncatedOpened == 0, "every truncated package is refused");

    packageRefused(package({{"a", "x"}}, "PKGX0001"), "a wrong magic is refused", "does not start with a package version");

    // GOAL: a table claiming more entries than the file could hold is refused by the count rule itself,
    //       on each of its two halves (the file's size, and kMaxPackageEntries).
    // WHY-THIS-SHAPE: matched on the reason, since any count above the entries really written is also
    //       refused later as "ends inside its table"; the second case's file is large enough (65537 x 13
    //       bytes) that only the fixed cap can refuse its count.
    // CONTROL: backend/daemon/we_compat/package.cpp, each half of the count condition deleted in turn
    //       (count_size, count_cap in the T27a report): red on its own line. Restored.
    const std::string one = package({{"a", "x"}});
    packageRefused(package({{"a", "x"}}, "PKGV0018", std::uint32_t(one.size() / 13 + 1)),
                   "a count the file's size cannot hold is refused", "more than it can hold");
    packageRefused(package({{"a", std::string(std::size_t(kMaxPackageEntries + 1) * 13, '\0')}}, "PKGV0018",
                           kMaxPackageEntries + 1),
                   "a count over kMaxPackageEntries is refused", "more than it can hold");

    // CONTROL: backend/daemon/we_compat/package.cpp, the runs-past-its-end refusal deleted (past_end):
    //       both lines red. Restored.
    packageRefused(package({{"a", "xyz", 4}}), "an entry whose size runs past the end of the file is refused",
                   "runs past its end");
    packageRefused(package({{"a", "xyz", 3, 1}}), "an entry whose offset runs past the end of the file is refused",
                   "runs past its end");
    // CONTROL: backend/daemon/we_compat/package.cpp, the same-name refusal deleted (same_name): red. Restored.
    packageRefused(package({{"a", "x"}, {"a", "y"}}), "a repeated name is refused", "two files of the same name");
}

// GOAL: every entry name that could reach outside the item's folder is refused at open, each for the
//       path rule's reason; a name of 0 or over kMaxPackageNameBytes bytes for the length rule's.
// WHY-THIS-SHAPE: one name per case and the reason matched, so a name refused by a different rule
//       (the length rule, a short table) cannot pass for the path rule.
// CONTROL: backend/daemon/we_compat/package.cpp, the isPlainRelativePath call deleted (path_rule): every
//       path line red; shared/plain_path.h, the backslash test deleted (backslash): only the backslash
//       line red; package.cpp, `*nameLength == 0 ||` deleted (name_zero): the 0-byte line red. Restored.
void packageNames()
{
    const std::string path = "not a plain relative path";
    packageRefused(package({{"/etc/passwd", "x"}}), "an absolute name is refused", path);
    packageRefused(package({{"a/../b", "x"}}), "a '..' component is refused", path);
    packageRefused(package({{"a/./b", "x"}}), "a '.' component is refused", path);
    packageRefused(package({{"a//b", "x"}}), "an empty component is refused", path);
    packageRefused(package({{"a/", "x"}}), "a trailing slash is refused", path);
    packageRefused(package({{std::string("a\0b", 3), "x"}}), "a name with a zero byte is refused", path);
    packageRefused(package({{"materials\\a.tex", "x"}}), "a name with a backslash is refused", path);
    packageRefused(package({{"", "x"}}), "a name of 0 bytes is refused", "file name of 0 bytes");
    packageRefused(package({{std::string(kMaxPackageNameBytes + 1, 'n'), "x"}}),
                   "a name over kMaxPackageNameBytes is refused", "file name of 4097 bytes");
    const std::string longest(kMaxPackageNameBytes, 'n');
    const auto pkg = openPackage(writeFile("longname.pkg", package({{longest, "x"}})));
    check(pkg && pkg->read(longest, 1) == "x", "a name of exactly kMaxPackageNameBytes opens and reads");
}

// GOAL: open refuses anything but a regular file, from its fstat, for that reason.
// WHY-THIS-SHAPE: the reason is matched so an open that failed for another cause (a missing fifo, a
//       read error on the directory) cannot pass; the fifo's creation is checked as setup, since an
//       absent fifo is refused too. Opened O_NONBLOCK by openPackage, so a regression that reads a pipe
//       finds no writer and returns rather than hanging the check.
// CONTROL: backend/daemon/we_compat/package.cpp, the S_ISREG refusal deleted (not_regular): the pipe,
//       directory and device lines red. Restored. Test side: the fifo path made a regular file before
//       mkfifo -> "FAIL (harness): could not make the named pipe". Restored.
void packageNotRegular()
{
    const auto refusedFile = [](const fs::path& path, const char* what) {
        const auto pkg = openPackage(path);
        check(!pkg && contains(pkg.error(), "is not a regular file"), what);
        if (!pkg) std::printf("  %s -> %s\n", what, pkg.error().c_str());
    };
    refusedFile(scratchDir(), "a directory is refused");
    refusedFile("/dev/null", "a character device is refused");
    const fs::path fifo = scratchDir() / "fifo.pkg";
    if (::mkfifo(fifo.c_str(), 0600) != 0) {
        harnessFailure(std::string("could not make the named pipe: ") + std::strerror(errno));
        return;
    }
    refusedFile(fifo, "a named pipe is refused");
}

void packageShrinks()
{
    // GOAL: a read of an entry the file no longer holds fails; it never returns short bytes.
    // CONTROL: backend/daemon/we_compat/package.cpp readAt, the end-of-file refusal turned into
    //       `out.resize(done); return out;` (short_read): red. Restored.
    const fs::path path = writeFile("shrinks.pkg", package({{"a", pattern(1000, 3)}}));
    auto pkg = openPackage(path);
    check(pkg.has_value(), "the package to be cut opens");
    if (pkg) {
        fs::resize_file(path, fs::file_size(path) - 10);
        const auto bytes = pkg->read("a", 1 << 20);
        check(!bytes && contains(bytes.error(), "ended early"), "an entry the file no longer holds is refused");
    }
}

// GOAL: open reads the table and not the entries' data; a read over its limit reads nothing at all
//       (readAt, which allocates the entry's size, is never reached).
// WHY-THIS-SHAPE: measured as bytes this process read (/proc/self/io rchar) around each call, on a
//       sparse 64 MiB entry, so reading the data costs no disk yet moves the count by 64 MiB. An
//       allocation counter would not see the read; checking only the result would see neither.
// CONTROL: backend/daemon/we_compat/package.cpp, open reading the whole file (open_reads_all) -> the
//       open line red; read doing readAt before the limit test (read_then_limit) -> the over-limit
//       line red. Restored.
void packageCost()
{
    constexpr std::uint32_t kBig = 64u << 20;
    const std::string table = package({{"scene.json", "{}"}, {"big", "", kBig}});
    const fs::path path = writeFile("sparse.pkg", table);
    fs::resize_file(path, table.size() + kBig);
    const auto before = bytesReadSoFar();
    auto pkg = openPackage(path);
    const auto opened = bytesReadSoFar();
    if (!before || !opened) {
        wallpaperx::testing::notVerified("the package read-cost checks", "/proc/self/io has no rchar line here");
        return;
    }
    check(pkg.has_value(), "the sparse package opens");
    if (!pkg) return;
    check(*opened - *before <= kMaxPackageTableBytes, "opening a package reads only its table");
    std::printf("  open read %llu bytes of a %zu-byte file\n", static_cast<unsigned long long>(*opened - *before),
                table.size() + kBig);
    const auto read = pkg->read("big", 1 << 20);
    const auto after = bytesReadSoFar();
    check(!read && after && *after - *opened < 4096, "a read over its limit is refused without reading");
    check(pkg->read("scene.json", 16) == "{}", "an entry of the sparse package reads back");
}

// GOAL: a table larger than the first 64 KiB read opens, and every name and entry reads back exactly:
//       the second read takes only the rest of the table, joined with no gap and no overlap.
// CONTROL: backend/daemon/we_compat/package.cpp, the second read taken from offset 0 (second_read_at_0):
//       "a package whose table is over 64 KiB opens" red (the joined table no longer parses). Restored.
// COVERAGE-ASSERT: the table's size is measured from the builder and must exceed 64 KiB, or the second
//       read is never made and this check would grade the first read twice.
void packageLargeTable()
{
    std::vector<PkgFile> files;
    std::size_t data = 0;
    for (int i = 0; i < 40; ++i) {
        files.push_back({"d" + std::to_string(i) + "/" + std::string(2000, char('a' + i % 26)), pattern(50 + i, i)});
        data += files.back().bytes.size();
    }
    const std::string bytes = package(files);
    check(bytes.size() - data > (64u << 10), "the large table is larger than the first read");
    const auto pkg = openPackage(writeFile("large.pkg", bytes));
    check(pkg.has_value(), "a package whose table is over 64 KiB opens");
    if (!pkg) {
        std::fprintf(stderr, "  %s\n", pkg.error().c_str());
        return;
    }
    int same = 0;
    for (const PkgFile& f : files) {
        if (pkg->read(f.name, 1 << 20) == f.bytes) ++same;
    }
    check(pkg->entries().size() == files.size() && same == int(files.size()),
          "every entry of a table over 64 KiB reads back its bytes");
}

void textureFormats()
{
    struct Case {
        std::uint32_t value, bytes;
        bool blocks;
        TextureFormat format;
    };
    const Case cases[] = {{0, 4, false, TextureFormat::Rgba8}, {4, 16, true, TextureFormat::Bc3},
                          {6, 16, true, TextureFormat::Bc2},   {7, 8, true, TextureFormat::Bc1},
                          {8, 2, false, TextureFormat::Rg8},   {9, 1, false, TextureFormat::R8}};
    // GOAL: every level decodes to exactly decodedBytes, and raw and LZ4 levels to the stored pixels
    //       byte for byte.
    // CONTROL: backend/daemon/we_compat/texture.cpp decode, a raw level returned one byte short
    //       (raw_short) -> "a level decodes to its stated size" red; its first byte changed (raw_byte)
    //       -> "keeps every level's size and bytes" red. Restored.
    // COVERAGE-ASSERT: the levels compared are counted: 6 formats x 7 container/LZ4 kinds x 6 levels.
    int compared = 0;
    for (const Case& c : cases) {
        for (const unsigned container : {1u, 2u, 3u, 4u}) {
            for (const bool lz4 : {false, true}) {
                if (container == 1 && lz4) continue;
                TexSpec spec = rawSpec(c.value, c.bytes, c.blocks, 37, 10, 6, lz4);
                spec.container = container;
                const auto tex = decodeAll(texture(spec));
                const std::string what = "format " + std::to_string(c.value) + " TEXB000" + std::to_string(container) +
                                         (lz4 ? " LZ4" : "");
                check(tex.has_value(), (what + " decodes").c_str());
                if (!tex) {
                    std::fprintf(stderr, "  %s\n", tex.error().c_str());
                    continue;
                }
                check(tex->texture.format == c.format, (what + " reports its format").c_str());
                bool same = tex->levels.size() == 1 && tex->levels[0].size() == 6;
                for (std::size_t i = 0; same && i < 6; ++i) {
                    const auto& level = tex->texture.images[0].levels[i];
                    const auto& px = tex->levels[0][i];
                    same = level.width == spec.images[0][i].w && level.height == spec.images[0][i].h &&
                           std::string(px.begin(), px.end()) == spec.images[0][i].data;
                    if (same) ++compared;
                }
                check(same, (what + " keeps every level's size and bytes").c_str());
            }
        }
    }
    check(compared == 6 * 7 * 6, "every format, container and level was compared");
    TexSpec padded = rawSpec(0, 4, false, 64, 32, 1, false);
    padded.imgW = 48;
    padded.imgH = 20;
    padded.flags = 3;
    const auto tex = decodeAll(texture(padded));
    check(tex && tex->texture.width == 64 && tex->texture.pictureWidth == 48 && tex->texture.pictureHeight == 20 && tex->texture.nearestFilter() &&
              tex->texture.clampToEdge() && !tex->texture.frames,
          "a padded still reports texture and picture sizes and its sampling flags");
}

void textureEmbedded()
{
    for (const bool isPng : {true, false}) {
        TexSpec spec{.texW = 64, .texH = 32, .imgW = 40, .imgH = 20, .embedded = isPng ? 13 : 2};
        spec.images.push_back({{40, 20, 1, isPng ? png(40, 20) : jpeg(40, 20)},
                               {20, 10, 1, isPng ? png(20, 10) : jpeg(20, 10)}});
        const std::string kind = isPng ? "PNG" : "JPEG";
        const auto tex = decodeAll(texture(spec));
        check(tex.has_value(), ("an embedded " + kind + " decodes").c_str());
        if (!tex) {
            std::fprintf(stderr, "  %s\n", tex.error().c_str());
            continue;
        }
        const auto& levels = tex->levels[0];
        check(tex->texture.format == TextureFormat::Rgba8 && levels.size() == 2 && levels[0].size() == 40 * 20 * 4 &&
                  levels[1].size() == 20 * 10 * 4,
              ("an embedded " + kind + " decodes every level to RGBA8").c_str());
        if (isPng) {
            const std::string want = rgbaPicture(40, 20);
            check(std::string(levels[0].begin(), levels[0].end()) == want,
                  "an embedded PNG decodes to its pixels");
        }
        // GOAL: a picture of another size than its level is refused by the comparison made before
        //       stbi_load allocates for its pixels.
        // WHY-THIS-SHAPE: matched on that comparison's reason; without it stb decodes the picture and
        //       the later header-and-data comparison refuses it, which a bare "refused" cannot tell apart.
        // CONTROL: backend/daemon/we_compat/texture.cpp decode, the comparison before stbi_load deleted
        //       (embedded_size): both kinds' lines red. Restored.
        TexSpec wrong = spec;
        wrong.images[0][0].w = 41;
        refusedFor(decodeAll(texture(wrong)), "an embedded " + kind + " of the wrong size is refused",
                   "picture where its level says");
        TexSpec other = spec;
        other.embedded = isPng ? 2 : 13;
        check(!decodeAll(texture(other)), ("an embedded " + kind + " under the other kind is refused").c_str());
    }
    // A volume: a 4x4x4 cube stored as one 4x16 PNG.
    TexSpec volume{.flags = 0x42, .texW = 4, .texH = 4, .imgW = 4, .imgH = 4, .container = 4, .embedded = 13};
    volume.images.push_back({{4, 4, 4, png(4, 16)}});
    const auto tex = decodeAll(texture(volume));
    check(tex && tex->texture.volume() && tex->texture.images[0].levels[0].depth == 4 && tex->levels[0][0].size() == 4 * 4 * 4 * 4,
          "a volume texture decodes all its slices");
    if (!tex) std::fprintf(stderr, "  %s\n", tex.error().c_str());
}

// One frame is {image, seconds, x, y, ux, uy, vx, vy}. A TEXS0003 table states `size`.
std::string frameTable(unsigned version, const std::vector<std::vector<float>>& frames,
                       std::pair<std::uint32_t, std::uint32_t> size = {16, 8})
{
    std::string out;
    tag(out, ("TEXS000" + std::to_string(version)).c_str());
    u32(out, std::uint32_t(frames.size()));
    if (version == 3) {
        u32(out, size.first);
        u32(out, size.second);
    }
    for (const auto& f : frames) {
        u32(out, std::uint32_t(f[0]));
        f32(out, f[1]);
        for (std::size_t i = 2; i < 8; ++i) {
            if (version == 1) {
                u32(out, std::uint32_t(std::int32_t(f[i])));
            } else {
                f32(out, f[i]);
            }
        }
    }
    return out;
}

// An animated texture of one raw 32x16 RGBA image, with the given frame table.
TexSpec animated(std::string table)
{
    TexSpec t = rawSpec(0, 4, false, 32, 16, 1, false);
    t.flags = 6;
    t.frameTable = std::move(table);
    return t;
}

void textureFrames()
{
    for (const unsigned version : {1u, 2u, 3u}) {
        TexSpec spec = rawSpec(0, 4, false, 32, 16, 1, false);
        spec.images.push_back(spec.images[0]);
        spec.flags = 6;
        spec.imgW = 24;
        spec.imgH = 12;
        // The second frame is stored a quarter turn round, as the spec's rotated example.
        spec.frameTable = frameTable(version, {{0, 0.1f, 0, 0, 16, 0, 0, 8}, {1, 0.25f, 16, 16, 0, -16, 8, 0}});
        const auto tex = decodeAll(texture(spec));
        const std::string what = "frame table TEXS000" + std::to_string(version);
        check(tex && tex->texture.frames && tex->texture.frames->frames.size() == 2, (what + " is read").c_str());
        if (!tex || !tex->texture.frames) {
            if (!tex) std::fprintf(stderr, "  %s\n", tex.error().c_str());
            continue;
        }
        const TextureFrame& rotated = tex->texture.frames->frames[1];
        check(rotated.image == 1 && rotated.seconds == 0.25f && rotated.x == 16 && rotated.uy == -16 && rotated.vx == 8,
              (what + " keeps a rotated frame's signed vectors").c_str());
        // GOAL: TEXS0003 reports the size it states, TEXS0001 the header's texture size (not the
        //       picture's), TEXS0002 none.
        // WHY-THIS-SHAPE: the picture (24x12) differs from the texture (32x16) and both width and
        //       height are compared, so taking the wrong field, or swapping them, is seen.
        // CONTROL: backend/daemon/we_compat/texture.cpp readFrames, TEXS0001 given the picture size
        //       (v1_picture) -> the TEXS0001 line red; TEXS0003's width and height swapped (v3_swap)
        //       -> the TEXS0003 line red. Restored.
        const auto& size = tex->texture.frames->frameSize;
        const bool sized = version == 2 ? !size
                           : version == 3 ? size && size->width == 16 && size->height == 8
                                          : size && size->width == 32 && size->height == 16;
        check(sized, (what + " reports a frame size exactly where the file gives one").c_str());

        // CONTROL: backend/daemon/we_compat/texture.cpp readFrames, a frame naming a missing image
        //       moved to image 0 instead of refused (missing_image): all three lines red. Restored.
        TexSpec badImage = spec;
        badImage.frameTable = frameTable(version, {{2, 0.1f, 0, 0, 16, 0, 0, 8}});
        refusedFor(decodeAll(texture(badImage)), what + ": a frame in a missing image is refused",
                   "in an image it does not hold");
    }
    // GOAL: an animated texture must have a frame table, and only an animated one may have bytes after
    //       its images.
    // CONTROL: backend/daemon/we_compat/texture.cpp parse, readFrames skipped when nothing follows the
    //       images (no_table) -> the first line red; the trailing-bytes refusal deleted (trailing) ->
    //       the second red. Restored.
    TexSpec missing = rawSpec(0, 4, false, 8, 8, 1, false);
    missing.flags = 4;
    refusedFor(decodeAll(texture(missing)), "an animated texture without its frame table is refused",
               "no frame table it can read");
    TexSpec extra = rawSpec(0, 4, false, 8, 8, 1, false);
    extra.frameTable = frameTable(3, {{0, 0.1f, 0, 0, 8, 0, 0, 8}});
    refusedFor(decodeAll(texture(extra)), "a still texture with bytes after its images is refused",
               "bytes after its images");
}

// The frame rules of P4-2B: where a frame may lie, how long it lasts, and the size a table states.
void textureFrameRules()
{
    const std::vector<float> inside = {0, 0.1f, 0, 0, 16, 0, 0, 8};
    // GOAL: a frame corner outside level 0 widened by its own size on every side is refused; a frame
    //       lying wholly past the edge but inside that band parses (the built-in particle sheets).
    // CONTROL: backend/daemon/we_compat/texture.cpp frameNear returning true (near_any) -> the three
    //       far lines red; frameNear narrowed to the level itself (near_inside) -> the past-the-edge
    //       line red. Restored.
    const std::string far = "far outside its image";
    parseRefusedFor(animated(frameTable(3, {{0, 0.1f, 49, 0, 16, 0, 0, 8}})), "a frame reaching x = 2W + 1 is refused", far);
    parseRefusedFor(animated(frameTable(3, {{0, 0.1f, 0, -17, 16, 0, 0, 8}})), "a frame at y = -H - 1 is refused", far);
    parseRefusedFor(animated(frameTable(3, {{0, 0.1f, 1e30f, 0, 16, 0, 0, 8}})), "a frame at x = 1e30 is refused", far);
    parses(animated(frameTable(3, {{0, 0.1f, 40, 16, 16, 0, 0, 8}})), "a frame wholly past the edge, within the band, parses");
    parses(animated(frameTable(3, {{0, 0.1f, 16, 16, 0, -16, 8, 0}})), "a rotated frame inside its image parses");

    // GOAL: frame times are all exactly 0 (untimed) or all at least kMinTextureFrameSeconds (timed);
    //       a mix, a negative time, or a timed frame under 1 ms is refused.
    // CONTROL: backend/daemon/we_compat/texture.cpp readFrames, each frame judged timed by its own time
    //       (times_each) -> both mixed lines red; the floor compared as `< 0` (times_floor) -> the
    //       sub-millisecond and timed-then-0 lines red; as `std::abs(...) <` (times_abs) -> the negative line red. Restored.
    const auto timedAs = [](const std::vector<float>& seconds, std::optional<bool> timed, const std::string& what) {
        std::vector<std::vector<float>> frames;
        for (const float s : seconds) frames.push_back({0, s, 0, 0, 16, 0, 0, 8});
        const auto file = TextureFile::parse(texture(animated(frameTable(3, frames))));
        if (timed) {
            check(file && file->texture().frames && file->texture().frames->timed() == *timed, what.c_str());
            if (!file) std::fprintf(stderr, "  %s -> %s\n", what.c_str(), file.error().c_str());
        } else {
            check(!file && contains(file.error(), "neither all zero nor all a usable length"), what.c_str());
            if (!file) std::printf("  %s -> %s\n", what.c_str(), file.error().c_str());
        }
    };
    timedAs({0, 0}, false, "an all-zero table parses untimed");
    timedAs({0.1f, 0.25f}, true, "an all-timed table parses timed");
    timedAs({kMinTextureFrameSeconds, kMinTextureFrameSeconds}, true, "frames of exactly 1 ms parse timed");
    timedAs({0, 0.1f}, std::nullopt, "an untimed frame then a timed one is refused");
    timedAs({0.1f, 0}, std::nullopt, "a timed frame then an untimed one is refused");
    timedAs({-0.1f, -0.1f}, std::nullopt, "negative frame times are refused");
    timedAs({0.0005f, 0.0005f}, std::nullopt, "frames under 1 ms are refused");

    // GOAL: a stated frame size is 1..16384 and fits level 0 of every image a frame is in, and only those.
    // CONTROL: backend/daemon/we_compat/texture.cpp readFrames, the TEXS0003 range test deleted
    //       (frame_range) -> the 0 and 16385 lines red; the fits-level-0 test deleted (frame_fits) -> the
    //       33-wide and TEXS0001 lines red; the fit tested against every image (frame_all_images) -> the
    //       last line red. Restored.
    parseRefusedFor(animated(frameTable(3, {inside}, {0, 8})), "a stated frame width of 0 is refused", "has frames of 0x8");
    parseRefusedFor(animated(frameTable(3, {inside}, {16, kMaxTextureDimension + 1})),
                    "a stated frame height over 16384 is refused", "has frames of 16x16385");
    parseRefusedFor(animated(frameTable(3, {inside}, {33, 8})), "a frame wider than its image is refused",
                    "frames larger than an image");
    TexSpec v1 = animated(frameTable(1, {inside}));
    v1.texW = 64;
    v1.texH = 32;
    parseRefusedFor(v1, "a TEXS0001 table whose header size exceeds its image is refused", "frames larger than an image");
    TexSpec twoImages = animated(frameTable(3, {inside}));
    twoImages.images.push_back({{8, 8, 1, pattern(8 * 8 * 4, 5)}});
    parses(twoImages, "a frame size larger than an image no frame is in parses");
}

// GOAL: a header size outside 1..16384, or a picture larger than the header size, is refused; so is a
//       still whose picture is larger than level 0 of any image; an animated texture's atlas is exempt.
// WHY-THIS-SHAPE: reasons matched, since a picture larger than its header usually also exceeds level 0.
// CONTROL: backend/daemon/we_compat/texture.cpp parse, `!inRange(texture.width) ||` deleted
//       (header_width) -> the over-16384 line red; the width-0 line stays green, as a 0 width is also
//       under the picture's (at least 1), so that half of the range test has no input of its own;
//       the picture-over-header comparisons deleted
//       (header_picture) -> that line red; the still rule made any_of (still_any) -> the two-image line
//       red; the animated exemption deleted (atlas) -> the atlas line red. Restored.
void textureHeader()
{
    const std::string header = "has a header size of";
    TexSpec zero = rawSpec(0, 4, false, 32, 16, 1, false);
    zero.texW = 0;
    parseRefusedFor(zero, "a header width of 0 is refused", header);
    TexSpec wide = rawSpec(0, 4, false, 32, 16, 1, false);
    wide.texW = kMaxTextureDimension + 1;
    parseRefusedFor(wide, "a header width over 16384 is refused", header);
    TexSpec noPicture = rawSpec(0, 4, false, 32, 16, 1, false);
    noPicture.imgH = 0;
    parseRefusedFor(noPicture, "a picture height of 0 is refused", header);
    TexSpec over = rawSpec(0, 4, false, 32, 16, 1, false);
    over.texW = 16;
    parseRefusedFor(over, "a picture wider than its header size is refused", header);

    const std::string level0 = "picture larger than its largest level";
    TexSpec still = rawSpec(0, 4, false, 32, 16, 1, false);
    still.texW = 64;
    still.texH = 32;
    still.imgW = 48;
    still.imgH = 16;
    parseRefusedFor(still, "a still whose picture exceeds level 0 is refused", level0);
    TexSpec two = still;
    two.images[0] = {{64, 32, 1, pattern(64 * 32 * 4, 1)}};
    two.images.push_back({{32, 16, 1, pattern(32 * 16 * 4, 2)}});
    parseRefusedFor(two, "a still whose picture exceeds one image's level 0 is refused", level0);
    TexSpec atlas = animated(frameTable(2, {{0, 0, 0, 0, 16, 0, 0, 8}}));
    atlas.texW = atlas.imgW = 64;
    atlas.texH = atlas.imgH = 32;
    parses(atlas, "an animated texture whose header size differs from its atlas parses");
}

void textureRefusals()
{
    TexSpec spec = rawSpec(4, 16, true, 16, 8, 3, true);
    spec.flags = 6;
    spec.frameTable = frameTable(3, {{0, 0.1f, 0, 0, 8, 0, 0, 8}});
    const std::string good = texture(spec);
    check(decodeAll(good).has_value(), "the texture to be cut decodes whole");
    int truncatedDecoded = 0;
    for (std::size_t n = 0; n < good.size(); ++n) {
        if (decodeAll(good.substr(0, n))) ++truncatedDecoded;
    }
    check(truncatedDecoded == 0, "every truncated texture is refused");

    const auto refused = [](const TexSpec& t, const char* what) {
        const auto tex = decodeAll(texture(t));
        check(!tex.has_value(), what);
        if (!tex) std::printf("  %s -> %s\n", what, tex.error().c_str());
    };
    TexSpec unknown = rawSpec(0, 4, false, 4, 4, 1, false);
    unknown.format = 12;
    refused(unknown, "an unsupported pixel format is refused");
    TexSpec kind = rawSpec(0, 4, false, 4, 4, 1, false);
    kind.embedded = 7;
    refused(kind, "an unsupported embedded kind is refused");
    TexSpec wrongSize = rawSpec(0, 4, false, 4, 4, 1, false);
    wrongSize.images[0][0].data.pop_back();
    refused(wrongSize, "a level of the wrong size for its format is refused");
    TexSpec halving = rawSpec(0, 4, false, 8, 8, 2, false);
    halving.images[0][1] = {3, 4, 1, pattern(48, 1)};
    refused(halving, "a level that is not half the one before is refused");
    TexSpec zero = rawSpec(0, 4, false, 4, 4, 1, false);
    zero.images[0][0] = {0, 4, 1, ""};
    refused(zero, "a level of no pixels is refused");
    TexSpec big = rawSpec(9, 1, false, 4, 4, 1, false);
    big.images[0][0] = {kMaxTextureDimension + 1, 1, 1, std::string(kMaxTextureDimension + 1, 'x')};
    refused(big, "a level wider than the limit is refused");
    TexSpec many = rawSpec(9, 1, false, 4, 4, 1, false);
    many.images.assign(kMaxTextureImages + 1, many.images[0]);
    refused(many, "more images than the limit are refused");

    // GOAL: LZ4 data that expands to anything but its stated size, short or long, is refused by decode.
    // WHY-THIS-SHAPE: each input parses (its stated size is right for its level), so only decode's
    //       exact-size expansion can refuse it, and its reason is matched.
    // CONTROL: backend/daemon/we_compat/texture.cpp expandInto, the result compared `>= 0` (lz4_short)
    //       -> the short line red; LZ4_decompress_safe_partial in its place (lz4_long) -> the long line
    //       red. Restored.
    const std::string expand = "does not expand to its stated size";
    TexSpec shortLz4 = rawSpec(0, 4, false, 8, 8, 1, true);
    shortLz4.images[0][0].stored = compressed(pattern(8 * 8 * 4 - 4, 0));
    refusedFor(decodeAll(texture(shortLz4)), "LZ4 data that expands short of its stated size is refused", expand);
    TexSpec longLz4 = rawSpec(0, 4, false, 8, 8, 1, true);
    longLz4.images[0][0].stored = compressed(pattern(8 * 8 * 4 + 64, 0));
    refusedFor(decodeAll(texture(longLz4)), "LZ4 data that expands past its stated size is refused", expand);
    TexSpec garbageLz4 = rawSpec(0, 4, false, 8, 8, 1, true);
    garbageLz4.images[0][0].stored = std::string("\xf0\xff\xff\xff\xff", 5);
    refused(garbageLz4, "LZ4 data that is not LZ4 is refused");
}

// Pictures stb_image_write cannot make: 16-bit and interlaced PNGs, PNGs whose data inflates past
// their stated size, and a progressive JPEG.

void be32(std::string& out, std::uint32_t v)
{
    for (int i = 3; i >= 0; --i) out += char((v >> (8 * i)) & 0xff);
}

void be16(std::string& out, std::uint32_t v)
{
    out += char((v >> 8) & 0xff);
    out += char(v & 0xff);
}

std::uint32_t crc32(std::string_view bytes)
{
    std::uint32_t crc = 0xffffffffu;
    for (const char c : bytes) {
        crc ^= std::uint8_t(c);
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

void pngChunk(std::string& out, const char* type, const std::string& data)
{
    be32(out, std::uint32_t(data.size()));
    const std::string body = type + data;
    out += body;
    be32(out, crc32(body));
}

// An RGBA PNG of `depth` bits per channel. Without `zlib` it holds no IDAT: parse() reads no further
// than the IHDR, so that is enough for its budget rule.
std::string pngFile(std::uint32_t w, std::uint32_t h, int depth, bool interlaced, const std::optional<std::string>& zlib)
{
    std::string out("\x89PNG\r\n\x1a\n", 8);
    std::string ihdr;
    be32(ihdr, w);
    be32(ihdr, h);
    ihdr += char(depth);
    ihdr += char(6);
    ihdr += '\0';
    ihdr += '\0';
    ihdr += char(interlaced ? 1 : 0);
    pngChunk(out, "IHDR", ihdr);
    if (zlib) pngChunk(out, "IDAT", *zlib);
    pngChunk(out, "IEND", "");
    return out;
}

using Rgba16 = std::array<std::uint16_t, 4>;

// The picture's filter-0 scanlines, in Adam7's seven passes when interlaced.
std::string scanlines(int w, int h, int depth, bool interlaced, const std::function<Rgba16(int, int)>& pixel)
{
    struct Pass {
        int x0, y0, dx, dy;
    };
    static constexpr Pass kAdam7[] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4},
                                      {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};
    static constexpr Pass kWhole[] = {{0, 0, 1, 1}};
    std::string out;
    out.reserve(std::size_t(w) * h * (depth / 2) + std::size_t(h) * 4);
    for (const Pass& p : interlaced ? std::span<const Pass>(kAdam7) : std::span<const Pass>(kWhole)) {
        for (int y = p.y0; p.x0 < w && y < h; y += p.dy) {
            out += '\0';
            for (int x = p.x0; x < w; x += p.dx) {
                for (const std::uint16_t v : pixel(x, y)) {
                    if (depth == 16) out += char(v >> 8);
                    out += char(v & 0xff);
                }
            }
        }
    }
    return out;
}

std::string zlibOf(const std::string& raw)
{
    int size = 0;
    unsigned char* z = stbi_zlib_compress(reinterpret_cast<unsigned char*>(const_cast<char*>(raw.data())),
                                          int(raw.size()), &size, 8);
    if (z == nullptr) {
        harnessFailure("stbi_zlib_compress could not compress a picture's scanlines");
        return {};
    }
    std::string out(reinterpret_cast<const char*>(z), std::size_t(size));
    std::free(z);
    return out;
}

// A zlib stream inflating to at least `bytes` zero bytes: one literal, then fixed-Huffman matches of
// 258 at distance 1, 13 bits each.
std::string zeroZlib(std::uint64_t bytes)
{
    std::string out("\x78\x01", 2);
    std::uint32_t acc = 0;
    int held = 0;
    const auto bit = [&](std::uint32_t b) {
        acc |= b << held;
        if (++held == 8) {
            out += char(acc);
            acc = 0;
            held = 0;
        }
    };
    // Header fields go least significant bit first, Huffman codes most significant first.
    const auto field = [&](std::uint32_t v, int n) { for (int i = 0; i < n; ++i) bit((v >> i) & 1); };
    const auto code = [&](std::uint32_t v, int n) { for (int i = n - 1; i >= 0; --i) bit((v >> i) & 1); };
    field(1, 1);
    field(1, 2);
    code(0x30, 8);
    std::uint64_t n = 1;
    for (; n < bytes; n += 258) {
        code(0xc5, 8);
        code(0, 5);
    }
    code(0, 7);
    if (held) out += char(acc);
    be32(out, std::uint32_t(((n % 65521) << 16) | 1));
    return out;
}

// A flat mid-grey progressive (SOF2) JPEG of three full-resolution components: a DC scan, then one
// AC scan per component, each block taking one bit per scan since every coefficient is 0.
std::string progressiveJpeg(std::uint32_t w, std::uint32_t h)
{
    std::string out("\xff\xd8", 2);
    const auto segment = [&out](unsigned char marker, const std::string& body) {
        out += '\xff';
        out += char(marker);
        be16(out, std::uint32_t(body.size() + 2));
        out += body;
    };
    segment(0xdb, std::string(1, '\0') + std::string(64, '\x01'));
    // One code, of length 1, for symbol 0: a DC difference of 0, and an AC end of block.
    std::string oneCode(16, '\0');
    oneCode[0] = 1;
    segment(0xc4, std::string(1, '\x00') + oneCode + std::string(1, '\0') + std::string(1, '\x10') + oneCode +
                      std::string(1, '\0'));
    std::string frame(1, '\x08');
    be16(frame, h);
    be16(frame, w);
    frame += '\x03';
    for (char id = 1; id <= 3; ++id) frame += std::string{id, '\x11', '\0'};
    segment(0xc2, frame);
    const std::size_t blocks = std::size_t((w + 7) / 8) * ((h + 7) / 8);
    const auto zeroBits = [&out](std::size_t n) {
        out.append(n / 8, '\0');
        if (n % 8) out += char(0xff >> (n % 8));
    };
    segment(0xda, std::string("\x03\x01\x00\x02\x00\x03\x00\x00\x00\x00", 10));
    zeroBits(3 * blocks);
    for (char id = 1; id <= 3; ++id) {
        segment(0xda, std::string{'\x01', id, '\0', '\x01', '\x3f', '\0'});
        zeroBits(blocks);
    }
    out += "\xff\xd9";
    return out;
}

// texture.h's decode budget: twice the picture's bytes, `perPixel` per pixel padded to 32, and 1 MiB.
std::uint64_t decodeBudget(std::uint64_t pictureBytes, std::uint64_t perPixel, std::uint64_t w, std::uint64_t h)
{
    return 2 * pictureBytes + perPixel * (w + 32) * (h + 32) + (1u << 20);
}

// Allocator and page rounding beside a budget.
constexpr std::uint64_t kPeakSlack = 4u << 20;
constexpr std::uint64_t kTwoGiB = 2ull << 30;

std::optional<std::uint64_t> statusBytes(std::string_view key)
{
    std::ifstream in("/proc/self/status");
    std::string line;
    while (std::getline(in, line)) {
        if (line.starts_with(key)) return std::stoull(line.substr(key.size())) * 1024;
    }
    return std::nullopt;
}

// How far this process's peak resident size rose while `run` ran, or nothing if /proc cannot say;
// `run` runs either way. Large blocks are kept on mmap and the heap trimmed first, so memory freed
// earlier and still resident cannot absorb a rise.
std::optional<std::uint64_t> peakRise(const std::function<void()>& run)
{
    ::mallopt(M_MMAP_THRESHOLD, 128 << 10);
    ::malloc_trim(0);
    std::ofstream reset("/proc/self/clear_refs");
    reset << "5";
    reset.close();
    const auto base = statusBytes("VmHWM:");
    run();
    const auto peak = statusBytes("VmHWM:");
    if (!reset || !base || !peak) return std::nullopt;
    return *peak > *base ? *peak - *base : 0;
}

// A host that cannot measure is NOT VERIFIED, never a pass. CONTROL: test side, clear_refs misnamed ->
// one NOT VERIFIED line per peak check (four). Restored.
void peakWithin(const std::optional<std::uint64_t>& rise, std::uint64_t bound, const std::string& what)
{
    if (!rise) {
        wallpaperx::testing::notVerified(what.c_str(), "/proc/self/clear_refs or VmHWM is not available here");
        return;
    }
    check(*rise <= bound, what.c_str());
    std::printf("  %s: peak rose %.1f MiB, bound %.1f MiB\n", what.c_str(), double(*rise) / (1 << 20),
                double(bound) / (1 << 20));
}

// One embedded picture as the texture's only level.
TexSpec embeddedSpec(std::int32_t kind, std::uint32_t w, std::uint32_t h, std::string picture, bool lz4 = false)
{
    TexSpec t{.texW = w, .texH = h, .imgW = w, .imgH = h, .embedded = kind};
    t.images.push_back({{w, h, 1, std::move(picture), lz4}});
    return t;
}

// Parse alone: for levels whose data is not meant to decode, or would take gigabytes to.
void parsesOnly(const TexSpec& t, const std::string& what)
{
    const auto file = TextureFile::parse(texture(t));
    check(file.has_value(), what.c_str());
    if (!file) std::fprintf(stderr, "  %s -> %s\n", what.c_str(), file.error().c_str());
}

void refusalSeen(const std::expected<std::vector<std::uint8_t>, std::string>& px, const std::string& what,
                 std::string_view reason)
{
    check(!px && contains(px.error(), reason), what.c_str());
    if (px) {
        std::fprintf(stderr, "  %s -> decoded\n", what.c_str());
    } else {
        std::printf("  %s -> %s\n", what.c_str(), px.error().c_str());
    }
}

void textureStbBudget()
{
    const std::string outOfMemory = "does not decode (outofmem)";
    // GOAL: an embedded PNG stating 16x16 whose data inflates to 64 MiB is refused by decode(), and the
    //       decode's peak memory rises by no more than its budget (about 2 MiB here) plus slack.
    // WHY-THIS-SHAPE: stb accepts data past the stated size, so without the budget this decodes; the
    //       reason is matched, and the peak (VmHWM, reset through clear_refs) is measured over decode()
    //       alone, the file being built before. A refusal alone would not show the memory was bounded.
    // CONTROL: backend/daemon/we_compat/texture.cpp stbAllocate/stbReallocate ignoring the budget
    //       (budget_off, budget_off_2): both lines red, "-> decoded", peak 65.0 MiB against 5.8. Restored.
    const std::string bomb = pngFile(16, 16, 8, false, zeroZlib(64u << 20));
    const auto file = TextureFile::parse(texture(embeddedSpec(13, 16, 16, bomb)));
    check(file.has_value(), "the inflate bomb parses, its stated size being small");
    if (file) {
        std::expected<std::vector<std::uint8_t>, std::string> px = std::unexpected(std::string());
        const auto rise = peakRise([&] { px = file->decode(0, 0); });
        refusalSeen(px, "a PNG inflating far past its stated size is refused by the decode budget", outOfMemory);
        peakWithin(rise, decodeBudget(bomb.size(), 16, 16, 16) + kPeakSlack,
                   "the refused inflate bomb's peak memory stays within its budget");
    }
    // GOAL: after a refused decode, a following decode on the same thread gets its own full budget.
    // WHY-THIS-SHAPE: the second picture (1024x1024) needs several times the bomb's budget, so a budget
    //       left over from the refused decode, or carried between decodes, cannot hold it.
    // CONTROL: backend/daemon/we_compat/texture.cpp StbBudgetScope keeping one static thread_local
    //       budget, set by the thread's first decode (budget_carry): this line red. Restored.
    const auto after = decodeAll(texture(embeddedSpec(13, 1024, 1024, png(1024, 1024))));
    check(after.has_value(), "after a refused decode, a larger picture decodes on the same thread");
    if (!after) std::fprintf(stderr, "  %s\n", after.error().c_str());

    // GOAL: legal large pictures fit their 16-byte-per-pixel budget: a 2048x2048 8-bit interlaced PNG and
    //       a 2048x2048 progressive JPEG of three full-resolution components decode, to their pixels.
    // WHY-THIS-SHAPE: these are stb's two costliest 8-bit layouts (Adam7 passes beside the whole image;
    //       progressive coefficients held for every block); a baseline picture would fit a far smaller
    //       budget and so could not show the figure is large enough.
    // CONTROL: backend/daemon/we_compat/texture.cpp kStbPixelBytes 16 -> 8 (px16_to_8, px16_to_8_2):
    //       both lines red. Restored.
    const auto pixel8 = [](int x, int y) {
        return Rgba16{std::uint16_t(x & 0xff), std::uint16_t(y & 0xff), std::uint16_t((x ^ y) & 0xff), 255};
    };
    const auto interlaced = decodeAll(
        texture(embeddedSpec(13, 2048, 2048, pngFile(2048, 2048, 8, true, zlibOf(scanlines(2048, 2048, 8, true, pixel8))))));
    bool same = interlaced.has_value();
    for (std::size_t i = 0; same && i < 2048u * 2048; ++i) {
        const Rgba16 want = pixel8(int(i % 2048), int(i / 2048));
        const auto* got = &interlaced->levels[0][0][i * 4];
        same = got[0] == want[0] && got[1] == want[1] && got[2] == want[2] && got[3] == want[3];
    }
    check(same, "an 8-bit interlaced 2048x2048 PNG decodes to its pixels within its budget");
    if (!interlaced) std::fprintf(stderr, "  %s\n", interlaced.error().c_str());
    const auto progressive = decodeAll(texture(embeddedSpec(2, 2048, 2048, progressiveJpeg(2048, 2048))));
    const bool grey = progressive && std::ranges::all_of(std::views::iota(std::size_t(0), 2048u * 2048), [&](std::size_t i) {
        const auto* px = &progressive->levels[0][0][i * 4];
        return std::abs(px[0] - 128) <= 1 && std::abs(px[1] - 128) <= 1 && std::abs(px[2] - 128) <= 1 && px[3] == 255;
    });
    check(grey, "a progressive 2048x2048 JPEG decodes to its pixels within its budget");
    if (!progressive) std::fprintf(stderr, "  %s\n", progressive.error().c_str());

    // GOAL: a well-compressed 16-bit interlaced RGBA PNG (1024x1024, one colour) decodes, to its pixels.
    // WHY-THIS-SHAPE: stb holds more a pixel for this layout than the 16 bytes others get; a small file
    //       is the point, since twice the file's bytes is also in the budget and a large one would cover
    //       a per-pixel figure that is too small.
    // CONTROL: backend/daemon/we_compat/texture.cpp kStbInterlaced16PixelBytes 32 -> 16 (px32_to_16):
    //       this line red. Restored.
    // COVERAGE-ASSERT: the file is checked to be under 1 MiB, so twice it cannot stand in for the pixels;
    //       test side, the bound made 1 KiB -> "FAIL: the 16-bit interlaced picture is well compressed".
    const Rgba16 flat{0x1234, 0x5678, 0x9abc, 0xffff};
    const std::string deep = pngFile(1024, 1024, 16, true, zlibOf(scanlines(1024, 1024, 16, true, [&](int, int) { return flat; })));
    check(deep.size() < (1u << 20), "the 16-bit interlaced picture is well compressed");
    const auto deepTex = decodeAll(texture(embeddedSpec(13, 1024, 1024, deep)));
    const bool deepSame = deepTex && std::ranges::all_of(std::views::iota(std::size_t(0), 1024u * 1024), [&](std::size_t i) {
        const auto* px = &deepTex->levels[0][0][i * 4];
        return px[0] == 0x12 && px[1] == 0x56 && px[2] == 0x9a && px[3] == 0xff;
    });
    check(deepSame, "a well-compressed 16-bit interlaced 1024x1024 PNG decodes to its pixels");
    if (!deepTex) std::fprintf(stderr, "  %s\n", deepTex.error().c_str());

    // GOAL: only a 16-bit interlaced PNG gets the 32-byte budget: a 16-bit non-interlaced and an 8-bit
    //       interlaced 1024x1024 PNG, each inflating to 40 MiB, are held to the 16-byte one.
    // WHY-THIS-SHAPE: no legal picture of these layouts needs over 16 bytes a pixel, so which budget was
    //       chosen shows only in how much a hostile one may take before it is refused: stb doubles its
    //       buffer, so under 16 it stops at 16 MiB and under 32 at 32 MiB. Peak memory is the measure;
    //       both budgets refuse it for the same reason.
    // CONTROL: backend/daemon/we_compat/texture.cpp stbPixelBytes judging on the bit depth alone
    //       (select_depth_only): the 16-bit line red, peak 31.5 MiB against 22.5; on the interlace byte
    //       alone (select_interlace_only): the 8-bit line red, 31.5 MiB. Unbroken: about 16. Restored.
    for (const bool sixteen : {true, false}) {
        const std::string pic = pngFile(1024, 1024, sixteen ? 16 : 8, !sixteen, zeroZlib(40u << 20));
        const std::string what = sixteen ? "a 16-bit non-interlaced PNG" : "an 8-bit interlaced PNG";
        const auto bombFile = TextureFile::parse(texture(embeddedSpec(13, 1024, 1024, pic)));
        check(bombFile.has_value(), (what + " inflate bomb parses").c_str());
        if (!bombFile) continue;
        std::expected<std::vector<std::uint8_t>, std::string> px = std::unexpected(std::string());
        const auto rise = peakRise([&] { px = bombFile->decode(0, 0); });
        refusalSeen(px, what + " inflating past its size is refused", outOfMemory);
        peakWithin(rise, decodeBudget(pic.size(), 16, 1024, 1024) + kPeakSlack, what + " is held to the 16-byte budget");
    }
}

void textureLz4Ratio()
{
    const std::string ratio = "claims more than its data could expand to";
    // GOAL: an LZ4 level claiming 255 x its stored bytes + 1 is refused at parse, raw or embedded; one
    //       claiming exactly 255 x is not refused (it parses).
    // WHY-THIS-SHAPE: R8 levels of 16321 and 16320 pixels from 64 stored bytes make the claim agree
    //       with the level's size, so only the ratio rule can refuse it; the exact case is parsed and
    //       not decoded, its 64 bytes not being LZ4.
    // CONTROL: backend/daemon/we_compat/texture.cpp, the ratio refusal deleted (ratio_off, ratio_off_2):
    //       both + 1 lines red, "-> parsed"; compared `>=` (ratio_ge, ratio_ge_2): both exact lines red.
    //       Restored.
    const auto rawClaim = [](std::uint32_t w) {
        TexSpec t{.format = 9, .texW = w, .texH = 1, .imgW = w, .imgH = 1};
        t.images.push_back({{w, 1, 1, "", true, w, std::string(64, '\x01')}});
        return t;
    };
    const auto embeddedClaim = [](std::uint32_t expanded) {
        TexSpec t = embeddedSpec(13, 16, 16, "", true);
        t.images[0][0].rawSize = expanded;
        t.images[0][0].stored = std::string(64, '\x01');
        return t;
    };
    parseRefusedFor(rawClaim(255 * 64 + 1), "a raw LZ4 level claiming 255x its stored bytes + 1 is refused", ratio);
    parsesOnly(rawClaim(255 * 64), "a raw LZ4 level claiming exactly 255x its stored bytes parses");
    parseRefusedFor(embeddedClaim(255 * 64 + 1), "an embedded LZ4 level claiming 255x its stored bytes + 1 is refused", ratio);
    parsesOnly(embeddedClaim(255 * 64), "an embedded LZ4 level claiming exactly 255x its stored bytes parses");

    // GOAL: a 1 GiB claim from 10 stored bytes is refused at parse, and refusing it raises peak memory
    //       by under 4 MiB.
    // WHY-THIS-SHAPE: what the rule prevents is decode() allocating the claim before LZ4 can say it is
    //       false, so a texture that parses is decoded inside the measurement, as a caller would.
    // CONTROL: backend/daemon/we_compat/texture.cpp, the ratio refusal deleted (ratio_off_3, ratio_off_4):
    //       both lines red, the claim parsed and decode() took 1023.8 MiB before refusing. Restored.
    TexSpec claim{.format = 0, .texW = 16384, .texH = 16384, .imgW = 16384, .imgH = 16384};
    claim.images.push_back({{16384, 16384, 1, "", true, 1u << 30, std::string(10, '\0')}});
    std::string claimBytes = texture(claim);
    std::optional<std::string> refusal;
    const auto rise = peakRise([&] {
        const auto f = TextureFile::parse(std::move(claimBytes));
        if (f) {
            const auto px = f->decode(0, 0);
            std::fprintf(stderr, "  the 1 GiB claim parsed; decode -> %s\n", px ? "decoded" : px.error().c_str());
        } else {
            refusal = f.error();
        }
    });
    check(refusal && contains(*refusal, ratio), "a 1 GiB LZ4 claim from 10 bytes is refused at parse");
    if (refusal) std::printf("  1 GiB claim -> %s\n", refusal->c_str());
    peakWithin(rise, 4u << 20, "refusing a 1 GiB LZ4 claim from 10 bytes raises peak memory under 4 MiB");

    // GOAL: a genuine LZ4 level near the maximum ratio (an all-zero 2048x2048 RGBA8 level) decodes.
    // CONTROL: backend/daemon/we_compat/texture.cpp kLz4MaxExpansion 255 -> 254 (ratio_254): this line
    //       red (and the two exact-255x lines). Restored.
    // COVERAGE-ASSERT: the level's real ratio is checked to be over 254.9, so it does test the margin;
    //       test side, the bound made 255.0 -> that line red.
    const std::string zeros(2048u * 2048 * 4, '\0');
    const double real = double(zeros.size()) / double(compressed(zeros).size());
    check(real > 254.9, "the all-zero level compresses to within 0.1 of LZ4's maximum ratio");
    std::printf("  all-zero 2048x2048 RGBA8: LZ4 ratio %.4f\n", real);
    TexSpec near{.format = 0, .texW = 2048, .texH = 2048, .imgW = 2048, .imgH = 2048};
    near.images.push_back({{2048, 2048, 1, zeros, true}});
    const auto nearTex = decodeAll(texture(near));
    check(nearTex && std::ranges::all_of(nearTex->levels[0][0], [](std::uint8_t b) { return b == 0; }),
          "an LZ4 level at nearly the maximum ratio decodes");
    if (!nearTex) std::fprintf(stderr, "  %s\n", nearTex.error().c_str());
}

void textureEmbeddedLz4()
{
    const std::string impossible = "compressed picture of an impossible size";
    const std::string expand = "does not expand to its stated size";
    for (const bool isPng : {true, false}) {
        const std::string kind = isPng ? "PNG" : "JPEG";
        const std::int32_t code = isPng ? 13 : 2;
        const std::string pic = isPng ? png(40, 20) : jpeg(40, 20);
        // GOAL: an LZ4-wrapped embedded picture decodes to its pixels.
        // CONTROL: backend/daemon/we_compat/texture.cpp decode, the stored bytes handed to stb instead of
        //       the expanded ones (emb_skip_expand, emb_skip_expand_2): both kinds' lines red. Restored.
        const auto good = decodeAll(texture(embeddedSpec(code, 40, 20, pic, true)));
        check(good && good->levels[0][0].size() == 40 * 20 * 4 &&
                  (!isPng || std::string(good->levels[0][0].begin(), good->levels[0][0].end()) == rgbaPicture(40, 20)),
              ("an LZ4-wrapped embedded " + kind + " decodes").c_str());
        if (!good) std::fprintf(stderr, "  %s\n", good.error().c_str());

        // GOAL: an embedded LZ4 picture claiming 0 bytes is refused at parse by the impossible-size rule.
        // WHY-THIS-SHAPE: reason matched: without the rule, decode refuses it anyway, as not expanding.
        // CONTROL: backend/daemon/we_compat/texture.cpp, `*expandedSize == 0 ||` deleted (emb_zero,
        //       emb_zero_2): both kinds' lines red, "-> parsed". Restored.
        TexSpec zero = embeddedSpec(code, 40, 20, pic, true);
        zero.images[0][0].rawSize = 0;
        parseRefusedFor(zero, "an embedded LZ4 " + kind + " claiming 0 bytes is refused", impossible);

        // GOAL: embedded LZ4 data that expands short of, or past, its stated size is refused by decode.
        // WHY-THIS-SHAPE: the short case's picture is whole, with zeros after it, so it would decode if
        //       only an LZ4 error refused; the long case fills the stated size exactly, so it would decode
        //       if a partial expansion were accepted.
        // CONTROL: backend/daemon/we_compat/texture.cpp decode, the embedded expansion refused only on an
        //       LZ4 error (emb_expand_errors_only, _2): both short lines red, "-> decoded";
        //       LZ4_decompress_safe_partial used (emb_expand_partial, _2): both long lines red. Restored.
        TexSpec shortLz4 = embeddedSpec(code, 40, 20, pic, true);
        shortLz4.images[0][0].rawSize = std::uint32_t(pic.size() + 16);
        refusedFor(decodeAll(texture(shortLz4)), "embedded LZ4 " + kind + " expanding short of its stated size is refused", expand);
        TexSpec longLz4 = embeddedSpec(code, 40, 20, pic, true);
        longLz4.images[0][0].stored = compressed(pic + std::string(64, 'x'));
        refusedFor(decodeAll(texture(longLz4)), "embedded LZ4 " + kind + " expanding past its stated size is refused", expand);
    }
    // GOAL: an embedded LZ4 picture claiming more than kMaxTextureFileBytes is refused at parse; one
    //       claiming exactly that parses.
    // WHY-THIS-SHAPE: stored in the fewest bytes the 255x rule allows, so that rule cannot refuse it,
    //       and on a 16x16 level, whose budget (about 1 GiB from the claim) stays under 2 GiB. Parsed only.
    // CONTROL: backend/daemon/we_compat/texture.cpp, `|| *expandedSize > kMaxTextureFileBytes` deleted
    //       (emb_over): the over line red, "-> parsed"; compared `>=` (emb_over_ge): the exact line red.
    //       Restored.
    const auto bigClaim = [](std::uint64_t expanded) {
        TexSpec t = embeddedSpec(13, 16, 16, "", true);
        t.images[0][0].rawSize = std::uint32_t(expanded);
        t.images[0][0].stored = std::string(std::size_t((expanded + 254) / 255), '\x01');
        return t;
    };
    parseRefusedFor(bigClaim(kMaxTextureFileBytes + 1), "an embedded LZ4 picture claiming over 512 MiB is refused", impossible);
    parsesOnly(bigClaim(kMaxTextureFileBytes), "an embedded LZ4 picture claiming exactly 512 MiB parses");
}

void texture2GiB()
{
    const std::string twoGiB = "would take more than 2 GiB to decode";
    const auto header = [](std::uint32_t side, int depth, bool interlaced) {
        return pngFile(side, side, depth, interlaced, std::nullopt);
    };
    const std::string jpegStub("\xff\xd8\xff\xd9", 4);
    // COVERAGE-ASSERT: each pair is checked to straddle 2 GiB by texture.h's own figure, so a boundary
    //       that moved in this file cannot leave both cases on one side. Test side, 8157 given as 8156
    //       here -> "FAIL (harness): the 2 GiB boundary pictures do not straddle 2 GiB".
    const auto straddles = [](std::uint64_t picture, std::uint64_t perPixel, std::uint32_t under) {
        return decodeBudget(picture, perPixel, under, under) <= kTwoGiB &&
               decodeBudget(picture, perPixel, under + 1, under + 1) > kTwoGiB;
    };
    if (!straddles(header(11550, 8, false).size(), 16, 11550) || !straddles(jpegStub.size(), 16, 11550) ||
        !straddles(header(8157, 16, true).size(), 32, 8157)) {
        harnessFailure("the 2 GiB boundary pictures do not straddle 2 GiB");
    }
    // GOAL: parse() refuses an embedded level whose decode budget is over 2 GiB with the 2 GiB sentence
    //       (an 8-bit PNG or a JPEG of 11551 square; an uncompressed 16-bit interlaced PNG of 8158 or 8192
    //       square), and the size just under each (11550, 8157) parses.
    // WHY-THIS-SHAPE: the pictures are headers only, which is all parse() reads, so nothing allocates
    //       pixels; parse is called alone, and the cases sit on either side of the exact boundary.
    // CONTROL: backend/daemon/we_compat/texture.cpp parse, the budget refusal deleted (cap_off, _2, _3):
    //       all four refused lines red, "-> parsed"; the 1 MiB term dropped from stbBudget (no_mib, _2):
    //       the 11551 and 8158 lines red; the cap tightened by 1 MiB (cap_tight, _2, _3): the three
    //       just-under lines red; parse using 16 bytes a pixel for every picture (parse_select_off, _2):
    //       the 8158 and 8192 lines red. Restored.
    parsesOnly(embeddedSpec(13, 11550, 11550, header(11550, 8, false)), "an 8-bit 11550x11550 PNG parses");
    parseRefusedFor(embeddedSpec(13, 11551, 11551, header(11551, 8, false)),
                    "an 8-bit 11551x11551 PNG is refused at parse for its decode budget", twoGiB);
    parsesOnly(embeddedSpec(2, 11550, 11550, jpegStub), "an 11550x11550 JPEG parses");
    parseRefusedFor(embeddedSpec(2, 11551, 11551, jpegStub), "an 11551x11551 JPEG is refused at parse for its decode budget", twoGiB);
    parsesOnly(embeddedSpec(13, 8157, 8157, header(8157, 16, true)), "a 16-bit interlaced 8157x8157 PNG parses");
    parseRefusedFor(embeddedSpec(13, 8158, 8158, header(8158, 16, true)),
                    "a 16-bit interlaced 8158x8158 PNG is refused at parse for its decode budget", twoGiB);
    parseRefusedFor(embeddedSpec(13, 8192, 8192, header(8192, 16, true)),
                    "a 16-bit interlaced 8192x8192 PNG is refused at parse for its decode budget", twoGiB);

    // GOAL: an LZ4-wrapped 16-bit interlaced 8192x8192 PNG, whose header parse() does not expand to
    //       read, parses, and decode() refuses it with the 2 GiB sentence before stb runs.
    // WHY-THIS-SHAPE: the picture is a header with no IDAT, so if stb ran it would refuse it for that
    //       (with another sentence); the sentence matched is the one decode() gives before calling stb.
    // CONTROL: backend/daemon/we_compat/texture.cpp decode using 16 bytes a pixel for every picture
    //       (decode_select_off): this line red, stb reached and refusing "(no IDAT)". Restored.
    const auto lz4File = TextureFile::parse(texture(embeddedSpec(13, 8192, 8192, header(8192, 16, true), true)));
    check(lz4File.has_value(), "an LZ4-wrapped 16-bit interlaced 8192x8192 PNG parses");
    if (lz4File) {
        refusalSeen(lz4File->decode(0, 0), "decode refuses an LZ4-wrapped 16-bit interlaced 8192x8192 PNG for its budget", twoGiB);
    } else {
        std::fprintf(stderr, "  %s\n", lz4File.error().c_str());
    }
}

// GOAL: levels each within the LZ4 ratio and the per-level limit, but totalling over
//       kMaxTextureDecodedBytes, are refused at parse by the total rule; a total of exactly it parses.
// WHY-THIS-SHAPE: three 1 GiB RGBA8 levels, each stored in the fewest bytes the 255x rule allows, so
//       no earlier rule refuses them, and the reason is matched; parsed only, so nothing is decoded.
// CONTROL: backend/daemon/we_compat/texture.cpp parse, the total refusal deleted (total_off): the
//       refused line red, "-> parsed"; compared `>=` (total_ge): the exact line red. Restored.
// COVERAGE-ASSERT: the inputs are checked against the ratio, the per-level limit and the total; test
//       side, the total taken as 3 GiB -> "FAIL (harness): the decoded-total levels are not within ...".
void textureDecodedTotal()
{
    static constexpr std::uint32_t kGiB = 1u << 30;
    static constexpr std::uint32_t kStored = (kGiB + 254) / 255;
    if (std::uint64_t(kStored) * 255 < kGiB || kGiB > kMaxTextureLevelBytes || 2ull * kGiB != kMaxTextureDecodedBytes) {
        harnessFailure("the decoded-total levels are not within the ratio and level limits, totalling the limit");
        return;
    }
    const auto gibLevels = [](std::uint32_t images) {
        TexSpec t{.format = 0, .texW = 16384, .texH = 16384, .imgW = 16384, .imgH = 16384};
        for (std::uint32_t i = 0; i < images; ++i) {
            t.images.push_back({{16384, 16384, 1, "", true, kGiB, std::string(kStored, '\x01')}});
        }
        return t;
    };
    parseRefusedFor(gibLevels(3), "three 1 GiB levels, each within its own limits, are refused for their total",
                    "once decoded");
    parsesOnly(gibLevels(2), "levels totalling exactly kMaxTextureDecodedBytes parse");
}

// Lists `folder`, calling `each` per entry; a folder that cannot be listed, at its start or part way
// through, is a fault of the host's items, not of the decoder, and is reported as one.
void listFolder(const fs::path& folder, bool recursive, const std::function<void(const fs::directory_entry&)>& each)
{
    std::error_code ec;
    if (recursive) {
        for (fs::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) each(*it);
    } else {
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) each(*it);
    }
    if (ec) harnessFailure("could not list " + folder.string() + ": " + ec.message());
}

// The user's own items, read in place: every scene package opens, and every texture in it and in the
// install's assets folder decodes.
// GOAL: every real package opens and every real texture decodes; a folder or file the run could not
//       read is a harness fault, never a pass and never blamed on the decoder.
// CONTROL: test side, on a synthetic steamapps folder holding an unlistable item folder and an unreadable
//       assets texture: "FAIL (harness): could not list ..." and "FAIL (harness): could not read ..." and
//       no "a built-in texture decodes" line. Removed after.
void realItems()
{
    const char* root = std::getenv("WALLPAPERX_TEST_STEAMAPPS");
    const fs::path workshop = root ? fs::path(root) / "workshop/content/431960" : fs::path();
    const fs::path assets = root ? fs::path(root) / "common/wallpaper_engine/assets" : fs::path();
    std::error_code ec;
    if (!root || !*root || !fs::is_directory(workshop, ec) || !fs::is_directory(assets, ec)) {
        wallpaperx::testing::notVerified("the real-item phase",
                                         "WALLPAPERX_TEST_STEAMAPPS names no steamapps folder with Wallpaper Engine "
                                         "items and its assets");
        return;
    }
    int packages = 0;
    int textures = 0;
    listFolder(workshop, false, [&](const fs::directory_entry& item) {
        std::error_code isDir;
        if (!item.is_directory(isDir)) return;
        listFolder(item.path(), false, [&](const fs::directory_entry& file) {
            if (file.path().extension() != ".pkg") return;
            ++packages;
            const auto pkg = openPackage(file.path());
            check(pkg.has_value(), ("package opens: " + file.path().string()).c_str());
            if (!pkg) return;
            for (const auto& entry : pkg->entries()) {
                if (!entry.name.ends_with(".tex")) continue;
                ++textures;
                const auto bytes = pkg->read(entry.name, kMaxTextureFileBytes);
                const auto tex = bytes ? decodeAll(std::move(*bytes)) : std::unexpected(bytes.error());
                if (!tex) std::fprintf(stderr, "  %s: %s: %s\n", file.path().c_str(), entry.name.c_str(), tex.error().c_str());
                check(tex.has_value(), "a packaged texture decodes");
            }
        });
    });
    listFolder(assets, true, [&](const fs::directory_entry& file) {
        std::error_code isFile;
        if (file.path().extension() != ".tex" || !file.is_regular_file(isFile)) return;
        std::ifstream in(file.path(), std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(in)), {});
        std::error_code sized;
        if (!in.is_open() || in.bad() || bytes.size() != fs::file_size(file.path(), sized) || sized) {
            harnessFailure("could not read " + file.path().string());
            return;
        }
        ++textures;
        const auto tex = decodeAll(std::move(bytes));
        if (!tex) std::fprintf(stderr, "  %s: %s\n", file.path().c_str(), tex.error().c_str());
        check(tex.has_value(), "a built-in texture decodes");
    });
    std::printf("we_compat_test: real items: %d packages, %d textures\n", packages, textures);
    check(packages > 0 && textures > 0, "the real-item folders held packages and textures");
}

} // namespace

// No argument: the synthetic phases. "real-items": the user's items alone, a separate entry so a host
// without them skips that entry and not the synthetic one.
int main(int argc, char** argv)
{
    wallpaperx::testing::g_testName = "we_compat_test";
    using Phase = std::pair<const char*, void (*)()>;
    const std::vector<Phase> synthetic = {
        {"packageReads", packageReads},           {"packageRefusals", packageRefusals},
        {"packageNames", packageNames},           {"packageNotRegular", packageNotRegular},
        {"packageShrinks", packageShrinks},       {"packageCost", packageCost},
        {"packageLargeTable", packageLargeTable}, {"textureFormats", textureFormats},
        {"textureEmbedded", textureEmbedded},     {"textureFrames", textureFrames},
        {"textureFrameRules", textureFrameRules}, {"textureHeader", textureHeader},
        {"textureRefusals", textureRefusals},     {"textureStbBudget", textureStbBudget},
        {"textureLz4Ratio", textureLz4Ratio},     {"textureEmbeddedLz4", textureEmbeddedLz4},
        {"texture2GiB", texture2GiB},             {"textureDecodedTotal", textureDecodedTotal},
    };
    const std::vector<Phase> real = {{"realItems", realItems}};
    const bool realOnly = argc > 1 && std::string_view(argv[1]) == "real-items";
    // GOAL: a scratch folder that cannot be made is reported as a harness fault, with the summary.
    // CONTROL: test side, TMPDIR pointing at a missing folder -> "FAIL (harness): could not make a
    //       scratch folder", exit 1, no phase run.
    if (!realOnly && scratchDir().empty()) {
        harnessFailure("could not make a scratch folder under the temporary directory; no phase was run");
    } else {
        for (const auto& [name, run] : realOnly ? real : synthetic) {
            if (phaseSelected(name)) guardedPhase(name, run);
        }
    }
    if (!realOnly && !scratchDir().empty()) {
        std::error_code ec;
        fs::remove_all(scratchDir(), ec);
    }
    const bool failed = wallpaperx::testing::reportCheckFailures();
    if (wallpaperx::testing::reportHarnessFaults("in we_compat_test's own fixtures or the host's files, not in the code under test")) return 1;
    if (failed) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
