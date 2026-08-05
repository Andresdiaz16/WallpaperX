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
// RSS oracle would have graded that gigabyte as no growth. The process's peak RSS is
// printed once at the end of the run as context, not asserted -- see the annotation on
// checkHeaderRejectionAllocatesNothing().

#include "vulkan/device.h"
#include "vulkan/offscreen.h"
#include "renderers/static_renderer.h"
#include "support/check.h"
#include "support/test_image.h"
#include "support/validation_grade.h"

// No STB_IMAGE_IMPLEMENTATION here, deliberately: this TU links against the *same*
// stb_image build the renderer ships (static_renderer.cpp owns the implementation). A
// second copy compiled into the test would let the instrument observe an stb that
// production does not use, which is the difference between measuring the shipped decoder
// and measuring a lookalike.
#include <stb_image.h>

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

// A build whose runtime already owns the allocator cannot host this instrument, and must
// not pretend to. Any allocator-replacing sanitizer -- thread, address, leak, memory,
// hwaddress -- intercepts malloc/free itself; a strong `malloc` in the executable wins
// the link, hands out glibc pointers, and the sanitizer's free() then operates on memory
// it never allocated. That is not theoretical: built with -DWALLPAPERX_SANITIZE=thread,
// the first version of this file SEGVed before reaching main. So the whole instrument is
// compiled out there and the probe exits 77 (ctest Skipped) with a printed reason, rather
// than running the checks with an oracle that cannot see anything and reporting green.
//
// The verdict is CMake's, not the preprocessor's: renderer/CMakeLists.txt defines
// WALLPAPERX_ALLOCATION_INSTRUMENT=0 whenever WALLPAPERX_SANITIZE is set. Deciding it
// here from predefined macros cannot be made complete -- GCC defines no predefined macro
// at all for -fsanitize=leak (checked with `g++ -fsanitize=leak -dM -E` on gcc 16.1), and
// the previous version of this block tested only __SANITIZE_THREAD__/__SANITIZE_ADDRESS__.
// A `-DWALLPAPERX_SANITIZE=leak` build therefore compiled the instrument in and died
// before main with "LeakSanitizer: CHECK failed: lsan_interceptors.cpp:83
// ((!lsan_init_is_running)) != (0)", exit 23 -- reported to the reader as a failing
// renderer test rather than as the Skip this guard exists to produce. The macro chain
// below survives as the fallback for a standalone `cmake -S renderer` configure, where
// WALLPAPERX_SANITIZE does not exist as a variable; gcc 16 and clang both answer
// __has_feature(leak_sanitizer) there (verified with a preprocessor probe), so the
// fallback covers LSan too.
#ifndef WALLPAPERX_ALLOCATION_INSTRUMENT
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_LEAK__) || \
    defined(__SANITIZE_MEMORY__) || defined(__SANITIZE_HWADDRESS__)
#define WALLPAPERX_ALLOCATION_INSTRUMENT 0
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer) ||  \
    __has_feature(leak_sanitizer) || __has_feature(memory_sanitizer) ||     \
    __has_feature(hwaddress_sanitizer)
#define WALLPAPERX_ALLOCATION_INSTRUMENT 0
#else
#define WALLPAPERX_ALLOCATION_INSTRUMENT 1
#endif
#else
#define WALLPAPERX_ALLOCATION_INSTRUMENT 1
#endif
#endif  // WALLPAPERX_ALLOCATION_INSTRUMENT

namespace {

constexpr size_t kMiB = 1024ull * 1024;

// TWO FLAGS, ON PURPOSE, and they are not interchangeable.
//
// `g_anyWindowOpen` is process-global so that a thread outside a window pays one relaxed
// load and, crucially, touches no thread-local storage at all. That matters: this code
// runs inside malloc, and malloc is called by the dynamic loader and by threads the ICD
// creates -- taking a TLS access on every allocation in the process, including ones made
// before the main thread's TCB is usable, is a startup hazard for no benefit.
//
// `g_armedHere` is thread-local and is what actually admits an allocation as evidence.
// The property under test is "the StaticRenderer constructor, on this thread, did not ask
// for pixel memory", but the measurement used to be process-wide: a validation layer or
// an ICD worker allocating 64 MiB concurrently would have been charged to production code
// and printed as "nothing pixel-sized is allocated on the way to the rejection" FAILing.
thread_local bool g_armedHere = false;
std::atomic<bool> g_anyWindowOpen{false};

// Counters are thread-local plain scalars, not atomics: only the arming thread writes or
// reads them once g_armedHere gates the path, so there is nothing to synchronise. They
// are constant-initialised and trivially destructible, which is load-bearing -- a
// thread_local with a destructor would register a __cxa_thread_atexit handler, and that
// allocates, from inside malloc.
thread_local size_t g_maxRequest = 0;
thread_local uint64_t g_requestCount = 0;

// The identity half of the oracle: a size only the decoder asks for (see kWatchedStbSize).
thread_local size_t g_watchSize = 0;
thread_local bool g_sawWatched = false;

// A fixed-size record of the big requests seen in the window, for triage when the identity
// check goes red: it answers "what did the decoder ask for instead?" without which an stb
// upgrade that changes its buffering strategy and a genuine regression look identical.
// Fixed-size and stack-free because this fills up inside malloc.
constexpr size_t kBigRequestFloor = 1 * kMiB;
constexpr size_t kBigRequestSlots = 12;
thread_local size_t g_bigRequests[kBigRequestSlots] = {};
thread_local size_t g_bigRequestCount = 0;  // may exceed kBigRequestSlots

// Must not allocate, and must be safe from any thread: it runs inside malloc.
void noteRequest(size_t bytes) {
    if (!g_anyWindowOpen.load(std::memory_order_relaxed)) return;
    if (!g_armedHere) return;
    ++g_requestCount;
    if (bytes > g_maxRequest) g_maxRequest = bytes;
    if (g_watchSize != 0 && bytes == g_watchSize) g_sawWatched = true;
    if (bytes >= kBigRequestFloor) {
        if (g_bigRequestCount < kBigRequestSlots) g_bigRequests[g_bigRequestCount] = bytes;
        ++g_bigRequestCount;
    }
}

// `watchSize` is the one allocation whose presence proves the shipped decoder ran through
// this interposer; 0 means "no identity check in this window".
struct Window {
    explicit Window(size_t watchSize = 0) {
        g_maxRequest = 0;
        g_requestCount = 0;
        g_bigRequestCount = 0;
        g_watchSize = watchSize;
        g_sawWatched = false;
        g_armedHere = true;
        g_anyWindowOpen.store(true, std::memory_order_relaxed);
    }
    ~Window() {
        g_anyWindowOpen.store(false, std::memory_order_relaxed);
        g_armedHere = false;
        g_watchSize = 0;
    }
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    static size_t maxRequest() { return g_maxRequest; }
    static uint64_t requests() { return g_requestCount; }
    static bool sawWatched() { return g_sawWatched; }

    // Prints every request at or above 1 MiB that the window recorded, so a red identity
    // check carries the evidence needed to tell "stb allocates differently now" from "the
    // decoder never ran".
    static void printBigRequests(const char* label) {
        std::printf("image_bounds_probe: %s: %zu request(s) >= 1 MiB in the window:", label, g_bigRequestCount);
        const size_t shown = g_bigRequestCount < kBigRequestSlots ? g_bigRequestCount : kBigRequestSlots;
        for (size_t i = 0; i < shown; ++i) std::printf(" %zu", g_bigRequests[i]);
        if (g_bigRequestCount > shown) std::printf(" ... (%zu more)", g_bigRequestCount - shown);
        std::printf("\n");
    }
};

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
    // Colour type 2 (RGB). What is load-bearing here is the *dimension*, not the channel
    // count -- an earlier version of this comment claimed the opposite and was wrong.
    //
    // stb_image has a guard of its own at stb_image.h:5121, `(1 << 30) / x / img_n < y`,
    // evaluated on the channel count declared in the file. It is a product bound, so the
    // square side it starts refusing at moves with img_n: ~18918 at 3 channels, ~16384 at
    // 4. kOversizeSide is 16000, which clears BOTH ((1<<30)/16000/4 = 16777, and 16777 is
    // not less than 16000), so a colour type 6 header of these dimensions would reach
    // stbi_load's allocation and ask for the same 976.56 MiB. Either colour type works at
    // this size; type 2 only buys headroom if the fixture is ever raised.
    //
    // The constraint a maintainer must not break is on kOversizeSide, not on this byte:
    // past 16384 colour type 2 becomes mandatory, and past 18918 the counterfactual in
    // checkInstrumentSeesTheDecoder() measures zero at any non-paletted colour type and
    // the probe compares two zeroes. See the static_assert on kOversizeSide.
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

    // This is the first asset written on a fresh build tree, so it -- not the caller --
    // owns creating the directory. support/test_image.h does the same for its own writes;
    // relying on whichever of the two happens to run first is how a clean checkout ends up
    // failing with "could not write" and being read as an S4.9 regression.
    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);

    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    file.close();
    if (!file) throw std::runtime_error("image_bounds_probe: could not write " + path);
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
// stb_image's own `(1 << 30) / x / img_n < y` guard rejects it from the header at every
// non-paletted colour type -- 3 channels included ((1<<30)/23000/3 = 15561 < 23000) -- so
// no allocation ever happens and the counterfactual would measure nothing against nothing.
// 16000x16000 is inside stb's guard at 3 and 4 channels alike, and four times over
// StaticRenderer's cap, which is precisely the band where the cap is the only protection.
constexpr uint32_t kOversizeSide = 16000;
constexpr uint64_t kOversizeDecodedBytes =
    static_cast<uint64_t>(kOversizeSide) * kOversizeSide * 4;  // 1.0 GiB of RGBA

// The invariant the paragraph above rests on, as a build failure rather than as prose: at
// the 4 channels StaticRenderer requests, stb refuses a square image once
// (1 << 30) / side / 4 < side, i.e. from 16385 up. Raise kOversizeSide past that and
// checkInstrumentSeesTheDecoder()'s counterfactual silently becomes zero.
static_assert((1u << 30) / kOversizeSide / 4 >= kOversizeSide,
              "kOversizeSide must stay inside stb_image's own header guard, or the decode never "
              "allocates and the counterfactual this probe is measured against reads zero");

// Any allocation at or above this, inside the window, means pixel memory. It is a
// quarter of StaticRenderer's 256 MiB cap, so by construction *every* image the cap
// rejects must ask for more than this if it is decoded first, while nothing on the
// arming thread's path to the rejection (the file read, the exception's own string)
// comes close.
constexpr size_t kPixelMemoryFloor = 64 * kMiB;

// The legitimately-sized fixture, and the fingerprint of the one allocation that proves
// the shipped decoder ran through this interposer.
//
// stb decodes a PNG by first inflating the raw filtered scanlines: one filter byte per
// row, so (width * channels + 1) * height bytes in a single request (stb_image.h's
// stbi__create_png_image_raw / the guesssize passed to the zlib decode). That +1 per row
// is what makes the number attributable -- w*h*4 is exactly what the staging buffer is
// too, so it identifies nothing.
//
// 1021, not a round 1024, and the oddness is the point: (1021*4+1)*1021 = 4,170,785 is an
// odd number, which no size-class-rounded or alignment-padded driver allocation can equal
// by coincidence. The cost of choosing an odd side is nil (StaticRenderer's upload path
// makes no alignment assumption about width, and a non-power-of-two width is if anything
// the more searching case); the cost of a round one is a residual chance that the check
// goes green on something that is not the decoder at all, which is exactly the defect
// this shape exists to remove.
constexpr int kValidSide = 1021;
constexpr size_t kWatchedStbSize = (static_cast<size_t>(kValidSide) * 4 + 1) * kValidSide;

using wallpaperx::testing::check;
using wallpaperx::testing::failureCount;

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
// CONTROL: the shipped stb given its own allocator in static_renderer.cpp (`#define
//       STBI_MALLOC/STBI_REALLOC/STBI_FREE -> __libc_*`), so the decoder no longer calls
//       through this file's malloc -> "FAIL: ... a direct stbi_load of the oversize header
//       asks the allocator for pixel-sized memory", printed line "made 2 allocation
//       requests, the largest 0.0 MiB" against a decoded size of 976.6 MiB, on both the
//       NVIDIA and the lavapipe entry. Reverted. (An earlier round ran the cruder mutation
//       of deleting noteRequest() from the wrapper, with the same result; this one is
//       kept because it also distinguishes "stb is hidden" from "the instrument is dead".)
// COVERAGE-ASSERT: asserts this window saw a non-zero number of requests at all, so an
//       interposer that is optimised out or linked away fails here instead of measuring
//       zero and reading as a pass. It covers THIS window only -- Window's constructor
//       resets the counters, so each later window carries its own such assert.
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
//       memory it never asked for. Peak RSS is printed once at the end of the run, not
//       around this window: VmHWM is a process-lifetime high-water mark that cannot fall
//       and has already been raised by the 976 MiB stbi_load in the check above, so a
//       "before -> after" pair here would read as a per-window delta while being nothing
//       of the kind.
// CONTROL: static_renderer.cpp's `if (decodedBytes > kMaxDecodedBytes)` disabled ->
//       "FAIL: nothing pixel-sized is allocated on the way to the rejection" (printed
//       line: "17 allocation requests, the largest 976.56 MiB") and "FAIL: the rejection
//       names the declared dimensions" (it threw stb's "failed to load image ... not
//       enough pixels" instead). Two things from that run decided the shape above: "an
//       image over the decoded-size cap is rejected" stayed GREEN, and the process peak
//       RSS was 321196 KiB against 320916 KiB on the clean run -- i.e. an RSS oracle would
//       have graded that gigabyte as no growth at all. Reverted, re-measured green.
//       Second control, for the thread-confinement of the instrument: a test-owned thread
//       allocating 128 MiB inside this window. With the counters process-global (as they
//       were) that produced "FAIL: nothing pixel-sized is allocated on the way to the
//       rejection ... largest 128.00 MiB" -- a foreign allocation charged to production
//       code; thread-confined, the same run is green at 0.02 MiB. Both reverted.
// COVERAGE-ASSERT: `requests > 0`. This is the file's only upper-bound check, and an
//       upper bound cannot tell "the allocation did not happen" from "nothing was
//       measured". A future edit that leaves this region unarmed -- moving the Window
//       inside the try, wrapping the construction in a helper that arms its own -- would
//       otherwise read 0 and report the rejection as clean. Not a tautology: the path
//       opens the file and builds the exception's message, so it allocates today (12
//       requests on the NVIDIA entry, 13 on the lavapipe one). Its own control: the
//       Window moved below the construction it measures -> "FAIL: the allocation
//       instrument was live across the rejected construction", printed "0 allocation
//       requests, the largest 0.00 MiB", while "nothing pixel-sized is allocated on the
//       way to the rejection" passed on that same run -- which is the defect. Reverted.
void checkHeaderRejectionAllocatesNothing(wallpaperx::Device& device, VkRenderPass renderPass,
                                          const std::string& oversizePath) {
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

    std::printf("image_bounds_probe: rejecting the %ux%u header made %llu allocation requests, the "
                "largest %.2f MiB\n",
                kOversizeSide, kOversizeSide, static_cast<unsigned long long>(requests),
                static_cast<double>(maxRequest) / kMiB);
    std::printf("image_bounds_probe: rejection said: %s\n", threw ? message.c_str() : "(nothing -- it did not throw)");

    check(threw, "an image over the decoded-size cap is rejected");
    check(requests > 0,
          "the allocation instrument was live across the rejected construction (so a zero max-request "
          "reading is evidence rather than an unarmed window)");
    check(maxRequest < kPixelMemoryFloor,
          "nothing pixel-sized is allocated on the way to the rejection");
    check(message.find(std::to_string(kOversizeSide) + "x" + std::to_string(kOversizeSide)) != std::string::npos,
          "the rejection names the declared dimensions");
}

// GOAL: the measurement above is taken through the production call path, not only
//       through this file's own stbi_load. If StaticRenderer's allocations bypassed the
//       interposer -- a vendored stb copy, an STBI_MALLOC override, a different decoder
//       -- a zero reading from the rejected load would mean nothing.
// WHY-THIS-SHAPE: asserts on the IDENTITY of an allocation, not its magnitude. The
//       obvious form, `maxRequest >= decoded` (~4 MiB), asks how big the largest request
//       in the window was, which does not say who made it: the window spans the whole
//       constructor, createPipeline included, and how much the ICD allocates there is a
//       driver and cache property, not a fact about the decoder. Measured on this box, the
//       largest non-stb request in this window moved from 0.12 MiB to 0.48 MiB purely by
//       emptying the Mesa shader cache -- a driver whose pipeline compile crosses 4 MiB
//       (llvmpipe's LLVM JIT is the obvious candidate; the code-review pass measured
//       14.7 MiB on its host) turns the magnitude form green on a run where the decoder
//       was never observed at all, and the S4.9 check above would then report "nothing
//       pixel-sized allocated" for a renderer that decodes 976 MiB before rejecting.
//       kWatchedStbSize is stb's PNG raw-scanline buffer, (w*4+1)*h: one filter byte per
//       row makes it a size nothing on the Vulkan path has a reason to ask for -- unlike
//       w*h*4, which is exactly what the staging buffer is (it shows up in the printed
//       list below, one byte-count away from the watched size).
// CONTROL: `#define STBI_MALLOC/STBI_REALLOC/STBI_FREE -> __libc_*` added above
//       stb_image's implementation in static_renderer.cpp, hiding the shipped decoder from
//       the interposer while leaving everything else in the window intact -> "FAIL: the
//       instrument observes the shipped stb_image's own pixel allocation ...", printed
//       "exact 4170785-byte request seen: NO", 0 requests >= 1 MiB, on both the NVIDIA and
//       the lavapipe entry. Reverted; re-run clean -> "seen: YES", green. Honest caveat:
//       on this host that mutation would have reddened the magnitude form too (largest
//       0.11 MiB), so the false green above is argued from the driver-dependence measured
//       there, not reproduced here.
// COVERAGE-ASSERT: the printed list of every in-window request >= 1 MiB. It is what
//       tells a maintainer whether a red here is an stb upgrade that repacked its
//       buffers or a decoder that never ran -- the cost of an identity oracle is that it
//       is tied to stb's allocation shape, and this is what keeps that failure
//       diagnosable in one line rather than loud and mysterious.
void checkInstrumentSeesTheProductionPath(wallpaperx::Device& device, VkRenderPass renderPass,
                                          const std::string& validPath, int side) {
    const size_t decoded = static_cast<size_t>(side) * side * 4;
    size_t maxRequest = 0;
    bool sawWatched = false;
    bool constructed = false;
    std::string failure;
    {
        Window window(kWatchedStbSize);
        try {
            wallpaperx::StaticRenderer renderer(device, renderPass, validPath, WALLPAPERX_SHADER_DIR);
            constructed = true;
        } catch (const std::exception& e) {
            failure = e.what();
        }
        maxRequest = Window::maxRequest();
        sawWatched = Window::sawWatched();
    }
    std::printf("image_bounds_probe: loading the valid %dx%d image made a largest request of %.2f MiB "
                "(decoded size %.2f MiB); exact %zu-byte request seen: %s\n",
                side, side, static_cast<double>(maxRequest) / kMiB, static_cast<double>(decoded) / kMiB,
                kWatchedStbSize, sawWatched ? "YES" : "NO");
    Window::printBigRequests("valid load");

    check(constructed, "an image inside the cap still loads");
    if (!constructed) std::fprintf(stderr, "  it failed with: %s\n", failure.c_str());
    check(sawWatched,
          "the instrument observes the shipped stb_image's own pixel allocation inside StaticRenderer "
          "(so a zero reading from the rejected load is evidence rather than an artefact)");
}

}  // namespace

int main() {
#if !WALLPAPERX_ALLOCATION_INSTRUMENT
    // 77, not 0. Every check here rests on the allocation instrument, so without it this
    // binary can only produce an inconclusive result -- and an inconclusive result that
    // exits 0 is indistinguishable from a verified one.
    std::printf("image_bounds_probe: SKIPPED -- this build's runtime intercepts malloc/free itself (a "
                "sanitizer tier); the allocation instrument S4.9's oracle is built on cannot coexist "
                "with it. Nothing about the decoded-size bound was verified by this run.\n");
    return 77;
#else
    // The two "instrument" checks and the S4.9 check must run in one process: the
    // counterfactual is only a counterfactual if it is measured by the same instrument,
    // in the same build, on the same host.
    const bool ok = wallpaperx::testing::gradedRun(
        "image_bounds_probe", [&](wallpaperx::testing::ValidationGrade& grade) {
            try {
                // The assets share WALLPAPERX_TEST_IMAGE_DIR with the other probes; the
                // bounds_ prefix is what keeps them apart (see renderer/CMakeLists.txt).
                const std::string dir = std::string(WALLPAPERX_TEST_IMAGE_DIR);
                const std::string oversizePath =
                    writeOversizeHeaderPng(dir + "/bounds_oversize_header.png", kOversizeSide, kOversizeSide);
                const std::string validPath = wallpaperx::testing::writeSolidPng(
                    dir + "/bounds_valid.png", kValidSide, kValidSide, {40, 160, 220, 255});

                checkInstrumentSeesTheDecoder(oversizePath);

                wallpaperx::Device device(/*enableValidation=*/true);
                grade.watch(device);
                wallpaperx::OffscreenTargetManager targets(device);

                checkHeaderRejectionAllocatesNothing(device, targets.renderPass(), oversizePath);
                checkInstrumentSeesTheProductionPath(device, targets.renderPass(), validPath, kValidSide);
            } catch (const std::exception& e) {
                // A host with no usable ICD lands here. Reported as a failure of this
                // probe's setup, with the reason, rather than as a silent pass.
                wallpaperx::testing::fail(std::string("image_bounds_probe: could not run: ") + e.what());
            }
            // A process-lifetime peak, printed once and deliberately NOT per check: VmHWM
            // never falls, so a "before -> after" pair around one window reads as a delta
            // it cannot be. It is here as context for the header note's central claim --
            // a gigabyte was requested during this run and the peak barely moved -- and
            // nothing asserts on it.
            std::printf("image_bounds_probe: process peak RSS (VmHWM) for the whole run: %zu KiB "
                        "(monotonic and process-wide, not a per-check figure -- requests, not RSS, "
                        "are the oracle here)\n",
                        vmHighWaterKb());
            return failureCount() == 0;
        });

    if (!ok) {
        // Two distinct verdicts. gradedRun folds the validation grade into its result, so
        // a run whose every check passed can still be red because the GPU-side oracle
        // could not be made -- and printing "0 check(s) failed" for that would send the
        // reader looking for a check that does not exist.
        if (wallpaperx::testing::reportFailures("image_bounds_probe") == 0) {
            std::fprintf(stderr, "image_bounds_probe: every check passed, but the validation grade did not -- "
                                 "see the FAIL line above\n");
        }
        return 1;
    }
    std::printf("image_bounds_probe: all checks passed\n");
    return 0;
#endif  // WALLPAPERX_ALLOCATION_INSTRUMENT
}
