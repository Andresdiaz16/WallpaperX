// SPDX-License-Identifier: GPL-2.0-or-later

#include "crash_record.h"

#include "log_escape.h"
#include "log_pump.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <charconv>
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

// The signal line the handler appends after the JSON line, and the line naming a video it
// appends to the playing record before its own signal line.
constexpr char kSignalPrefix[] = "signal ";
constexpr char kBlamePrefix[] = "blame ";

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

// The playing record's descriptor, -1 while none is open; claimed by compare-exchange.
constexpr int kPlayingClaimed = -2;  // a record owns the slot but has no descriptor to write to
std::atomic<int> g_playingFd{-1};

// The blame line of the video this thread is working on. Per thread, so the render thread and the
// audio thread each name their own; only the owning thread writes it, so the handler on that
// thread sees a live string. Constant-initialized and initial-exec: the handler's read then runs
// no TLS initializer and no __tls_get_addr, neither of which is async-signal-safe.
[[gnu::tls_model("initial-exec")]] constinit thread_local std::atomic<const std::string*> t_blame{nullptr};

// Set by stopBlamingWallpapers(). Sequentially consistent against g_slot's publish, so a record
// being constructed as it is set is seen by one side or the other and removed.
std::atomic<bool> g_blameless{false};

constexpr int kFatalSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
constexpr int kHandlerSlots = 32;  // every signal above is below this
struct sigaction g_previous[kHandlerSlots] = {};

void writeSignalLine(int fd, int sig) {
    char line[16];
    static_assert(sizeof(kSignalPrefix) - 1 + 3 <= sizeof(line), "the prefix, two digits and a newline");
    size_t n = sizeof(kSignalPrefix) - 1;
    std::memcpy(line, kSignalPrefix, n);
    if (sig >= 10) line[n++] = static_cast<char>('0' + sig / 10);
    line[n++] = static_cast<char>('0' + sig % 10);
    line[n++] = '\n';
    const ssize_t ignored = ::write(fd, line, n);
    (void)ignored;
}

// The signal writeSignalLine() put at the start of `rest`, or nothing for a line it could not have
// written -- a corrupt file's number is bounded rather than overflowed.
std::optional<int> parseSignalLine(std::string_view rest) {
    if (!rest.starts_with(kSignalPrefix)) return std::nullopt;
    rest.remove_prefix(sizeof(kSignalPrefix) - 1);
    int signal = 0;
    if (std::from_chars(rest.data(), rest.data() + rest.size(), signal).ec != std::errc() || signal <= 0) {
        return std::nullopt;
    }
    return signal;
}

// Not for the handler: it allocates.
std::string signalName(int signal) {
    const char* name = ::sigabbrev_np(signal);
    return name != nullptr ? "SIG" + std::string(name) : "signal " + std::to_string(signal);
}

// Removes the record published as `generation` without touching its descriptor, which the applying
// thread could close and reuse meanwhile. The path is copied and the generation re-checked, so a
// record withdrawn and replaced during the copy is left to its own thread rather than unlinked by a
// torn name. A read-modify-write rather than a load: its release half keeps the copy from drifting
// past it, which a fence would also do but TSan cannot model. Async-signal-safe.
void unlinkPublished(uint64_t generation) {
    if (generation == kFree || generation == kHeld) return;
    char path[PATH_MAX];
    std::memcpy(path, g_path, sizeof(path));
    uint64_t still = generation;
    if (g_slot.compare_exchange_strong(still, generation, std::memory_order_acq_rel)) ::unlink(path);
}

// Everything here is async-signal-safe: the records were formatted in advance, so this only
// writes to open descriptors, or unlinks a path already spelled out.
void onFatalSignal(int sig, siginfo_t* info, void* context) {
    const std::string* blame = t_blame.load(std::memory_order_acquire);
    const int playingFd = g_playingFd.load(std::memory_order_acquire);
    const bool blameless = g_blameless.load();
    if (!blameless && blame != nullptr && playingFd >= 0) {
        const ssize_t ignored = ::write(playingFd, blame->data(), blame->size());
        (void)ignored;
        writeSignalLine(playingFd, sig);
    }

    const uint64_t generation = g_slot.load(std::memory_order_acquire);
    if (!blameless && generation != kFree && generation != kHeld && ::gettid() == g_tid.load(std::memory_order_relaxed)) {
        // The record is this thread's, and this thread is here, so nothing withdraws it.
        writeSignalLine(g_fd.load(std::memory_order_relaxed), sig);
    } else {
        // This thread's death says nothing about the wallpaper, nor about any the other threads
        // go on applying while the log below is flushed.
        stopBlamingWallpapers();
    }

    // Before the previous handler, which may not return: what is queued goes out first, an abort's
    // message included. Only a flush, not release(): the other threads run on, and on the original
    // descriptors a stalled destination would block them and a sanitizer's report after this. That
    // report goes through the pipe, flushed by the death callback install() registers.
    logpump::flush(logpump::kExitFlush);

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

// The whole of a record this daemon could have written, or nothing: a file larger than `maxBytes`
// or not regular is not one of ours.
std::string readRecordFile(const std::filesystem::path& file, off_t maxBytes) {
    const int fd = ::open(file.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return {};

    std::string text;
    struct stat info {};
    if (::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size <= maxBytes) {
        char buffer[4096];
        for (;;) {
            const ssize_t n = ::read(fd, buffer, sizeof(buffer));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            text.append(buffer, static_cast<size_t>(n));
            if (text.size() > static_cast<size_t>(maxBytes)) {
                text.clear();
                break;
            }
        }
    }
    ::close(fd);
    return text;
}

// Created with its directory when that is missing, truncated when it exists. -1 on failure.
int createRecordFile(const std::filesystem::path& file) {
    constexpr int kFlags = O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW;
    int fd = ::open(file.c_str(), kFlags, 0644);
    if (fd < 0 && errno == ENOENT) {
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
        if (!ec) fd = ::open(file.c_str(), kFlags, 0644);
    }
    return fd;
}

bool writeAll(int fd, const std::string& text) {
    size_t written = 0;
    while (written < text.size()) {
        const ssize_t n = ::write(fd, text.data() + written, text.size() - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        written += static_cast<size_t>(n);
    }
    return true;
}

constexpr const char* kKeyVideos = "videos";
constexpr const char* kKeyRun = "run";
constexpr const char* kKeyDeaths = "deaths";

// Thirty-two monitors, each path at PATH_MAX with every byte escaped by JSON, fit well inside.
constexpr off_t kMaxPlayingRecordBytes = 1024 * 1024;

std::optional<VideoOnMonitor> readVideoOnMonitor(const nlohmann::json& entry) {
    VideoOnMonitor video;
    if (!entry.is_object() || !entry.contains(kKeyMonitorId) || !entry.at(kKeyMonitorId).is_number_integer() ||
        !readString(entry, kKeyPath, video.path)) {
        return std::nullopt;
    }
    const int64_t monitorId = entry.at(kKeyMonitorId).get<int64_t>();
    if (monitorId < 0 || monitorId > INT_MAX) return std::nullopt;
    video.monitorId = static_cast<int>(monitorId);
    return video;
}

// Beside the playing record: "<run> <monitor id>" per line, one for each monitor whose count was
// cleared.
std::filesystem::path clearedRecordPath(const std::filesystem::path& stateDir) {
    if (stateDir.empty()) return {};
    return stateDir / "playing-cleared";
}

// The monitors the run named `run` cleared. Lines from any other run are ignored, so a file an
// earlier start failed to remove says nothing about this record.
std::set<int> readCleared(const std::filesystem::path& file, std::string_view run) {
    std::set<int> monitors;
    const std::string text = readRecordFile(file, kMaxRecordBytes);
    for (size_t start = 0; start < text.size();) {
        const size_t end = std::min(text.find('\n', start), text.size());
        const std::string_view line = std::string_view(text).substr(start, end - start);
        start = end + 1;
        // A line cut short has no newline, and a partial number must not zero another monitor.
        if (end == text.size() || !line.starts_with(run) || line.size() <= run.size() || line[run.size()] != ' ') {
            continue;
        }
        const std::string_view number = line.substr(run.size() + 1);
        int monitorId = 0;
        const auto parsed = std::from_chars(number.data(), number.data() + number.size(), monitorId);
        if (parsed.ec == std::errc() && parsed.ptr == number.data() + number.size()) monitors.insert(monitorId);
    }
    return monitors;
}

std::optional<PlayingVideo> readPlayingVideo(const nlohmann::json& entry) {
    std::optional<VideoOnMonitor> video = readVideoOnMonitor(entry);
    if (!video || !entry.contains(kKeyDeaths) || !entry.at(kKeyDeaths).is_number_integer()) return std::nullopt;
    const int64_t deaths = entry.at(kKeyDeaths).get<int64_t>();
    if (deaths < 0 || deaths > INT_MAX) return std::nullopt;
    return PlayingVideo{video->monitorId, std::move(video->path), static_cast<int>(deaths)};
}

}  // namespace

std::filesystem::path crashRecordPath(const std::filesystem::path& stateDir, int monitorId) {
    if (stateDir.empty()) return {};
    return stateDir / ("applying-" + std::to_string(monitorId) + ".json");
}

std::optional<CrashRecord> readCrashRecord(const std::filesystem::path& stateDir, int monitorId) {
    const std::filesystem::path file = crashRecordPath(stateDir, monitorId);
    if (file.empty()) return std::nullopt;

    const std::string text = readRecordFile(file, kMaxRecordBytes);
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
    record.signal = parseSignalLine(rest);
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
    return "not applied: the daemon crashed with " + signalName(*crash.signal) +
           " while applying this wallpaper on its last run; set it again to retry";
}

std::filesystem::path playingRecordPath(const std::filesystem::path& stateDir) {
    if (stateDir.empty()) return {};
    return stateDir / "playing.json";
}

std::optional<PlayingHistory> readPlayingRecord(const std::filesystem::path& stateDir) {
    const std::filesystem::path file = playingRecordPath(stateDir);
    if (file.empty()) return std::nullopt;

    // The counts on the first line, then what the handler appended: a blame line and a signal line.
    const std::string text = readRecordFile(file, kMaxPlayingRecordBytes);
    const size_t newline = text.find('\n');
    if (newline == std::string::npos) return std::nullopt;
    const nlohmann::json root = nlohmann::json::parse(text.substr(0, newline), nullptr, false);
    if (!root.is_object() || !root.contains(kKeyVideos) || !root.at(kKeyVideos).is_array()) return std::nullopt;

    std::string run;
    const std::set<int> cleared =
        readString(root, kKeyRun, run) ? readCleared(clearedRecordPath(stateDir), run) : std::set<int>();
    PlayingHistory history;
    for (const nlohmann::json& entry : root.at(kKeyVideos)) {
        std::optional<PlayingVideo> video = readPlayingVideo(entry);
        if (video && cleared.count(video->monitorId) == 0) history.videos.push_back(std::move(*video));
    }

    std::string_view rest(text.data() + newline + 1, text.size() - newline - 1);
    if (!rest.starts_with(kBlamePrefix)) return history;
    rest.remove_prefix(sizeof(kBlamePrefix) - 1);
    const size_t blameEnd = rest.find('\n');
    if (blameEnd == std::string_view::npos) return history;
    history.died = readVideoOnMonitor(nlohmann::json::parse(rest.substr(0, blameEnd), nullptr, false));
    rest.remove_prefix(blameEnd + 1);
    if (history.died) history.signal = parseSignalLine(rest);
    return history;
}

std::string describePlaybackDeaths(int deaths, std::optional<int> signal) {
    const std::string how = signal ? "crashed with " + signalName(*signal) : "crashed";
    return "not applied: the daemon " + how + " while playing this video, the " + std::to_string(deaths) +
           (deaths == 2 ? "nd" : deaths == 3 ? "rd" : "th") +
           " time since it was set; set it again to retry";
}

std::string playingBlameLine(int monitorId, const std::string& path) {
    if (!isValidUtf8(path)) return {};
    return std::string(kBlamePrefix) + nlohmann::json{{kKeyMonitorId, monitorId}, {kKeyPath, path}}.dump() + "\n";
}

PlayingRecord::PlayingRecord(std::filesystem::path stateDir)
    : m_file(playingRecordPath(stateDir)), m_clearedFile(clearedRecordPath(stateDir)) {}

PlayingRecord::~PlayingRecord() {
    end();
    if (m_fd < 0) return;
    ::close(m_fd);
    g_playingFd.store(-1, std::memory_order_release);
}

void PlayingRecord::start(const std::vector<PlayingVideo>& videos) {
    if (m_file.empty() || m_fd >= 0) return;

    const std::string run = std::to_string(::getpid()) + "-" +
                            std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    std::set<int> counted;
    nlohmann::json entries = nlohmann::json::array();
    for (const PlayingVideo& video : videos) {
        // dump() throws on invalid UTF-8, and such a path never reaches the config to be restored.
        if (!isValidUtf8(video.path)) continue;
        entries.push_back({{kKeyMonitorId, video.monitorId}, {kKeyPath, video.path}, {kKeyDeaths, video.deaths}});
        counted.insert(video.monitorId);
    }
    const std::string text = nlohmann::json{{kKeyRun, run}, {kKeyVideos, std::move(entries)}}.dump() + "\n";

    // Claimed before the file is touched, so a second record in the process leaves the first's alone.
    int expected = -1;
    if (!g_playingFd.compare_exchange_strong(expected, kPlayingClaimed, std::memory_order_acq_rel)) return;

    // Whatever the last run left has been read and is answered for by this one, so it goes even
    // when nothing can replace it: left, the next start would count the same death again. A record
    // that could not be written names nothing, as an ApplyingRecord's does.
    const int fd = createRecordFile(m_file);
    if (fd < 0 || !writeAll(fd, text)) {
        if (fd >= 0) ::close(fd);
        std::error_code ec;
        std::filesystem::remove(m_file, ec);
        g_playingFd.store(-1, std::memory_order_release);
        return;
    }
    m_fd = fd;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_run = run;
        m_counted = std::move(counted);
    }
    g_playingFd.store(fd, std::memory_order_release);
    // Folded into the counts just written. Left behind, its lines name another run and are ignored.
    std::error_code ec;
    std::filesystem::remove(m_clearedFile, ec);
}

void PlayingRecord::clear(int monitorId) noexcept {
    try {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_counted.count(monitorId) == 0) return;
        const std::string line = m_run + " " + std::to_string(monitorId) + "\n";
        m_counted.erase(monitorId);
        const int fd = ::open(m_clearedFile.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0644);
        if (fd < 0) return;
        writeAll(fd, line);
        ::close(fd);
    } catch (const std::exception&) {
        // Allocation or the lock: the count stands, which only costs a clear.
    }
}

void PlayingRecord::end() {
    // Withdrawn from the handler, not closed: the render thread may still be running, and a handler
    // that read the descriptor just before must not write into one reused since. The destructor
    // closes it, once no thread can be working on a video.
    int mine = m_fd;
    if (mine >= 0) g_playingFd.compare_exchange_strong(mine, kPlayingClaimed, std::memory_order_acq_rel);
}

PlayingBlame::PlayingBlame(const std::string& line) : m_previous(t_blame.load(std::memory_order_relaxed)) {
    // An empty line names nothing, and so blames nothing an outer guard names either: the thread is
    // working on this video now, and it cannot be recorded.
    t_blame.store(line.empty() ? nullptr : &line, std::memory_order_release);
}

PlayingBlame::~PlayingBlame() { t_blame.store(m_previous, std::memory_order_release); }

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
    if (g_blameless.load()) {
        g_slot.store(kFree, std::memory_order_release);
        return;
    }

    const int fd = createRecordFile(file);
    if (fd < 0) {
        g_slot.store(kFree, std::memory_order_release);
        return;
    }
    if (!writeAll(fd, text)) {
        // Half a record would be read as no record, so it is not worth keeping open.
        ::close(fd);
        ::unlink(file.c_str());
        g_slot.store(kFree, std::memory_order_release);
        return;
    }

    m_file = file.native();
    m_fd = fd;
    std::memcpy(g_path, m_file.c_str(), m_file.size() + 1);
    g_tid.store(::gettid(), std::memory_order_relaxed);
    g_fd.store(fd, std::memory_order_relaxed);
    g_slot.store(++g_lastGeneration);
    // Set since the check above: stopBlamingWallpapers() may have looked before this record was
    // published, and nothing may stay on disk once it has run.
    if (g_blameless.load()) ::unlink(m_file.c_str());
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

void stopBlamingWallpapers() noexcept {
    g_blameless.store(true);
    unlinkPublished(g_slot.load());
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
