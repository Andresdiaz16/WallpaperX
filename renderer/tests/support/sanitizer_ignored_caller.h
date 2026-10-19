// Test-only, for ThreadSanitizer builds. Whether a call came from a library backend/tsan.supp names
// `called_from_lib`, for interposers that must hand such a call to libc as the sanitizer itself would.
//
// Why: a test interposer sits between the library and the sanitizer, so the sanitizer sees the call as
// the interposer's and acts on it, while the same library's matching call (an unlock, a relock inside a
// condition wait) reaches the sanitizer directly and is ignored. The mutex then reads as held forever,
// which produces lock-order cycles and races that do not exist without the interposer.
//
// Libraries loaded later (the ICD, the validation layer) are found by refreshing the table when a caller
// lies in no known object. No mutex, no allocation: this runs inside pthread_mutex_lock and
// pthread_cond_wait interposers.
#pragma once

#include <dlfcn.h>
#include <link.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace wallpaperx::testing::ignored_caller {

// Keep in step with backend/tsan.supp's called_from_lib entries.
inline constexpr const char* kLibraries[] = {"libVkLayer_khronos_validation.so", "libvulkan_lvp.so", "libvulkan.so",
                                             "libLLVM", "libvulkan_radeon.so", "libvulkan_intel.so",
                                             "libavcodec.so", "libavutil.so", "libsystemd.so"};

struct Range {
    std::uintptr_t begin;
    std::uintptr_t end;
    bool ignored;
};
inline constexpr int kMaxRanges = 1024;
inline Range g_ranges[kMaxRanges];
inline std::atomic<int> g_count{0};
inline std::atomic_flag g_refreshing = ATOMIC_FLAG_INIT;

// 1 in an ignored library, 0 not, -1 in no known object. Newest first, so an object loaded where a
// closed one used to be is the one that answers.
inline int lookup(std::uintptr_t address, int count) {
    for (int i = count - 1; i >= 0; --i) {
        if (address >= g_ranges[i].begin && address < g_ranges[i].end) return g_ranges[i].ignored ? 1 : 0;
    }
    return -1;
}

// Append-only, published by raising the count, so a reader never sees an entry being written.
inline int append(dl_phdr_info* info, size_t, void* countIn) {
    int& count = *static_cast<int*>(countIn);
    const char* name = info->dlpi_name != nullptr ? info->dlpi_name : "";
    bool ignored = false;
    for (const char* library : kLibraries) ignored = ignored || std::strstr(name, library) != nullptr;
    for (int i = 0; i < info->dlpi_phnum && count < kMaxRanges; ++i) {
        const ElfW(Phdr)& segment = info->dlpi_phdr[i];
        if (segment.p_type != PT_LOAD) continue;
        const std::uintptr_t begin = info->dlpi_addr + segment.p_vaddr;
        const std::uintptr_t end = begin + segment.p_memsz;
        bool known = false;
        for (int k = count - 1; k >= 0 && !known; --k) {
            known = g_ranges[k].begin == begin && g_ranges[k].end == end && g_ranges[k].ignored == ignored;
        }
        if (!known) g_ranges[count++] = Range{begin, end, ignored};
    }
    return 0;
}

inline bool fromIgnoredLibrary(const void* caller) {
    const auto address = reinterpret_cast<std::uintptr_t>(caller);
    const int answer = lookup(address, g_count.load(std::memory_order_acquire));
    if (answer >= 0) return answer == 1;
    while (g_refreshing.test_and_set(std::memory_order_acquire)) {
    }
    int count = g_count.load(std::memory_order_relaxed);
    dl_iterate_phdr(&append, &count);
    g_count.store(count, std::memory_order_release);
    g_refreshing.clear(std::memory_order_release);
    return lookup(address, count) == 1;
}

// libc's own definition of `symbol`, bypassing every interposer and the sanitizer. Resolve it before
// the interposer can be reached from a library, or on first use outside any lock.
template <class Fn>
Fn libcSymbol(const char* symbol) {
    void* libc = ::dlopen("libc.so.6", RTLD_LAZY | RTLD_NOLOAD);
    return libc != nullptr ? reinterpret_cast<Fn>(::dlsym(libc, symbol)) : nullptr;
}

}  // namespace wallpaperx::testing::ignored_caller
