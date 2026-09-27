// SPDX-License-Identifier: GPL-2.0-or-later

// Linked into crash_record_test only: a real memory fault on the log writer's own thread
// (backend/daemon/log_pump.cpp), which is the only code in that binary that calls memrchr. The
// executable's definition takes the place of libc's for everything statically linked into it.
//
// Its own translation unit because <cstring> declares memrchr's C++ overloads, which a definition
// here would collide with.

#include <atomic>
#include <cstddef>

#include <sys/mman.h>

namespace {
std::atomic<bool> g_armed{false};
}  // namespace

// Armed, the next memrchr call stores to a page mapped PROT_NONE: a fault the thread takes.
void wallpaperxTestFaultInLogWriter() { g_armed.store(true); }

extern "C" void* memrchr(const void* data, int byte, std::size_t size) {
    if (g_armed.exchange(false)) {
        void* page = ::mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page != MAP_FAILED) *static_cast<volatile char*>(page) = 1;
    }
    const auto* at = static_cast<const unsigned char*>(data) + size;
    while (size-- > 0) {
        if (*--at == static_cast<unsigned char>(byte)) return const_cast<unsigned char*>(at);
    }
    return nullptr;
}
