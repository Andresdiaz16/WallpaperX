// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// What daemon_system_test and tests/alloc_fail_shim.cpp (preloaded into the daemon) share, through a
// file both map MAP_SHARED: lock-free atomics, so the two processes can arm and read it without a
// lock between them.

#include <atomic>

namespace wallpaperx::testing {

struct AllocFailControl {
    // The thread that appended a Unix descriptor to a D-Bus message first -- in the daemon, the
    // render worker sending a GetFrame reply. 0 until one has been.
    std::atomic<int> workerTid;
    // While above 0, each operator new on that thread counts it down, and the one taking it to 0
    // throws std::bad_alloc.
    std::atomic<int> countdown;
    // How many were made to fail.
    std::atomic<int> fired;
    // How many times that thread has started waiting on a condition variable the daemon's own code
    // waits on -- the render worker going back to wait for its next job, which it does only once a
    // call is done. A wait reached from a driver or another library, in the middle of a call, is not.
    std::atomic<int> callsDone;
    // Those other waits on that thread, which are not the end of a call.
    std::atomic<int> otherWaits;
};
static_assert(std::atomic<int>::is_always_lock_free);

}  // namespace wallpaperx::testing
