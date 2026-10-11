// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// A config write that blocks until the test lets it through -- a dead mount, in one mknod.
//
// It is the only lever this suite has on a slow filesystem: production may not be patched
// for a test, and no developer's tmpfs has a write that takes measurable time.
// ConfigStore::save() opens "<config.json>.tmp" with O_WRONLY and deliberately no
// O_NONBLOCK, and opening a fifo for writing parks the caller in the kernel until a reader
// arrives -- which is exactly what a stalled NFS mount does to that same open().
//
// Shared by the two tiers that use it (config_store_test drives WallpaperManager directly,
// daemon_system_test drives the real daemon binary) rather than copied into each: the
// fixture's correctness rests on details -- the non-blocking open, "no writer yet" not being
// end of file, the probe below -- that a second copy would drift away from silently. It is
// header-only and holds no state either tier shares.

#include "daemon/config_store.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <system_error>
#include <thread>

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

namespace wallpaperx::testing {

// True only for a regular file. Every read of a config file a released write may have
// touched goes through this first: the release renames the fifo ONTO config.json, and
// opening a fifo for reading with no writer attached blocks for good -- a hang where a check
// was meant to be. ConfigStore::load() guards the same way, with O_NONBLOCK.
inline bool isRegularFile(const std::filesystem::path& file) {
    struct stat info {};
    return ::stat(file.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

// The dependence on the ".tmp" name is real and is never assumed: blockingWriteFixtureWorks()
// below parks a save of its own through this fixture before any phase grades anything, so a
// rename of the store's temp file turns those phases into skips that name the fixture rather
// than into failures blaming the daemon for a write that was never blocked.
// CONTROL for that (X.3): the suffix here changed to ".tmp2", i.e. the store writing
//       somewhere this fixture does not watch -> config_store_test printed three NOT VERIFIED
//       lines naming ConfigStore::save() and the path, 0 FAILs, and exit 77 (ctest SKIP).
//       Reverted.
class BlockingWrite {
public:
    explicit BlockingWrite(const std::filesystem::path& configFile) : m_temp(configFile.string() + ".tmp") {}

    bool arm() {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(m_temp).parent_path(), ec);
        return !ec && ::mkfifo(m_temp.c_str(), 0644) == 0;
    }

    const std::string& temp() const { return m_temp; }

    struct Released {
        bool ok = false;
        std::string bytes;  // what the parked write was carrying, to the byte

        // When the writer was let go. A lower bound on when the write could have finished,
        // and the timestamp an ordering check should be stated against: "when the writer
        // closed" is observed by this thread's peer, which may not be scheduled until after
        // the writing thread has already moved on, so a check against it would be graded on
        // the scheduler.
        std::chrono::steady_clock::time_point at{};
        std::string why;
    };

    // Attaches a reader, which releases the parked write, and reads what it writes until the
    // writer closes. Never blocks indefinitely: the open is O_NONBLOCK, so a fixture that
    // failed to park anything gives up at `patience` with a reason instead of hanging the
    // thread its caller is going to join.
    Released release(std::chrono::milliseconds patience) {
        Released out;
        const int fd = ::open(m_temp.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            out.why = m_temp + " could not be opened for reading: " + std::strerror(errno);
            return out;
        }
        out.at = std::chrono::steady_clock::now();
        const auto deadline = out.at + patience;
        bool sawBytes = false;
        for (;;) {
            char buf[4096];
            const ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n > 0) {
                out.bytes.append(buf, static_cast<size_t>(n));
                sawBytes = true;
                continue;
            }
            if (n == 0 && sawBytes) {
                out.ok = true;
                break;
            }
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                out.why = m_temp + " could not be read: " + std::strerror(errno);
                break;
            }
            // read()==0 with nothing read yet is "no writer has attached", which a
            // non-blocking reader cannot tell from end of file. Treated as "not yet" and
            // bounded by the deadline, never as a finished write: the opposite reading would
            // grade an empty pipe as a completed save.
            if (std::chrono::steady_clock::now() >= deadline) {
                out.why = "no config write reached " + m_temp + " within " + std::to_string(patience.count()) +
                          " ms";
                break;
            }
            pollfd waiting{fd, POLLIN, 0};
            ::poll(&waiting, 1, 20);
        }
        ::close(fd);
        return out;
    }

private:
    std::string m_temp;
};

// Proves the fixture above actually parks a write, using ConfigStore directly -- no manager,
// no daemon, no deadline.
//
// Without it, "the save did not block" has two causes that look identical from outside: a
// store that no longer opens "<file>.tmp", which makes this fixture dead and the phase
// inconclusive, and a daemon that no longer waits for its write, which is the defect the
// phases exist to catch. Reporting the first as the second is exactly the
// blame-the-daemon-for-the-harness failure X.3 forbids, and the fifo's continued existence
// does not tell them apart: a store writing somewhere else leaves it untouched.
//
// Every wait here is bounded, and the save runs on a thread that is never joined: on a host
// where the fixture works, that thread is parked in the kernel until this releases it.
inline bool blockingWriteFixtureWorks(const std::filesystem::path& probeFile, std::string& why) {
    BlockingWrite probe(probeFile);
    if (!probe.arm()) {
        why = "a fifo could not be created at " + probe.temp();
        return false;
    }
    const ConfigStore store(probeFile);
    auto saved = std::make_shared<std::promise<std::string>>();
    std::future<std::string> future = saved->get_future();
    std::thread([saved, store] {
        saved->set_value(store.save(PersistedConfig{{PersistedWallpaper{7, "static", "/probe.png"}}}).error);
    }).detach();

    if (future.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready) {
        why = "a save into a fifo at " + probe.temp() +
              " returned instead of parking, so ConfigStore::save() no longer opens that path and no write on "
              "this host can be made to block";
        return false;
    }
    const BlockingWrite::Released released = probe.release(std::chrono::milliseconds(5000));
    if (!released.ok) {
        why = "the parked probe save wrote nothing that could be read back (" + released.why + ")";
        return false;
    }
    if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        why = "the probe save did not return even after its write was released, so this fixture cannot let a "
              "blocked write through again";
        return false;
    }
    return true;
}

}  // namespace wallpaperx::testing
