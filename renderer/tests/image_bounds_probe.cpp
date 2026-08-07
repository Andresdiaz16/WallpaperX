// S4.9 -- StaticRenderer's decode budget is applied from the image *header*, before any
// pixel memory is allocated, and it is applied at the edge the contract states.
//
// The whole difficulty of this goal is the oracle. "StaticRenderer threw" is satisfied
// just as well by a renderer that decodes a gigabyte first and complains afterwards,
// which is exactly the state this code was in before f1aa32c and exactly the state a
// future edit would return it to by moving the size test below stbi_load(). The verdict
// is not the property; the memory is. And since aded2e5 there is a *second* size gate,
// after the decode, which can reject the same file for a different reason -- so "it threw
// with the right dimensions in the message" no longer identifies which gate acted either.
// Every check below therefore names the gate it expects (see classifyRejection) rather
// than settling for "it was rejected".
//
// So this probe measures allocation *requests*, by interposing malloc/calloc/realloc in
// this executable. stb_image is compiled into the renderer's static library with the
// default STBI_MALLOC (= malloc), so its pixel-buffer allocation is a call into this
// file's symbol -- which means the request is observable without a single test-only line
// in any production file (X.6). fopen is interposed the same way, to count the opens the
// decode path makes and to redirect a chosen one of them, which is how the bytes the
// decoder actually consumed are identified.
//
// Requests, and not peak RSS, for a reason that was measured rather than assumed: with
// the header gate disabled, the rejected load asked for 976 MiB and this probe's VmHWM
// read 322,796 KiB against 323,360 KiB on the clean run -- a gigabyte requested, and the
// peak did not move outside run-to-run noise. Memory that is allocated and freed without
// ever being written to never enters RSS at all, so an RSS oracle would have graded that
// gigabyte as no growth. Peak RSS is printed once as context, never asserted -- see the
// annotation on checkHeaderRejectionAllocatesNothing().

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

#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

// --------------------------------------------------------------------------- //
// Interposition instruments
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
//
// The fopen interposer sits under the same switch. It has no sanitizer conflict of its
// own, but every check that uses it also needs the allocation instrument, and main()
// returns 77 before any of them run when this is 0 -- one switch, no second story.
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

// The identity half of the oracle: sizes only the decoder asks for (see kWatchedStbSize).
// Two slots, because a window has to answer "allocated for THIS image and never for THAT
// one" -- the absent half is what says which file's bytes actually reached the decoder.
constexpr size_t kWatchSlots = 2;
thread_local size_t g_watchSize[kWatchSlots] = {};
thread_local bool g_sawWatched[kWatchSlots] = {};

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
    for (size_t i = 0; i < kWatchSlots; ++i) {
        if (g_watchSize[i] != 0 && bytes == g_watchSize[i]) g_sawWatched[i] = true;
    }
    if (bytes >= kBigRequestFloor) {
        if (g_bigRequestCount < kBigRequestSlots) g_bigRequests[g_bigRequestCount] = bytes;
        ++g_bigRequestCount;
    }
}

// The watched sizes are the allocations whose presence (or absence) identifies which
// image the shipped decoder ran on; 0 means "this slot is not watched in this window".
struct Window {
    explicit Window(size_t watchSize = 0, size_t secondWatchSize = 0) {
        g_maxRequest = 0;
        g_requestCount = 0;
        g_bigRequestCount = 0;
        g_watchSize[0] = watchSize;
        g_watchSize[1] = secondWatchSize;
        g_sawWatched[0] = false;
        g_sawWatched[1] = false;
        g_armedHere = true;
        g_anyWindowOpen.store(true, std::memory_order_relaxed);
    }
    ~Window() {
        g_anyWindowOpen.store(false, std::memory_order_relaxed);
        g_armedHere = false;
        g_watchSize[0] = 0;
        g_watchSize[1] = 0;
    }
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    static size_t maxRequest() { return g_maxRequest; }
    static uint64_t requests() { return g_requestCount; }
    static bool sawWatched(size_t slot = 0) { return g_sawWatched[slot]; }

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

// The file-open half of the instrument. It does two jobs: it COUNTS the opens production
// makes of one path, and it can redirect a chosen subset of them elsewhere.
//
// The count is a property in its own right -- the whole decode path shares ONE open -- and
// it is also what aims the redirect, since an open is identified by index and nothing else.
//
// `redirectMaxCount` is not a convenience: redirecting every open from index 1 shows the
// replacement to a two-open implementation as well, so both shapes pass and the check goes
// green on exactly what it exists to refuse. Capping at one is what splits them.
struct FileSwapPlan {
    const char* watchedPath = nullptr;      // the exact string production passes to fopen
    const char* replacementPath = nullptr;  // what to open instead
    int redirectFromOpen = 0;               // 1-based index of the first open to redirect; 0 = observe only
    int redirectMaxCount = 0;               // cap on redirects performed; 0 = no cap
    int opens = 0;                          // opens of watchedPath seen on this thread
    int redirects = 0;
};

// Thread-local for the same reason the allocation counters are: an ICD worker opening a
// shader cache file must not be able to advance this plan's open counter. Constant-
// initialised trivially-destructible pointer, so no __cxa_thread_atexit registration.
thread_local FileSwapPlan* g_swapPlan = nullptr;
std::atomic<int> g_swapPlansArmed{0};

struct ArmedSwap {
    explicit ArmedSwap(FileSwapPlan& plan) {
        g_swapPlan = &plan;
        g_swapPlansArmed.fetch_add(1, std::memory_order_relaxed);
    }
    ~ArmedSwap() {
        g_swapPlansArmed.fetch_sub(1, std::memory_order_relaxed);
        g_swapPlan = nullptr;
    }
    ArmedSwap(const ArmedSwap&) = delete;
    ArmedSwap& operator=(const ArmedSwap&) = delete;
};

}  // namespace

// glibc's own entry points, so no dlsym(RTLD_NEXT) bootstrap is needed for the allocator
// (dlsym itself allocates, which makes that idiom recursive in exactly this situation).
// free() is deliberately NOT interposed: __libc_malloc and malloc are the same allocator,
// so glibc's free handles these pointers, and every uninterposed allocation in the process
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

// fopen has no __libc_ alias, so this one does need dlsym -- which is safe here in a way
// it would not be inside malloc: dlsym allocates, but it does not open files, so there is
// no recursion to bootstrap around. Resolution is idempotent, so the relaxed racing store
// can only ever write the same value twice.
//
// Unarmed, this is one relaxed load and a tail call. It has to be: the Vulkan loader and
// the ICD open files on threads this probe does not own.
FILE* fopen(const char* path, const char* mode) {
    static std::atomic<void*> realFopen{nullptr};
    void* real = realFopen.load(std::memory_order_relaxed);
    if (real == nullptr) {
        real = dlsym(RTLD_NEXT, "fopen");
        realFopen.store(real, std::memory_order_relaxed);
    }
    auto* realFn = reinterpret_cast<FILE* (*)(const char*, const char*)>(real);
    if (realFn == nullptr) return nullptr;  // unresolvable libc; every open fails loudly

    if (g_swapPlansArmed.load(std::memory_order_relaxed) != 0 && g_swapPlan != nullptr &&
        path != nullptr && g_swapPlan->watchedPath != nullptr &&
        std::strcmp(path, g_swapPlan->watchedPath) == 0) {
        ++g_swapPlan->opens;
        const bool inRange =
            g_swapPlan->redirectFromOpen > 0 && g_swapPlan->opens >= g_swapPlan->redirectFromOpen;
        const bool underCap =
            g_swapPlan->redirectMaxCount == 0 || g_swapPlan->redirects < g_swapPlan->redirectMaxCount;
        if (inRange && underCap) {
            ++g_swapPlan->redirects;
            return realFn(g_swapPlan->replacementPath, mode);
        }
    }
    return realFn(path, mode);
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

// A PNG whose IHDR *declares* width x height at `bitDepth` but whose pixel data is a stub.
//
// The point of this file is that the declared size is enormous while the file is a few
// dozen bytes: stbi_info() and stbi_is_16_bit() answer from the IHDR alone, so the header
// gate can act without touching pixel memory, while anything that reaches stbi_load()
// asks the allocator for pixel-sized memory first and then fails on the truncated stream.
// Generating a genuinely 16000x16000 PNG would need a gigabyte in this process just to
// write it, which would wreck the very measurement this probe exists to take.
//
// bitDepth is a parameter because it is half the contract: stbi_is_16_bit() selects which
// peak factor StaticRenderer charges, and 8 and 16 are the only two values that matters
// for. Colour type stays 2 (RGB) -- see the note on kOversizeSide for why the *dimension*
// is the load-bearing part and the channel count is not.
std::string writeHeaderOnlyPng(const std::string& path, uint32_t width, uint32_t height, int bitDepth) {
    std::vector<unsigned char> png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

    std::vector<unsigned char> ihdr;
    appendBe32(ihdr, width);
    appendBe32(ihdr, height);
    ihdr.push_back(static_cast<unsigned char>(bitDepth));
    // Colour type 2 (RGB). What is load-bearing here is the *dimension*, not the channel
    // count -- an earlier version of this comment claimed the opposite and was wrong.
    //
    // stb_image has a guard of its own at stb_image.h:5121, `(1 << 30) / x / img_n < y`,
    // evaluated on the channel count declared in the file and NOT on the bit depth. It is
    // a product bound, so the square side it starts refusing at moves with img_n: ~18918
    // at 3 channels, ~16384 at 4. Every fixture below clears it at 3 and at 4 channels
    // alike, so either colour type would work; type 2 only buys headroom if a fixture is
    // ever enlarged. The static_asserts near the fixture constants pin this.
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
// Which gate rejected, and why the test has to care
// --------------------------------------------------------------------------- //

// StaticRenderer::loadAndUploadTexture has several distinct rejection paths, and more than
// one of them refuses the same oversize file: the header gate refuses it before the decode,
// and with that gate deleted stbi_load refuses it as well -- after asking for the gigabyte.
// Both messages name dimensions, so "it threw, and the message contains 16000x16000" cannot
// tell them apart, and telling them apart is the entire S4.9 property.
//
// Matching on message text couples this file to production's wording, which is a real
// cost and is accepted deliberately: the gate that acted is not observable any other way
// from outside, and the alternative -- asserting only that *something* threw -- is the
// vacuity this probe exists to prevent. Unrecognised is a distinct verdict rather than a
// fallback, so a reworded message fails loudly instead of quietly reclassifying.
enum class Gate {
    None,                   // did not throw at all
    CannotOpen,             // fopen() of the image failed
    NotDecodable,           // the header probe refused the file
    NonPositiveDimensions,  // header parsed, w or h <= 0
    HeaderPeakBudget,       // the pre-decode peak estimate exceeded the budget
    DecodeFailed,           // stbi_load() returned null
    // The decode disagreed with the header. No fixture here can produce it any more (see
    // checkOneOpenFeedsProbeAndDecode) -- it is kept so that if stb's two parsers ever do
    // disagree, the result is *classified* rather than landing in Unrecognised.
    PostLoadMismatch,
    Unrecognised,           // threw something none of the above explains
};

const char* gateName(Gate gate) {
    switch (gate) {
        case Gate::None: return "no rejection";
        case Gate::CannotOpen: return "cannot-open";
        case Gate::NotDecodable: return "not-a-decodable-image";
        case Gate::NonPositiveDimensions: return "non-positive-dimensions";
        case Gate::HeaderPeakBudget: return "header peak-budget gate";
        case Gate::DecodeFailed: return "stbi_load failed";
        case Gate::PostLoadMismatch: return "header-vs-decoder disagreement";
        case Gate::Unrecognised: return "UNRECOGNISED rejection";
    }
    return "UNRECOGNISED rejection";
}

Gate classifyRejection(bool threw, const std::string& message) {
    if (!threw) return Gate::None;
    if (message.find("estimated peak") != std::string::npos) return Gate::HeaderPeakBudget;
    if (message.find("header and decoder disagree") != std::string::npos) return Gate::PostLoadMismatch;
    if (message.find("failed to load image") != std::string::npos) return Gate::DecodeFailed;
    if (message.find("cannot open image") != std::string::npos) return Gate::CannotOpen;
    if (message.find("not a decodable image") != std::string::npos) return Gate::NotDecodable;
    if (message.find("non-positive dimensions") != std::string::npos) return Gate::NonPositiveDimensions;
    return Gate::Unrecognised;
}

// One construction, measured. Everything below drives StaticRenderer through this so the
// window is opened and closed identically in every case.
struct Attempt {
    Gate gate = Gate::None;
    std::string message;
    bool constructed = false;
    size_t maxRequest = 0;
    uint64_t requests = 0;
    bool sawWatched[2] = {false, false};
};

Attempt attemptLoad(wallpaperx::Device& device, VkRenderPass renderPass, const std::string& path,
                    size_t watchSize = 0, size_t secondWatchSize = 0) {
    Attempt out;
    bool threw = false;
    {
        Window window(watchSize, secondWatchSize);
        try {
            wallpaperx::StaticRenderer renderer(device, renderPass, path, WALLPAPERX_SHADER_DIR);
            out.constructed = true;
        } catch (const std::exception& e) {
            threw = true;
            out.message = e.what();
        }
        out.maxRequest = Window::maxRequest();
        out.requests = Window::requests();
        out.sawWatched[0] = Window::sawWatched(0);
        out.sawWatched[1] = Window::sawWatched(1);
    }
    out.gate = classifyRejection(threw, out.message);
    return out;
}

// --------------------------------------------------------------------------- //
// The contract, restated -- and why it is restated rather than shared
// --------------------------------------------------------------------------- //

// static_renderer.cpp's budget and factors are file-local constexprs in a .cpp; there is
// no header to include and adding one for a test's benefit is forbidden (X.6). So they are
// mirrored here, and the mirror is load-bearing rather than redundant: the fixtures below
// are placed exactly ON the edges these numbers imply, so a change to either constant in
// production -- and only there -- moves the real edge off the fixtures and reddens this
// file. Verified by mutation: raising kPeakFactor16Bit to 7 reddens the 16-bit at-budget
// case; changing `>` to `>=` reddens both at-budget cases.
constexpr uint64_t kBudgetBytes = 768ull * 1024 * 1024;
constexpr uint64_t kFactor8Bit = 4;
constexpr uint64_t kFactor16Bit = 6;

constexpr uint64_t peakEstimate(uint32_t width, uint32_t height, int bitDepth) {
    return static_cast<uint64_t>(width) * height * 4 * (bitDepth == 16 ? kFactor16Bit : kFactor8Bit);
}

// The pre-decode gate: strict `>`, so a file landing exactly on the budget is admitted.
constexpr bool rejectedFromHeader(uint32_t width, uint32_t height, int bitDepth) {
    return peakEstimate(width, height, bitDepth) > kBudgetBytes;
}

// stb's first pixel-sized malloc for a PNG: the inflate target for the raw filtered
// scanlines, one filter byte per row. Used to place kPixelMemoryFloor.
constexpr uint64_t pngRawScanlineBytes(uint64_t width, uint64_t height, uint64_t channels,
                                       uint64_t bitDepth) {
    return (width * channels * bitDepth / 8 + 1) * height;
}

// --------------------------------------------------------------------------- //
// Fixtures
// --------------------------------------------------------------------------- //

// The declared size of the oversize header: 16000x16000, the case static_renderer.cpp's
// own comment records ("a 995 KB PNG of 16000x16000 decodes to 976 MiB", OOM-killed under
// a 1.2 GB limit). GOALS.md explains why it is not something larger: stb_image's own
// `(1 << 30) / x / img_n < y` guard refuses ~23000x23000 from the header at every
// non-paletted colour type, so no allocation ever happens there and the counterfactual
// would measure nothing against nothing. 16000x16000 is inside stb's guard at 3 and 4
// channels alike, and ~5x over the peak budget, which is the band where the budget is the
// only protection.
constexpr uint32_t kOversizeSide = 16000;
constexpr uint64_t kOversizeDecodedBytes =
    static_cast<uint64_t>(kOversizeSide) * kOversizeSide * 4;  // 976.6 MiB of RGBA

// The invariant the paragraph above rests on, as a build failure rather than as prose: at
// the 4 channels StaticRenderer requests, stb refuses a square image once
// (1 << 30) / side / 4 < side, i.e. from 16385 up. Raise kOversizeSide past that and
// checkInstrumentSeesTheDecoder()'s counterfactual silently becomes zero.
static_assert((1u << 30) / kOversizeSide / 4 >= kOversizeSide,
              "kOversizeSide must stay inside stb_image's own header guard, or the decode never "
              "allocates and the counterfactual this probe is measured against reads zero");
static_assert(rejectedFromHeader(kOversizeSide, kOversizeSide, 8),
              "the oversize fixture must be rejected by the *header* gate, not merely rejected");

// The four edge fixtures. Each pair straddles the exact byte at which the budget starts
// binding, one image row apart, which is what makes a change of comparison or of factor
// detectable rather than merely plausible.
//
//   8-bit:  w*h*4*4 == budget at 8192x6144 (50,331,648 px). 6145 rows is the first over.
//   16-bit: w*h*4*6 == budget at 8192x4096 (33,554,432 px). 4097 rows is the first over.
//
// 8192x4097 appears twice, at both depths: same dimensions, one IHDR byte apart, opposite
// verdicts. That is the whole 16-bit claim in one comparison.
constexpr uint32_t kEdgeWidth = 8192;
constexpr uint32_t kAt8Height = 6144;
constexpr uint32_t kOver8Height = 6145;
constexpr uint32_t kAt16Height = 4096;
constexpr uint32_t kOver16Height = 4097;

static_assert(peakEstimate(kEdgeWidth, kAt8Height, 8) == kBudgetBytes,
              "the 8-bit at-budget fixture must land EXACTLY on the budget; one byte off and a "
              "regression from `>` to `>=` (or back) is no longer straddled by anything here");
static_assert(peakEstimate(kEdgeWidth, kAt16Height, 16) == kBudgetBytes,
              "same, for the 16-bit factor");
static_assert(!rejectedFromHeader(kEdgeWidth, kAt8Height, 8));
static_assert(rejectedFromHeader(kEdgeWidth, kOver8Height, 8));
static_assert(!rejectedFromHeader(kEdgeWidth, kAt16Height, 16));
static_assert(rejectedFromHeader(kEdgeWidth, kOver16Height, 16));
static_assert(!rejectedFromHeader(kEdgeWidth, kOver16Height, 8),
              "8192x4097 must be admitted at 8 bits and refused at 16, or the same-dimensions "
              "pair proves nothing about which factor was charged");
// Every edge fixture must clear stb's own IHDR product guard at its declared 3 channels,
// or stbi_info() refuses it and the case measures stb's limit instead of ours.
static_assert((1u << 30) / kEdgeWidth / 3 >= kOver8Height, "edge fixture outside stb's IHDR guard");

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

// The interposition pair: two images of different size, each identifiable from its
// allocation footprint alone, both far inside the budget so the load completes.
//
// Odd sides, deliberately: stb's raw-scanline buffer is (width * channels + 1) * height,
// which is odd for an odd side, so no size-class rounding or alignment padding elsewhere
// in the process can produce it by accident. writeSolidPng emits 4 channels.
constexpr int kSwapSmallSide = 257;
constexpr int kSwapGrownSide = 511;
constexpr size_t kSmallScanlineBytes =
    static_cast<size_t>(pngRawScanlineBytes(kSwapSmallSide, kSwapSmallSide, 4, 8));
constexpr size_t kGrownScanlineBytes =
    static_cast<size_t>(pngRawScanlineBytes(kSwapGrownSide, kSwapGrownSide, 4, 8));
static_assert(!rejectedFromHeader(kSwapSmallSide, kSwapSmallSide, 8),
              "both interposition fixtures must be admitted by the header gate, or the load never "
              "reaches the decode whose bytes these checks identify");
static_assert(!rejectedFromHeader(kSwapGrownSide, kSwapGrownSide, 8));
static_assert(kSmallScanlineBytes != kGrownScanlineBytes,
              "the two fixtures must be distinguishable by allocation size, or 'the decoder ran on "
              "the replacement and not the original' is not a statement about anything");
static_assert(kSmallScanlineBytes % 2 == 1 && kGrownScanlineBytes % 2 == 1,
              "an even watched size could be matched by a rounded or padded allocation from the "
              "driver, which would turn the identity oracle into a coincidence detector");

// The opens StaticRenderer::loadAndUploadTexture makes of the image across the whole load:
// ONE, shared by stbi_info_from_file, stbi_is_16_bit_from_file and stbi_load_from_file.
//
// Not bookkeeping -- it is the property, asserted by checkDecodePathOpensTheImageOnce().
// A second open is a window in which a 16-bit file of identical geometry replaces the
// 8-bit one whose header was charged, on the one axis the post-decode dimension guard
// cannot see. It also aims the redirect, which identifies an open by index and nothing else.
constexpr int kExpectedOpens = 1;

// Any allocation at or above this, inside a window, means pixel memory.
//
// It is placed between two measured quantities, not derived from a cap constant (the
// previous justification -- "a quarter of StaticRenderer's 256 MiB cap" -- outlived the
// constant it referred to by one commit):
//   - Below it must sit everything the rejection paths themselves allocate: the file
//     read, std::to_string, the exception's message. Measured at ~0.02 MiB.
//   - Above it must sit the first pixel-sized malloc of every fixture this probe expects
//     to be either admitted-then-failed or (counterfactually) decoded, or the checks that
//     assert on either side of the floor stop being able to distinguish anything. That
//     minimum is 96 MiB, at 8192x4097x3x8bit, and the static_assert below enforces it.
constexpr size_t kPixelMemoryFloor = 64 * kMiB;

constexpr uint64_t kSmallestFixturePixelRequest = std::min({
    pngRawScanlineBytes(kOversizeSide, kOversizeSide, 3, 8),
    pngRawScanlineBytes(kEdgeWidth, kAt8Height, 3, 8),
    pngRawScanlineBytes(kEdgeWidth, kOver8Height, 3, 8),
    pngRawScanlineBytes(kEdgeWidth, kAt16Height, 3, 16),
    pngRawScanlineBytes(kEdgeWidth, kOver16Height, 3, 16),
    pngRawScanlineBytes(kEdgeWidth, kOver16Height, 3, 8),
});
static_assert(kPixelMemoryFloor < kSmallestFixturePixelRequest,
              "kPixelMemoryFloor must stay below the smallest pixel-sized request any fixture "
              "here can make, or 'nothing pixel-sized was allocated' stops being falsifiable");

using wallpaperx::testing::check;
using wallpaperx::testing::failureCount;

// --------------------------------------------------------------------------- //
// Checks
// --------------------------------------------------------------------------- //

// GOAL: the instrument is real -- an allocation request made by the *shipped* stb_image
//       build, for the pixels of this very file, is seen and sized by this probe. Without
//       it, "no large allocation was observed" is equally consistent with an interposer
//       that was never called, and the S4.9 checks below would be vacuous.
// WHY-THIS-SHAPE: calls stbi_load() on the same oversize file directly, so the number it
//       records is the counterfactual the real check is measured against -- what a
//       decode-first StaticRenderer would have asked for, measured on this host rather
//       than assumed. A simpler smoke test ("malloc(1<<20) is observed") would prove the
//       symbol is interposed but not that *stb's* allocations route through it, which is
//       the only thing that matters here.
// CONTROL: `#define STBI_MALLOC/STBI_REALLOC/STBI_FREE -> __libc_*` above stb_image's
//       implementation in static_renderer.cpp, so the shipped decoder no longer calls
//       through this file's malloc -> "FAIL: a direct stbi_load of the oversize header
//       asks the allocator for pixel-sized memory", printed line "made 2 allocation
//       requests, the largest 0.0 MiB" against a decoded size of 976.6 MiB. Reverted;
//       re-run green at 976.6 MiB.
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

// GOAL: S4.9 -- an image whose declared dimensions exceed the budget is rejected BY THE
//       HEADER GATE: no pixel-sized allocation is ever requested, and the message names
//       the declared dimensions.
// WHY-THIS-SHAPE: asserts on the largest allocation *request* inside the constructor, and
//       on which gate fired -- not on peak RSS and not on the exception alone. "It threw"
//       is satisfied by a renderer that decodes 976 MiB and complains afterwards, and
//       since aded2e5 that is no longer even hypothetical: delete the header gate and the
//       post-load gate rejects this same file, naming dimensions, after spending the
//       gigabyte. RSS cannot be the oracle either -- a buffer allocated and freed without
//       being written never enters RSS, so an RSS check reports "no growth" for exactly
//       the decode-first case it exists to catch. The request is what is unavoidable:
//       nothing can decode into memory it never asked for.
// CONTROL: static_renderer.cpp's `peakBytes > kMaxPeakDecodeBytes` throw disabled -> three
//       reds here -- "the oversize header is refused by the header gate, before the
//       decode", "nothing pixel-sized is allocated on the way to the rejection" (printed
//       "21 allocation requests, the largest 976.56 MiB"), and "the rejection names the
//       declared dimensions". Note what stayed GREEN in that same run: "an image over the
//       decoded-size budget is rejected" -- the verdict-only assertion, passing for a
//       renderer that had just spent 976 MiB. Reverted, re-measured green at 0.02 MiB.
// COVERAGE-ASSERT: `requests > 0`. This is an upper-bound check, and an upper bound cannot
//       tell "the allocation did not happen" from "nothing was measured". Not a tautology:
//       the path opens the file and builds the exception's message, so it allocates today
//       (15-18 requests, and the exact number is a driver/libstdc++ detail -- which is why
//       the assertion is `> 0` and not a figure). Its own control: the Window moved below the
//       construction it measures -> "FAIL: the allocation instrument was live across the
//       rejected construction", printed "0 allocation requests, the largest 0.00 MiB",
//       while "nothing pixel-sized is allocated on the way to the rejection" passed on
//       that same run -- which is the defect. Reverted.
void checkHeaderRejectionAllocatesNothing(wallpaperx::Device& device, VkRenderPass renderPass,
                                          const std::string& oversizePath) {
    const Attempt attempt = attemptLoad(device, renderPass, oversizePath);

    std::printf("image_bounds_probe: rejecting the %ux%u header made %llu allocation requests, the "
                "largest %.2f MiB\n",
                kOversizeSide, kOversizeSide, static_cast<unsigned long long>(attempt.requests),
                static_cast<double>(attempt.maxRequest) / kMiB);
    std::printf("image_bounds_probe: rejection said [%s]: %s\n", gateName(attempt.gate),
                attempt.constructed ? "(nothing -- it constructed)" : attempt.message.c_str());

    check(!attempt.constructed, "an image over the decoded-size budget is rejected");
    check(attempt.gate == Gate::HeaderPeakBudget,
          "the oversize header is refused by the header gate, before the decode");
    check(attempt.requests > 0,
          "the allocation instrument was live across the rejected construction (so a zero max-request "
          "reading is evidence rather than an unarmed window)");
    check(attempt.maxRequest < kPixelMemoryFloor,
          "nothing pixel-sized is allocated on the way to the rejection");
    check(attempt.message.find(std::to_string(kOversizeSide) + "x" + std::to_string(kOversizeSide)) !=
              std::string::npos,
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
//       was never observed at all. kWatchedStbSize is stb's PNG raw-scanline buffer,
//       (w*4+1)*h: one filter byte per row makes it a size nothing on the Vulkan path has
//       a reason to ask for -- unlike w*h*4, which is exactly what the staging buffer is
//       (it shows up in the printed list below, one byte-count away from the watched size).
// CONTROL: the same `#define STBI_MALLOC -> __libc_*` mutation as above, which hides the
//       shipped decoder from the interposer while leaving everything else in the window
//       intact -> "FAIL: the instrument observes the shipped stb_image's own pixel
//       allocation inside StaticRenderer ...", printed "exact 4170785-byte request seen:
//       NO", 0 requests >= 1 MiB. Reverted; re-run clean -> "seen: YES", green.
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

    check(constructed, "an image inside the budget still loads");
    if (!constructed) std::fprintf(stderr, "  it failed with: %s\n", failure.c_str());
    check(sawWatched,
          "the instrument observes the shipped stb_image's own pixel allocation inside StaticRenderer "
          "(so a zero reading from the rejected load is evidence rather than an artefact)");
}

struct EdgeCase {
    const char* label;
    uint32_t width;
    uint32_t height;
    int bitDepth;
};

// GOAL: the header gate acts at the byte the contract states, on both sides -- an image
//       whose estimated peak is exactly the budget is ADMITTED, one image row more is
//       refused -- and a 16-bit image is charged the larger factor, so that 8192x4097 is
//       admitted at 8 bits and refused at 16.
// WHY-THIS-SHAPE: four fixtures on two edges rather than one "obviously too big" image.
//       An oversize-only check passes for any budget and any factor -- 16000x16000 is
//       refused whether the factor is 1 or 6 -- so it cannot see a comparison flipped to
//       `>=` (the shape of the bug that let a 16-bit 8192x8192 through the old cap, which
//       computed to exactly 268435456 and was therefore not `>` 256 MiB), nor a 16-bit
//       factor quietly equal to the 8-bit one. Each pair is one image ROW apart, so the
//       fixtures straddle the exact byte. The admitted cases are graded DecodeFailed and
//       required to have made a pixel-sized request: that is what distinguishes "the gate
//       admitted it" from "it was rejected somewhere else with a message we did not
//       recognise", which an `!threw`-style assertion cannot do for a header-only stub.
// CONTROL: (a) `peakBytes > kMaxPeakDecodeBytes` changed to `>=` -> "FAIL: 8-bit, exactly
//       at the budget: reaches the decoder rather than the header gate (saw: header
//       peak-budget gate)" plus its allocation half, and the same pair for the 16-bit
//       at-budget case; the two over-budget cases stayed green, which is the point.
//       (b) kPeakFactor16Bit changed 6 -> 4 -> "FAIL: 16-bit, one row over the budget:
//       refused by the header gate (saw: stbi_load failed)", plus "no pixel-sized
//       allocation before the refusal" and "the refusal names the bit depth it charged
//       for", while every 8-bit case stayed green. (c) 6 -> 7 -> only the 16-bit
//       at-budget case reddens. All three reverted, re-measured green.
// COVERAGE-ASSERT: the number of cases actually run is asserted against the table size,
//       and each case asserts its own window saw requests at all. A future edit that
//       `continue`s past a case, or a fixture that fails to be written, then fails here
//       instead of quietly shrinking the straddle to nothing. Controlled with
//       `if (casesRun == 2) break;` -> "FAIL: every budget-edge case ran (2 of 5)".
void checkHeaderGateEdges(wallpaperx::Device& device, VkRenderPass renderPass, const std::string& dir) {
    const EdgeCase cases[] = {
        {"8-bit, exactly at the budget", kEdgeWidth, kAt8Height, 8},
        {"8-bit, one row over the budget", kEdgeWidth, kOver8Height, 8},
        {"16-bit, exactly at the budget", kEdgeWidth, kAt16Height, 16},
        {"16-bit, one row over the budget", kEdgeWidth, kOver16Height, 16},
        {"8-bit twin of the 16-bit over-budget case", kEdgeWidth, kOver16Height, 8},
    };
    constexpr size_t kCaseCount = sizeof(cases) / sizeof(cases[0]);

    size_t casesRun = 0;
    for (const EdgeCase& c : cases) {
        const std::string dims = std::to_string(c.width) + "x" + std::to_string(c.height);
        const std::string path =
            dir + "/bounds_edge_" + dims + "_" + std::to_string(c.bitDepth) + "bit.png";
        std::string label(c.label);

        // A fixture that cannot be written is this probe's fault, not the renderer's, and
        // must not print as an S4.9 failure. Reported, and the case is not counted -- the
        // count assertion at the end then fails too, so it cannot pass unnoticed.
        try {
            writeHeaderOnlyPng(path, c.width, c.height, c.bitDepth);
        } catch (const std::exception& e) {
            wallpaperx::testing::fail("image_bounds_probe: could not write the fixture for " + label +
                                      ": " + e.what());
            continue;
        }

        const Attempt attempt = attemptLoad(device, renderPass, path);
        const bool expectHeaderRefusal = rejectedFromHeader(c.width, c.height, c.bitDepth);

        std::printf("image_bounds_probe: %s (%s, %d-bit, estimated peak %llu MiB vs %llu MiB budget): "
                    "%s, largest request %.2f MiB over %llu requests\n",
                    label.c_str(), dims.c_str(), c.bitDepth,
                    static_cast<unsigned long long>(peakEstimate(c.width, c.height, c.bitDepth) / kMiB),
                    static_cast<unsigned long long>(kBudgetBytes / kMiB), gateName(attempt.gate),
                    static_cast<double>(attempt.maxRequest) / kMiB,
                    static_cast<unsigned long long>(attempt.requests));
        if (!attempt.message.empty()) {
            std::printf("image_bounds_probe:   said: %s\n", attempt.message.c_str());
        }

        check(attempt.requests > 0, label + ": the allocation instrument was live across this case");

        if (expectHeaderRefusal) {
            check(attempt.gate == Gate::HeaderPeakBudget,
                  label + ": refused by the header gate (saw: " + gateName(attempt.gate) + ")");
            check(attempt.maxRequest < kPixelMemoryFloor,
                  label + ": no pixel-sized allocation before the refusal");
            check(attempt.message.find(dims) != std::string::npos,
                  label + ": the refusal names the declared dimensions");
            check(attempt.message.find(std::to_string(c.bitDepth) + "-bit") != std::string::npos,
                  label + ": the refusal names the bit depth it charged for");
        } else {
            // Header-only stubs, so the *only* way past the gate is into stbi_load, which
            // then fails on the truncated stream. Both halves matter: the gate verdict
            // says the budget admitted it, the allocation says the decoder really started.
            check(attempt.gate == Gate::DecodeFailed,
                  label + ": reaches the decoder rather than the header gate (saw: " +
                      gateName(attempt.gate) + ")");
            check(attempt.maxRequest >= kPixelMemoryFloor,
                  label + ": the admitted image really did reach a pixel-sized allocation");
        }
        ++casesRun;
    }

    check(casesRun == kCaseCount,
          "every budget-edge case ran (" + std::to_string(casesRun) + " of " +
              std::to_string(kCaseCount) + ")");
}

struct SwapResult {
    Attempt attempt;
    int opens = 0;
    int redirects = 0;
};

// One load with the fopen interposer armed. `redirectFromOpen` is 1-based, 0 = observe
// only; `redirectMaxCount` caps the redirects performed, 0 = no cap. The two watched sizes
// go to the allocation window, so the caller can ask which image stb allocated for.
SwapResult loadWithRedirect(wallpaperx::Device& device, VkRenderPass renderPass,
                            const std::string& path, const std::string& replacement,
                            int redirectFromOpen, int redirectMaxCount, size_t expectDecoded,
                            size_t expectAbsent) {
    FileSwapPlan plan;
    plan.watchedPath = path.c_str();
    plan.replacementPath = replacement.c_str();
    plan.redirectFromOpen = redirectFromOpen;
    plan.redirectMaxCount = redirectMaxCount;

    SwapResult result;
    {
        ArmedSwap armed(plan);
        result.attempt = attemptLoad(device, renderPass, path, expectDecoded, expectAbsent);
    }
    result.opens = plan.opens;
    result.redirects = plan.redirects;
    return result;
}

// GOAL: probes and decode read the image through ONE open, so the bytes the budget verdict
//       was computed on cannot be replaced before they are decoded. False at 2 (a decode
//       that reopens by path) or 3 (probes via stbi_info(path)/stbi_is_16_bit(path)).
// WHY-THIS-SHAPE: exactly 1, not "at most 2". Every open after the first admits a 16-bit
//       file of identical geometry in place of the 8-bit one that was charged, and the
//       post-decode guard compares dimensions -- which such a substitute matches -- so that
//       axis is undetectable there. Measured on a load that SUCCEEDS: on a rejected one, a
//       count of 1 could mean it gave up before the second.
// CONTROL: production's `stbi_load_from_file(probe.get(), ...)` -> `stbi_load(imagePath
//       .c_str(), ...)` gives "(2 opens of the image, expected 1)". Reverted, green at 1.
// COVERAGE-ASSERT: the load must have constructed -- controlled by pointing this at a
//       nonexistent path, which still counts 1 open and reddens only that assert.
void checkDecodePathOpensTheImageOnce(wallpaperx::Device& device, VkRenderPass renderPass,
                                      const std::string& validPath) {
    FileSwapPlan plan;
    plan.watchedPath = validPath.c_str();
    plan.replacementPath = nullptr;
    plan.redirectFromOpen = 0;  // observe only

    Attempt attempt;
    {
        ArmedSwap armed(plan);
        attempt = attemptLoad(device, renderPass, validPath);
    }

    std::printf("image_bounds_probe: a successful load opened the image %d time(s) (expected %d)\n",
                plan.opens, kExpectedOpens);

    check(attempt.constructed,
          "the open-counting fixture loads at all (so the count is of a complete decode path)");
    if (!attempt.constructed) {
        std::fprintf(stderr, "  it failed with: %s\n", attempt.message.c_str());
    }
    check(plan.opens == kExpectedOpens,
          "the whole decode path reads the image through one open, so no bytes can be substituted "
          "between the header verdict and the decode (" + std::to_string(plan.opens) +
              " opens of the image, expected " + std::to_string(kExpectedOpens) + ")");
}

struct RedirectPhase {
    const char* label;
    const std::string* replacement;
    size_t expectDecoded;  // the raw-scanline size the decoder MUST have asked for
    size_t expectAbsent;   // the raw-scanline size it must NOT have asked for
    int redirectFromOpen;  // 1-based; 0 = observe only
    int expectRedirects;
};

// GOAL: the single open feeds BOTH the header probes and the decoder -- stb decodes the
//       bytes the budget verdict was computed on. False for anything that reaches the file
//       a second time, including by open(2), which the open counter cannot see.
// WHY-THIS-SHAPE: the oracle is which image stb ALLOCATED for, watching the replacement's
//       size present and the original's absent. "It constructed" would instead lean on
//       production's post-decode dimension guard, and go green if that guard were deleted.
//       The redirect is capped at one: uncapped, a two-open implementation sees the
//       replacement on both opens and passes identically -- measured, see CONTROL (b).
//       Phase 1 (identical copy) is the attribution control; phase 3 arms the old swap at
//       open 2 and requires it to deliver nothing.
// CONTROL: (a) production reverted to `stbi_load(imagePath.c_str(), ...)` -> 10 FAILs here,
//       four on the differently-sized phase ("...(1044995 bytes): NO", "...(264453 bytes):
//       it did", "saw: header-vs-decoder disagreement", "exactly 1 open (saw 2)") and five
//       on the old-swap phase, "0 redirects expected, saw 1" among them. Phase 1 reddened
//       only on the open count, as designed. (b) (a) re-run with redirectMaxCount=0 -> the
//       differently-sized phase's three substantive checks all went GREEN. Both reverted.
// COVERAGE-ASSERT: per-phase open and redirect counts, plus phasesRun == kPhaseCount.
//       Controlled: `p.redirectFromOpen` -> 99 gives "1 redirects expected, saw 0"; an
//       `if (phasesRun == 1) break;` gives "every interposition phase ran (1 of 3)"; and
//       kSmallScanlineBytes computed from kSwapSmallSide + 2 reddens the marker itself.
void checkOneOpenFeedsProbeAndDecode(wallpaperx::Device& device, VkRenderPass renderPass,
                                     const std::string& smallPath,
                                     const std::string& identicalCopyPath,
                                     const std::string& grownPath) {
    const RedirectPhase phases[] = {
        {"byte-identical copy through the same open", &identicalCopyPath, kSmallScanlineBytes,
         kGrownScanlineBytes, 1, 1},
        {"differently-sized image through the same open", &grownPath, kGrownScanlineBytes,
         kSmallScanlineBytes, 1, 1},
        {"the old probe/decode swap, armed at open 2", &grownPath, kSmallScanlineBytes,
         kGrownScanlineBytes, 2, 0},
    };
    constexpr size_t kPhaseCount = sizeof(phases) / sizeof(phases[0]);

    size_t phasesRun = 0;
    for (const RedirectPhase& p : phases) {
        const std::string label(p.label);
        const SwapResult r =
            loadWithRedirect(device, renderPass, smallPath, *p.replacement, p.redirectFromOpen,
                             /*redirectMaxCount=*/1, p.expectDecoded, p.expectAbsent);

        std::printf("image_bounds_probe: %s: %s (%d open(s), %d redirected; decoded-size marker %s, "
                    "replaced-size marker %s)\n",
                    label.c_str(),
                    r.attempt.constructed ? "constructed" : gateName(r.attempt.gate), r.opens,
                    r.redirects, r.attempt.sawWatched[0] ? "seen" : "ABSENT",
                    r.attempt.sawWatched[1] ? "SEEN" : "absent");
        if (!r.attempt.message.empty()) {
            std::printf("image_bounds_probe:   said: %s\n", r.attempt.message.c_str());
        }

        check(r.opens == kExpectedOpens,
              label + ": exactly " + std::to_string(kExpectedOpens) + " open (saw " +
                  std::to_string(r.opens) + ")");
        check(r.redirects == p.expectRedirects,
              label + ": " + std::to_string(p.expectRedirects) + " redirects expected, saw " +
                  std::to_string(r.redirects));
        check(r.attempt.sawWatched[0],
              label + ": stb allocated the buffer for the image this phase expects the decoder to "
                      "have read (" + std::to_string(p.expectDecoded) + " bytes): " +
                  (r.attempt.sawWatched[0] ? "yes" : "NO"));
        check(!r.attempt.sawWatched[1],
              label + ": stb never allocated for the other image (" +
                  std::to_string(p.expectAbsent) + " bytes): " +
                  (r.attempt.sawWatched[1] ? "it did" : "correct"));
        check(r.attempt.constructed,
              label + ": the header verdict and the decode agree, so nothing was substituted "
                      "between them (saw: " +
                  gateName(r.attempt.gate) + ")");
        ++phasesRun;
    }

    check(phasesRun == kPhaseCount,
          "every interposition phase ran (" + std::to_string(phasesRun) + " of " +
              std::to_string(kPhaseCount) + ")");
}

// NOT COVERED, stated rather than left to be inferred from an absence: StaticRenderer's
// post-decode dimension guard. Its only remaining subject is stb's two parsers disagreeing
// with each other about the same bytes, and no fixture this probe can write produces that.
// Reaching it would mean simulating a two-open production that no longer exists, which
// grades a shape the code cannot take. Gate::PostLoadMismatch stays in the classifier so
// that if it ever does fire it is named rather than landing in Unrecognised.

// GOAL: a file that cannot be opened at all is reported as such, naming the OS reason --
//       not folded into "not a decodable image", which is what the single-open rework
//       replaced and which told a user nothing about a permission or a missing path.
// WHY-THIS-SHAPE: two errnos, not one. ENOENT alone would pass for an implementation that
//       special-cased "does not exist" and still mis-attributed everything else; EACCES is
//       the case a wallpaper in an unreadable directory actually produces. The EACCES half
//       is skipped with a printed reason when the process can read a mode-000 file anyway
//       (running as root, which is normal in a container) rather than failing -- an
//       inconclusive case must not print as a defect in the renderer.
// CONTROL: the message on static_renderer.cpp's fopen-failure path changed back to the
//       pre-rework wording, `describeRejection("not a decodable image (can't fopen)", ...)`
//       -> exactly 4 FAILs, "a missing file is reported as unopenable, not as undecodable
//       (saw: not-a-decodable-image)" and its strerror half, for both errnos, and nothing
//       else red. That precise mutation was chosen over the blunter one (pointing the open
//       at /dev/null so it can never fail), which reddens 33 checks across the whole file
//       and would not show that THIS check is what distinguishes the two attributions.
//       Reverted, re-measured green.
void checkUnopenableFileIsAttributed(wallpaperx::Device& device, VkRenderPass renderPass,
                                     const std::string& dir) {
    const std::string missingPath = dir + "/bounds_no_such_file.png";
    std::error_code ignored;
    std::filesystem::remove(missingPath, ignored);

    const Attempt missing = attemptLoad(device, renderPass, missingPath);
    std::printf("image_bounds_probe: a missing file: %s -- %s\n", gateName(missing.gate),
                missing.message.c_str());
    check(missing.gate == Gate::CannotOpen,
          std::string("a missing file is reported as unopenable, not as undecodable (saw: ") +
              gateName(missing.gate) + ")");
    check(missing.message.find(std::strerror(ENOENT)) != std::string::npos,
          "the unopenable-file message carries the OS reason for a missing path");

    const std::string deniedPath = dir + "/bounds_unreadable.png";
    wallpaperx::testing::writeSolidPng(deniedPath, 4, 4, {1, 2, 3, 255});
    std::filesystem::permissions(deniedPath, std::filesystem::perms::none,
                                 std::filesystem::perm_options::replace, ignored);
    // Restore before anything can throw past here, so a mode-000 file is never left in a
    // shared asset directory for the next run (or the other probes) to trip over.
    struct PermissionRestore {
        const std::string& path;
        ~PermissionRestore() {
            std::error_code ec;
            std::filesystem::permissions(path, std::filesystem::perms::owner_read |
                                                   std::filesystem::perms::owner_write,
                                         std::filesystem::perm_options::replace, ec);
        }
    } restore{deniedPath};

    FILE* canStillRead = std::fopen(deniedPath.c_str(), "rb");
    if (canStillRead != nullptr) {
        std::fclose(canStillRead);
        std::printf("image_bounds_probe: SKIPPED the permission-denied case -- this process can read "
                    "a mode-000 file (running as root?), so there is no way to make fopen fail with "
                    "EACCES here. The missing-file case above still ran.\n");
        return;
    }

    const Attempt denied = attemptLoad(device, renderPass, deniedPath);
    std::printf("image_bounds_probe: an unreadable file: %s -- %s\n", gateName(denied.gate),
                denied.message.c_str());
    check(denied.gate == Gate::CannotOpen,
          std::string("a permission-denied file is reported as unopenable, not as undecodable (saw: ") +
              gateName(denied.gate) + ")");
    check(denied.message.find(std::strerror(EACCES)) != std::string::npos,
          "the unopenable-file message carries the OS reason for a permission failure");
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
    // The instrument checks and the S4.9 checks must run in one process: the
    // counterfactual is only a counterfactual if it is measured by the same instrument,
    // in the same build, on the same host.
    const bool ok = wallpaperx::testing::gradedRun(
        "image_bounds_probe", [&](wallpaperx::testing::ValidationGrade& grade) {
            try {
                // The assets share WALLPAPERX_TEST_IMAGE_DIR with the other probes; the
                // bounds_ prefix is what keeps them apart (see renderer/CMakeLists.txt).
                const std::string dir = std::string(WALLPAPERX_TEST_IMAGE_DIR);
                const std::string oversizePath = writeHeaderOnlyPng(
                    dir + "/bounds_oversize_header.png", kOversizeSide, kOversizeSide, 8);
                const std::string validPath = wallpaperx::testing::writeSolidPng(
                    dir + "/bounds_valid.png", kValidSide, kValidSide, {40, 160, 220, 255});

                checkInstrumentSeesTheDecoder(oversizePath);

                wallpaperx::Device device(/*enableValidation=*/true);
                grade.watch(device);
                wallpaperx::OffscreenTargetManager targets(device);

                checkHeaderRejectionAllocatesNothing(device, targets.renderPass(), oversizePath);
                checkInstrumentSeesTheProductionPath(device, targets.renderPass(), validPath, kValidSide);
                checkHeaderGateEdges(device, targets.renderPass(), dir);

                const std::string swapSmallPath = wallpaperx::testing::writeSolidPng(
                    dir + "/bounds_swap_small.png", kSwapSmallSide, kSwapSmallSide, {10, 20, 30, 255});
                const std::string swapCopyPath = wallpaperx::testing::writeSolidPng(
                    dir + "/bounds_swap_small_copy.png", kSwapSmallSide, kSwapSmallSide, {10, 20, 30, 255});
                const std::string swapGrownPath = wallpaperx::testing::writeSolidPng(
                    dir + "/bounds_swap_grown.png", kSwapGrownSide, kSwapGrownSide, {90, 80, 70, 255});

                checkDecodePathOpensTheImageOnce(device, targets.renderPass(), swapSmallPath);
                checkOneOpenFeedsProbeAndDecode(device, targets.renderPass(), swapSmallPath,
                                                swapCopyPath, swapGrownPath);
                checkUnopenableFileIsAttributed(device, targets.renderPass(), dir);
            } catch (const std::exception& e) {
                // A host with no usable ICD lands here. Reported as a failure of this
                // probe's setup, with the reason, rather than as a silent pass.
                wallpaperx::testing::fail(std::string("image_bounds_probe: could not run: ") + e.what());
            }
            // A process-lifetime peak, printed once and deliberately NOT per check: VmHWM
            // never falls, so a "before -> after" pair around one window reads as a delta
            // it cannot be. It is here as context for the header note's central claim --
            // roughly 3 GB was requested across this run and the peak barely moved --
            // and nothing asserts on it.
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
