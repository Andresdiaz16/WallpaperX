// SPDX-License-Identifier: GPL-2.0-or-later

#include "crash_record.h"

#include "log_escape.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace wallpaperx {
namespace {

constexpr const char* kKeyMonitorId = "monitor_id";
constexpr const char* kKeyType = "type";
constexpr const char* kKeyPath = "path";

// The signal line the handler appends after the JSON line.
constexpr char kSignalPrefix[] = "signal ";

// Far above any record this daemon writes -- one JSON line holding a path -- so a file this
// size is not ours to read.
constexpr off_t kMaxRecordBytes = 64 * 1024;

// The one slot the handler reads, in a single word: kFree, kHeld (a record owns it and its
// fields are not readable), or a generation >= 2 meaning they are. Claimed by compare-exchange,
// so a second record while one is live gets no slot and writes no file; the fields below are
// written only by the holder, before the generation is stored with release. Generations never
// repeat, which is what lets the handler tell that a path it copied was not rewritten under it.
constexpr uint64_t kFree = 0;
constexpr uint64_t kHeld = 1;
std::atomic<uint64_t> g_slot{kFree};
uint64_t g_lastGeneration = kHeld;  // holder-only; ordered between holders by the claim
std::atomic<int> g_fd{-1};
std::atomic<pid_t> g_tid{0};
char g_path[PATH_MAX] = {};

constexpr int kFatalSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
constexpr int kHandlerSlots = 32;  // every signal above is below this
struct sigaction g_previous[kHandlerSlots] = {};

// Everything here is async-signal-safe: the record was formatted before the render began, so
// this only writes a signal number to an open descriptor, or unlinks a path already spelled out.
void onFatalSignal(int sig, siginfo_t* info, void* context) {
    const uint64_t generation = g_slot.load(std::memory_order_acquire);
    if (generation != kFree && generation != kHeld) {
        if (::gettid() == g_tid.load(std::memory_order_relaxed)) {
            // The record is this thread's, and this thread is here, so nothing withdraws it.
            char line[16] = "signal ";
            size_t n = sizeof(kSignalPrefix) - 1;
            if (sig >= 10) line[n++] = static_cast<char>('0' + sig / 10);
            line[n++] = static_cast<char>('0' + sig % 10);
            line[n++] = '\n';
            const ssize_t ignored = ::write(g_fd.load(std::memory_order_relaxed), line, n);
            (void)ignored;
        } else {
            // Never written to from here: this thread's death says nothing about the wallpaper,
            // and the descriptor could be closed and reused by the applying thread meanwhile.
            // The path is copied and the generation re-checked, so a record withdrawn and
            // replaced during the copy is left to its own thread rather than unlinked by a torn
            // name. A read-modify-write rather than a load: its release half keeps the copy
            // from drifting past it, which a fence would also do but TSan cannot model.
            char path[PATH_MAX];
            std::memcpy(path, g_path, sizeof(path));
            uint64_t still = generation;
            if (g_slot.compare_exchange_strong(still, generation, std::memory_order_acq_rel)) ::unlink(path);
        }
    }

    // A sanitizer's handler, where one was installed, still gets to print its report.
    if (sig >= 0 && sig < kHandlerSlots) {
        const struct sigaction& previous = g_previous[sig];
        if ((previous.sa_flags & SA_SIGINFO) != 0) {
            if (previous.sa_sigaction != nullptr) previous.sa_sigaction(sig, info, context);
        } else if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN) {
            previous.sa_handler(sig);
        }
    }

    // SA_RESETHAND put the default back, so this ends the process the way the signal would have.
    ::raise(sig);
}

bool readString(const nlohmann::json& object, const char* key, std::string& out) {
    if (!object.contains(key)) return false;
    const nlohmann::json& field = object.at(key);
    if (!field.is_string()) return false;
    out = field.get<std::string>();
    return true;
}

}  // namespace

std::filesystem::path crashRecordPath(const std::filesystem::path& stateDir, int monitorId) {
    if (stateDir.empty()) return {};
    return stateDir / ("applying-" + std::to_string(monitorId) + ".json");
}

std::optional<CrashRecord> readCrashRecord(const std::filesystem::path& stateDir, int monitorId) {
    const std::filesystem::path file = crashRecordPath(stateDir, monitorId);
    if (file.empty()) return std::nullopt;

    const int fd = ::open(file.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return std::nullopt;

    std::string text;
    struct stat info {};
    if (::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size <= kMaxRecordBytes) {
        char buffer[4096];
        for (;;) {
            const ssize_t n = ::read(fd, buffer, sizeof(buffer));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            text.append(buffer, static_cast<size_t>(n));
            if (text.size() > static_cast<size_t>(kMaxRecordBytes)) break;
        }
    }
    ::close(fd);

    const size_t newline = text.find('\n');
    if (newline == std::string::npos) return std::nullopt;

    // allow_exceptions=false: a torn or foreign file is a value here, not a throw.
    const nlohmann::json root = nlohmann::json::parse(text.substr(0, newline), nullptr, false);
    CrashRecord record;
    if (!root.is_object() || !root.contains(kKeyMonitorId) || !root.at(kKeyMonitorId).is_number_integer() ||
        root.at(kKeyMonitorId) != monitorId || !readString(root, kKeyType, record.type) ||
        !readString(root, kKeyPath, record.path)) {
        return std::nullopt;
    }

    const std::string_view rest(text.data() + newline + 1, text.size() - newline - 1);
    if (rest.starts_with(kSignalPrefix)) {
        int signal = 0;
        for (size_t i = sizeof(kSignalPrefix) - 1; i < rest.size() && rest[i] >= '0' && rest[i] <= '9'; ++i) {
            signal = signal * 10 + (rest[i] - '0');
        }
        if (signal > 0) record.signal = signal;
    }
    return record;
}

void removeCrashRecord(const std::filesystem::path& stateDir, int monitorId) {
    const std::filesystem::path file = crashRecordPath(stateDir, monitorId);
    if (file.empty()) return;
    std::error_code ec;
    std::filesystem::remove(file, ec);
}

std::string describeCrash(const CrashRecord& crash) {
    if (!crash.signal) {
        return "not applied: the daemon was killed while applying this wallpaper on its last run; "
               "set it again to retry";
    }
    const char* name = ::sigabbrev_np(*crash.signal);
    return "not applied: the daemon crashed with " +
           (name != nullptr ? "SIG" + std::string(name) : "signal " + std::to_string(*crash.signal)) +
           " while applying this wallpaper on its last run; set it again to retry";
}

ApplyingRecord::ApplyingRecord(const std::filesystem::path& stateDir, int monitorId, const std::string& type,
                               const std::string& path) {
    const std::filesystem::path file = crashRecordPath(stateDir, monitorId);
    if (file.empty() || file.native().size() >= sizeof(g_path)) return;

    // dump() throws on invalid UTF-8. Such a path never reaches the config file either, so no
    // restart will re-apply it and there is nothing to record.
    if (!isValidUtf8(type) || !isValidUtf8(path)) return;
    const std::string text =
        nlohmann::json{{kKeyMonitorId, monitorId}, {kKeyType, type}, {kKeyPath, path}}.dump() + "\n";

    // Before the file exists: a record the handler cannot see must not be on disk either, or
    // a crash on another thread would leave it to refuse this wallpaper on no evidence.
    uint64_t expected = kFree;
    if (!g_slot.compare_exchange_strong(expected, kHeld, std::memory_order_acq_rel)) return;

    constexpr int kFlags = O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW;
    int fd = ::open(file.c_str(), kFlags, 0644);
    if (fd < 0 && errno == ENOENT) {
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
        if (!ec) fd = ::open(file.c_str(), kFlags, 0644);
    }
    if (fd < 0) {
        g_slot.store(kFree, std::memory_order_release);
        return;
    }

    size_t written = 0;
    while (written < text.size()) {
        const ssize_t n = ::write(fd, text.data() + written, text.size() - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            // Half a record would be read as no record, so it is not worth keeping open.
            ::close(fd);
            ::unlink(file.c_str());
            g_slot.store(kFree, std::memory_order_release);
            return;
        }
        written += static_cast<size_t>(n);
    }

    m_file = file.native();
    m_fd = fd;
    std::memcpy(g_path, m_file.c_str(), m_file.size() + 1);
    g_tid.store(::gettid(), std::memory_order_relaxed);
    g_fd.store(fd, std::memory_order_relaxed);
    g_slot.store(++g_lastGeneration, std::memory_order_release);
}

ApplyingRecord::~ApplyingRecord() {
    if (m_fd < 0) return;
    // Withdrawn before the close, so a handler interrupting this thread finds a live record
    // or none; freed only after the unlink, so the next record -- possibly the same monitor's,
    // at the same path -- cannot be created and then removed by this one's unlink.
    g_slot.store(kHeld, std::memory_order_release);
    ::close(m_fd);
    ::unlink(m_file.c_str());
    g_slot.store(kFree, std::memory_order_release);
}

void installFatalSignalHandlers() {
    struct sigaction action {};
    action.sa_sigaction = onFatalSignal;
    sigemptyset(&action.sa_mask);
    // RESETHAND so the re-raise lands on the default action; NODEFER so it lands at once;
    // ONSTACK so a thread that set up an alternate stack overflows onto it.
    action.sa_flags = SA_SIGINFO | SA_RESETHAND | SA_NODEFER | SA_ONSTACK;
    for (const int sig : kFatalSignals) {
        ::sigaction(sig, &action, &g_previous[sig]);
    }
}

}
