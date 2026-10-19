// Test-only. Whether an address lies inside a loaded library, for interposers that must treat calls
// from one library differently -- ThreadSanitizer's own runtime, or a library tsan.supp names.
//
// Build the object at namespace scope, so the library table is read before main. contains() is
// plain arithmetic: it is asked from inside the sanitizer's report path, where anything that takes a
// lock -- dladdr, a std::mutex, an allocation -- can deadlock against the sanitizer.
#pragma once

#include <link.h>

#include <cstdint>
#include <cstring>

namespace wallpaperx::testing {

class LoadedRange {
public:
    // `fragment` is matched as a substring of the loaded object's path, e.g. "libtsan.so".
    explicit LoadedRange(const char* fragment) : m_fragment(fragment) { dl_iterate_phdr(&LoadedRange::visit, this); }

    bool found() const noexcept { return m_begin < m_end; }
    bool contains(const void* address) const noexcept {
        const auto a = reinterpret_cast<std::uintptr_t>(address);
        return a >= m_begin && a < m_end;
    }

private:
    static int visit(dl_phdr_info* info, size_t, void* self) {
        auto* range = static_cast<LoadedRange*>(self);
        if (info->dlpi_name == nullptr || std::strstr(info->dlpi_name, range->m_fragment) == nullptr) return 0;
        for (int i = 0; i < info->dlpi_phnum; ++i) {
            const ElfW(Phdr)& segment = info->dlpi_phdr[i];
            if (segment.p_type != PT_LOAD) continue;
            const std::uintptr_t begin = info->dlpi_addr + segment.p_vaddr;
            const std::uintptr_t end = begin + segment.p_memsz;
            if (range->m_begin == range->m_end) {
                range->m_begin = begin;
                range->m_end = end;
            } else {
                if (begin < range->m_begin) range->m_begin = begin;
                if (end > range->m_end) range->m_end = end;
            }
        }
        return 1;
    }

    const char* m_fragment;
    std::uintptr_t m_begin = 0;
    std::uintptr_t m_end = 0;
};

}  // namespace wallpaperx::testing
