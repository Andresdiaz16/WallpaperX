// S4.9 -- the decoded-size bound is applied from the image *header*, before any pixel
// memory is allocated.
//
// The whole difficulty of this goal is the oracle. "StaticRenderer threw" is satisfied
// just as well by a renderer that decodes a gigabyte first and complains afterwards,
// which is exactly the state this code was in before f1aa32c and exactly the state a
// future edit would return it to by moving the size test below stbi_load(). The verdict
// is not the property; the memory is.
//
// So this probe measures allocation *requests*, by interposing malloc/calloc/realloc in
// this executable. stb_image is compiled into the renderer's static library with the
// default STBI_MALLOC (= malloc), so its pixel-buffer allocation is a call into this
// file's symbol -- which means the request is observable without a single test-only line
// in any production file (X.6).
//
// Requests, and not peak RSS, for a reason that was measured rather than assumed: with
// the cap disabled, the rejected load asked for 976 MiB and VmHWM moved by 348 KiB --
// memory that is allocated and freed without being written to never enters RSS at all. An
// RSS oracle would have graded that gigabyte as no growth. VmHWM is printed alongside as
// context, not asserted -- see the annotation on checkHeaderRejectionAllocatesNothing().

#include "vulkan/device.h"
#include "vulkan/offscreen.h"
#include "renderers/static_renderer.h"
#include "support/validation_grade.h"

// No STB_IMAGE_IMPLEMENTATION here, deliberately: this TU links against the *same*
// stb_image build the renderer ships (static_renderer.cpp owns the implementation). A
// second copy compiled into the test would let the instrument observe an stb that
// production does not use, which is the difference between measuring the shipped decoder
// and measuring a lookalike.
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

// --------------------------------------------------------------------------- //
// Allocation instrument
// --------------------------------------------------------------------------- //

// A sanitized build cannot host this instrument, and must not pretend to.
// ThreadSanitizer and AddressSanitizer install their own allocator by intercepting
// malloc/free; a strong `malloc` in the executable wins the link, hands out glibc
// pointers, and the sanitizer's free() then operates on memory it never allocated. That
// is not theoretical -- built with -DWALLPAPERX_SANITIZE=thread, the first version of
// this file SEGVed before reaching main. So the whole instrument is compiled out there
// and the probe exits 77 (ctest Skipped) with a printed reason, rather than running the
// checks with an oracle that cannot see anything and reporting green.
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#define WALLPAPERX_ALLOCATION_INSTRUMENT 0
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#define WALLPAPERX_ALLOCATION_INSTRUMENT 0
#else
#define WALLPAPERX_ALLOCATION_INSTRUMENT 1
#endif
#else
#define WALLPAPERX_ALLOCATION_INSTRUMENT 1
#endif

namespace {

std::atomic<bool> g_armed{false};
std::atomic<size_t> g_maxRequest{0};
std::atomic<uint64_t> g_requestCount{0};

// Must not allocate, and must be safe from any thread: it runs inside malloc.
void noteRequest(size_t bytes) {
    if (!g_armed.load(std::memory_order_relaxed)) return;
    g_requestCount.fetch_add(1, std::memory_order_relaxed);
    size_t previous = g_maxRequest.load(std::memory_order_relaxed);
    while (bytes > previous &&
           !g_maxRequest.compare_exchange_weak(previous, bytes, std::memory_order_relaxed)) {
    }
}

struct Window {
    explicit Window() {
        g_maxRequest.store(0, std::memory_order_relaxed);
        g_requestCount.store(0, std::memory_order_relaxed);
        g_armed.store(true, std::memory_order_relaxed);
    }
    ~Window() { g_armed.store(false, std::memory_order_relaxed); }
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    static size_t maxRequest() { return g_maxRequest.load(std::memory_order_relaxed); }
    static uint64_t requests() { return g_requestCount.load(std::memory_order_relaxed); }
};

constexpr size_t kMiB = 1024ull * 1024;

}  // namespace

// glibc's own entry points, so no dlsym(RTLD_NEXT) bootstrap is needed (dlsym itself
// allocates, which makes that idiom recursive in exactly this situation). free() is
// deliberately NOT interposed: __libc_malloc and malloc are the same allocator, so
// glibc's free handles these pointers, and every uninterposed allocation in the process
// stays valid.
#if WALLPAPERX_ALLOCATION_INSTRUMENT

extern "C" {

void* __libc_malloc(size_t);
void* __libc_calloc(size_t, size_t);
void* __libc_realloc(void*, size_t);

void* malloc(size_t size) {
    void* p = __libc_malloc(size);
    noteRequest(size);
    return p;
}

void* calloc(size_t count, size_t size) {
    void* p = __libc_calloc(count, size);
    noteRequest(count * size);
    return p;
}

void* realloc(void* ptr, size_t size) {
    void* p = __libc_realloc(ptr, size);
    noteRequest(size);
    return p;
}

}  // extern "C"

#endif  // WALLPAPERX_ALLOCATION_INSTRUMENT

namespace {

// --------------------------------------------------------------------------- //
// Test images
// --------------------------------------------------------------------------- //

std::string assetDir() {
    std::filesystem::create_directories(WALLPAPERX_TEST_IMAGE_DIR);
    return std::string(WALLPAPERX_TEST_IMAGE_DIR);
}

uint32_t crc32Of(const unsigned char* data, size_t length) {
    static uint32_t table[256];
    static bool built = false;
    if (!built) {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
        built = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void appendBe32(std::vector<unsigned char>& out, uint32_t value) {
    out.push_back(static_cast<unsigned char>(value >> 24));
    out.push_back(static_cast<unsigned char>(value >> 16));
    out.push_back(static_cast<unsigned char>(value >> 8));
    out.push_back(static_cast<unsigned char>(value));
}

void appendChunk(std::vector<unsigned char>& out, const char type[5], const std::vector<unsigned char>& payload) {
    appendBe32(out, static_cast<uint32_t>(payload.size()));
    std::vector<unsigned char> typed(type, type + 4);
    typed.insert(typed.end(), payload.begin(), payload.end());
    out.insert(out.end(), typed.begin(), typed.end());
    appendBe32(out, crc32Of(typed.data(), typed.size()));
}

// A PNG whose IHDR *declares* width x height but whose pixel data is a stub.
//
// The point of this file is that the declared size is enormous while the file is a few
// dozen bytes: stbi_info() answers from the IHDR alone, so the header check can reject it
// without touching pixel memory, while anything that reaches stbi_load() asks the
// allocator for the full decoded size first. Generating a genuinely 23000x23000 PNG would
// need 2.1 GB in this process just to write it, which would wreck the very measurement
// this probe exists to take.
std::string writeOversizeHeaderPng(const std::string& path, uint32_t width, uint32_t height) {
    std::vector<unsigned char> png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

    std::vector<unsigned char> ihdr;
    appendBe32(ihdr, width);
    appendBe32(ihdr, height);
    ihdr.push_back(8);  // bit depth
    // Colour type 2 (RGB), not 6 (RGBA), and the difference is load-bearing. stb_image
    // has a guard of its own -- `(1 << 30) / x / channels < y` -- keyed on the channel
    // count *in the file*. At four channels it refuses anything past ~16384x16384 before
    // allocating, so an RGBA header of this size never reaches stbi_load's allocation and
    // the counterfactual below would measure zero: the probe would then be comparing two
    // zeroes and passing. At three channels the same dimensions sail past stb's guard,
    // which is what leaves StaticRenderer's own cap as the only thing standing between
    // the file and a gigabyte.
    ihdr.push_back(2);
    ihdr.push_back(0);  // deflate
    ihdr.push_back(0);  // adaptive filtering
    ihdr.push_back(0);  // no interlace
    appendChunk(png, "IHDR", ihdr);

    // A zlib header followed by a truncated deflate stream. Enough for the decoder to
    // commit to decoding (and therefore to allocate) and far too little to produce the
    // declared pixels.
    appendChunk(png, "IDAT", {0x78, 0x01, 0x63, 0x00, 0x00});
    appendChunk(png, "IEND", {});

    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    file.close();
    if (!file) throw std::runtime_error("image_bounds_probe: could not write " + path);
    return path;
}

std::string writeSolidPng(const std::string& path, int side) {
    std::vector<unsigned char> pixels(static_cast<size_t>(side) * side * 4, 0);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i + 0] = 40;
        pixels[i + 1] = 160;
        pixels[i + 2] = 220;
        pixels[i + 3] = 255;
    }
    if (stbi_write_png(path.c_str(), side, side, 4, pixels.data(), side * 4) == 0) {
        throw std::runtime_error("image_bounds_probe: could not write " + path);
    }
    return path;
}

// Peak resident set size in KiB, or 0 when it cannot be read.
size_t vmHighWaterKb() {
    std::ifstream status("/proc/self/status");
    std::string key;
    while (status >> key) {
        if (key == "VmHWM:") {
            size_t kb = 0;
            status >> kb;
            return kb;
        }
        status.ignore(1 << 16, '\n');
    }
    return 0;
}

// --------------------------------------------------------------------------- //
// Checks
// --------------------------------------------------------------------------- //

// The declared size of the oversize header: 16000x16000, the case static_renderer.cpp's
// own comment records ("a 995 KB PNG of 16000x16000 decodes to 1.0 GB", OOM-killed under
// a 1.2 GB limit). GOALS.md names 23000x23000; that one cannot be used here, because
// stb_image's internal guard rejects it from the header for any RGBA file and no
// allocation ever happens -- measuring nothing against nothing. 16000x16000 is inside
// stb's guard and four times over StaticRenderer's cap, which is precisely the band where
// the cap is the only protection.
constexpr uint32_t kOversizeSide = 16000;
constexpr uint64_t kOversizeDecodedBytes =
    static_cast<uint64_t>(kOversizeSide) * kOversizeSide * 4;  // 1.0 GiB of RGBA

// Any allocation at or above this, inside the window, means pixel memory. It is a
// quarter of StaticRenderer's 256 MiB cap, so by construction *every* image the cap
// rejects must ask for more than this if it is decoded first, while nothing else on this
// path (an ICD's background thread, the exception's own string) comes close.
constexpr size_t kPixelMemoryFloor = 64 * kMiB;

int g_failures = 0;

void check(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: image_bounds_probe: %s\n", what);
    ++g_failures;
}

// GOAL: the instrument is real -- an allocation request made by the *shipped* stb_image
//       build, for the pixels of this very file, is seen and sized by this probe. Without
//       it, "no large allocation was observed" is equally consistent with an interposer
//       that was never called, and the S4.9 check below would be vacuous.
// WHY-THIS-SHAPE: calls stbi_load() on the same oversize file directly, so the number it
//       records is the counterfactual the real check is measured against -- what a
//       decode-first StaticRenderer would have asked for, measured on this host rather
//       than assumed. A simpler smoke test ("malloc(1<<20) is observed") would prove the
//       symbol is interposed but not that *stb's* allocations route through it, which is
//       the only thing that matters here.
// CONTROL: `noteRequest(size)` deleted from the malloc wrapper -> "FAIL: ... a direct
//       stbi_load of the oversize header asks the allocator for pixel-sized memory", with
//       the printed line reading "largest 0.0 MiB" against a decoded size of 976.6 MiB.
//       Restored.
// COVERAGE-ASSERT: asserts the window saw a non-zero number of requests at all, so an
//       interposer that is optimised out or linked away fails here instead of quietly
//       making every later measurement zero.
uint64_t checkInstrumentSeesTheDecoder(const std::string& oversizePath) {
    size_t maxRequest = 0;
    uint64_t requests = 0;
    stbi_uc* pixels = nullptr;
    int w = 0, h = 0, channels = 0;
    {
        Window window;
        pixels = stbi_load(oversizePath.c_str(), &w, &h, &channels, STBI_rgb_alpha);
        maxRequest = Window::maxRequest();
        requests = Window::requests();
    }
    // The file is a stub, so this must not have produced an image; freeing anyway keeps
    // the probe honest if stb ever manages it.
    if (pixels != nullptr) stbi_image_free(pixels);

    std::printf("image_bounds_probe: a direct stbi_load of the %ux%u header made %llu allocation "
                "requests, the largest %.1f MiB (the decoded size is %.1f MiB)\n",
                kOversizeSide, kOversizeSide, static_cast<unsigned long long>(requests),
                static_cast<double>(maxRequest) / kMiB, static_cast<double>(kOversizeDecodedBytes) / kMiB);

    check(requests > 0, "the allocation instrument observes requests at all during a decode");
    // Deliberately the SAME threshold the header-rejection check compares against, from
    // the other side: one number above it and one below it, taken by one instrument in
    // one process, is what makes the pair a measurement rather than two assertions.
    check(maxRequest >= kPixelMemoryFloor,
          "a direct stbi_load of the oversize header asks the allocator for pixel-sized memory "
          "(this is the counterfactual the header-rejection check is measured against)");
    return maxRequest;
}

// GOAL: S4.9 -- an image whose declared dimensions exceed the cap is rejected from the
//       header alone: no pixel-sized allocation is ever requested, and the message names
//       the declared dimensions.
// WHY-THIS-SHAPE: asserts on the largest allocation *request* inside the constructor,
//       not on peak RSS and not on the exception alone. "It threw" is satisfied by a
//       renderer that decodes 2.1 GB and complains afterwards -- the exact pre-f1aa32c
//       behaviour -- so the verdict cannot be the oracle. RSS cannot be the oracle
//       either: a 2.1 GB buffer that is allocated and freed without being written never
//       enters RSS, so an RSS check reports "no growth" for the decode-first case it
//       exists to catch. The request is what is unavoidable: nothing can decode into
//       memory it never asked for. VmHWM is printed for context and deliberately not
//       asserted, because on this file it cannot go red either way.
// CONTROL: static_renderer.cpp's `if (decodedBytes > kMaxDecodedBytes)` disabled ->
//       "FAIL: nothing pixel-sized is allocated on the way to the rejection" (printed
//       line: "largest 976.56 MiB") and "FAIL: the rejection names the declared
//       dimensions" (it threw stb's "failed to load image ... not enough pixels"
//       instead). Two things worth noting from that run, both of which decided the shape
//       above: "an image over the decoded-size cap is rejected" stayed GREEN, and VmHWM
//       moved 277488 -> 277836 KiB -- i.e. an RSS oracle would have graded that gigabyte
//       as no growth at all. Reverted.
void checkHeaderRejectionAllocatesNothing(wallpaperx::Device& device, VkRenderPass renderPass,
                                          const std::string& oversizePath) {
    const size_t hwmBefore = vmHighWaterKb();
    std::string message;
    bool threw = false;
    size_t maxRequest = 0;
    uint64_t requests = 0;
    {
        Window window;
        try {
            wallpaperx::StaticRenderer renderer(device, renderPass, oversizePath, WALLPAPERX_SHADER_DIR);
        } catch (const std::exception& e) {
            threw = true;
            message = e.what();
        }
        maxRequest = Window::maxRequest();
        requests = Window::requests();
    }
    const size_t hwmAfter = vmHighWaterKb();

    std::printf("image_bounds_probe: rejecting the %ux%u header made %llu allocation requests, the "
                "largest %.2f MiB; VmHWM %zu -> %zu KiB\n",
                kOversizeSide, kOversizeSide, static_cast<unsigned long long>(requests),
                static_cast<double>(maxRequest) / kMiB, hwmBefore, hwmAfter);
    std::printf("image_bounds_probe: rejection said: %s\n", threw ? message.c_str() : "(nothing -- it did not throw)");

    check(threw, "an image over the decoded-size cap is rejected");
    check(maxRequest < kPixelMemoryFloor,
          "nothing pixel-sized is allocated on the way to the rejection");
    check(message.find(std::to_string(kOversizeSide) + "x" + std::to_string(kOversizeSide)) != std::string::npos,
          "the rejection names the declared dimensions");
}

// GOAL: the measurement above is taken through the production call path, not only
//       through this file's own stbi_load. If StaticRenderer's allocations bypassed the
//       interposer, a zero reading from the rejected load would mean nothing.
// WHY-THIS-SHAPE: loads a legitimately-sized image through the same constructor and
//       asserts the instrument *did* see its pixel buffer. This is the positive half of
//       the pair: the same window, the same code path, the same instrument, differing
//       only in which file is named. Asserting a lower bound rather than an exact figure
//       -- how many buffers stb allocates for a given PNG is an implementation detail,
//       but it cannot decode 4 MiB of pixels while asking for less than 4 MiB.
// CONTROL: the same `noteRequest(size)` deletion used above -> "FAIL: ... the instrument
//       observes StaticRenderer's own pixel allocation", printed line "largest request
//       0.04 MiB (decoded size 4.00 MiB)". Restored.
void checkInstrumentSeesTheProductionPath(wallpaperx::Device& device, VkRenderPass renderPass,
                                          const std::string& validPath, int side) {
    const size_t decoded = static_cast<size_t>(side) * side * 4;
    size_t maxRequest = 0;
    bool constructed = false;
    std::string failure;
    {
        Window window;
        try {
            wallpaperx::StaticRenderer renderer(device, renderPass, validPath, WALLPAPERX_SHADER_DIR);
            constructed = true;
        } catch (const std::exception& e) {
            failure = e.what();
        }
        maxRequest = Window::maxRequest();
    }
    std::printf("image_bounds_probe: loading the valid %dx%d image made a largest request of %.2f MiB "
                "(decoded size %.2f MiB)\n",
                side, side, static_cast<double>(maxRequest) / kMiB, static_cast<double>(decoded) / kMiB);

    check(constructed, "an image inside the cap still loads");
    if (!constructed) std::fprintf(stderr, "  it failed with: %s\n", failure.c_str());
    check(maxRequest >= decoded,
          "the instrument observes StaticRenderer's own pixel allocation (so a zero reading from the "
          "rejected load is evidence rather than an artefact)");
}

}  // namespace

int main() {
#if !WALLPAPERX_ALLOCATION_INSTRUMENT
    // 77, not 0. Every check here rests on the allocation instrument, so without it this
    // binary can only produce an inconclusive result -- and an inconclusive result that
    // exits 0 is indistinguishable from a verified one.
    std::printf("image_bounds_probe: SKIPPED -- this is a sanitized build, whose runtime intercepts "
                "malloc/free itself; the allocation instrument S4.9's oracle is built on cannot coexist "
                "with it. Nothing about the decoded-size bound was verified by this run.\n");
    return 77;
#else
    // The two "instrument" checks and the S4.9 check must run in one process: the
    // counterfactual is only a counterfactual if it is measured by the same instrument,
    // in the same build, on the same host.
    const bool ok = wallpaperx::testing::gradedRun(
        "image_bounds_probe", [&](wallpaperx::testing::ValidationGrade& grade) {
            try {
                const std::string dir = assetDir();
                const std::string oversizePath =
                    writeOversizeHeaderPng(dir + "/bounds_oversize_header.png", kOversizeSide, kOversizeSide);
                constexpr int kValidSide = 1024;
                const std::string validPath = writeSolidPng(dir + "/bounds_valid.png", kValidSide);

                checkInstrumentSeesTheDecoder(oversizePath);

                wallpaperx::Device device(/*enableValidation=*/true);
                grade.watch(device);
                wallpaperx::OffscreenTargetManager targets(device);

                checkHeaderRejectionAllocatesNothing(device, targets.renderPass(), oversizePath);
                checkInstrumentSeesTheProductionPath(device, targets.renderPass(), validPath, kValidSide);
            } catch (const std::exception& e) {
                // A host with no usable ICD lands here. Reported as a failure of this
                // probe's setup, with the reason, rather than as a silent pass.
                std::fprintf(stderr, "FAIL: image_bounds_probe: could not run: %s\n", e.what());
                ++g_failures;
            }
            return g_failures == 0;
        });

    if (!ok) {
        // Two distinct verdicts. gradedRun folds the validation grade into its result, so
        // a run whose every check passed can still be red because the GPU-side oracle
        // could not be made -- and printing "0 check(s) failed" for that would send the
        // reader looking for a check that does not exist.
        if (g_failures > 0) {
            std::fprintf(stderr, "image_bounds_probe: %d check(s) failed\n", g_failures);
        } else {
            std::fprintf(stderr, "image_bounds_probe: every check passed, but the validation grade did not -- "
                                 "see the FAIL line above\n");
        }
        return 1;
    }
    std::printf("image_bounds_probe: all checks passed\n");
    return 0;
#endif  // WALLPAPERX_ALLOCATION_INSTRUMENT
}
