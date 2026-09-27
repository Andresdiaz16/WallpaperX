// SPDX-License-Identifier: GPL-2.0-or-later

// Linked into crash_record_test in an unsanitized build only: stands in for a sanitizer runtime's
// __sanitizer_set_death_callback, which the log writer (backend/daemon/log_pump.cpp) declares weak
// and registers a flush with when present. A sanitized build has the real one, and two definitions
// would clash, so the phase that uses this is skipped there.

#include <atomic>

namespace {
std::atomic<void (*)()> g_callback{nullptr};
}  // namespace

extern "C" void __sanitizer_set_death_callback(void (*callback)()) { g_callback.store(callback); }

// What was registered, for a stand-in sanitizer handler to run as a real one's Die() does.
void (*wallpaperxTestDeathCallback())() { return g_callback.load(); }
