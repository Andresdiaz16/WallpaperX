#include "support/validation_probe.h"

#include <vulkan/vulkan.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

namespace wallpaperx {
namespace {

// The prefix Device's debug callback writes before every layer message.
constexpr const char* kWarningPrefix = "[vulkan] ";

// Printed by Device::createInstance when the layer is present but
// VK_EXT_validation_features is not. It carries the same prefix as a real layer message
// -- it is a note about the validator's own configuration, not a complaint about our
// Vulkan usage -- so counting it would fail every check on a host with a stripped layer
// build. Matched by its text because that is the only thing distinguishing it; if the
// wording in device.cpp changes this stops excluding it and the affected probes go red
// rather than silently miscounting.
constexpr const char* kNotAWarning = "validation layer present but VK_EXT_validation_features is not";

class Capture;
Capture* g_instance = nullptr;

// Redirects stderr to a temporary file, counts the validation warnings written to it,
// and copies everything through to the real stderr as it is read.
//
// WHY A FILE AND NO THREAD. The obvious shape is a pipe with a reader thread forwarding
// bytes live. That version deadlocked: renderer/tests/device_affinity_probe.cpp forks,
// and fork() in a process with a second thread gives the child a copy of a heap whose
// allocator lock that thread may have been holding -- the child then wedged in its first
// malloc, the parent blocked reading a pipe that never reached EOF, and the probe hung
// forever (observed 3 runs out of 3, killed at 25 s). A file needs no reader running
// concurrently with the fork, so there is no second thread to inherit a lock from.
//
// WHY NOT READ THE CALLBACK, which would be simpler still: the callback is production
// code in device.cpp, and giving it a test-visible sink is precisely the test-only hook
// this file exists to avoid. stderr is an interface production already commits to.
//
// THE COST IS THAT OUTPUT IS BATCHED, and that is dangerous rather than merely
// inconvenient: a process that dies by a signal or _Exit runs no destructor, so without
// the handlers installed below the whole run's stderr goes into the temporary file and
// is discarded with it. That would be a straight regression against writing to stderr
// directly, which at least printed everything up to the fault.
//
// CONTROL (install failure): the constructor forced down its failure path
//       (`m_file = nullptr; errno = EROFS;`) -> device_probe printed "FATAL: the
//       validation stderr capture could not be installed (Read-only file system)" and
//       exited 1. Before requireActive() existed, count() returned a hard 0 and the run
//       reported all checks passed, including "zero validation warnings" -- across all
//       eight linked binaries at once. Reverted.
// CONTROL (death handlers): a marker written to stderr immediately before a deliberate
//       std::abort() in device_probe's main -> the marker reached the terminal (exit
//       134). With installDeathHandlers() commented out, the same run printed it zero
//       times: the temporary file was discarded with the process. Both reverted.
class Capture {
public:
    Capture() {
        m_file = std::tmpfile();
        if (!m_file) {
            m_initErrno = errno;
            return;
        }
        m_fd = fileno(m_file);
        if (m_fd < 0) {
            m_initErrno = errno;
            return;
        }
        m_realStderr = dup(STDERR_FILENO);
        if (m_realStderr < 0) {
            m_initErrno = errno;
            return;
        }
        if (dup2(m_fd, STDERR_FILENO) < 0) {
            m_initErrno = errno;
            return;
        }
        m_active = true;
        g_instance = this;
        installDeathHandlers();
    }

    ~Capture() {
        if (!m_active) return;
        drain();
        std::fflush(stderr);
        dup2(m_realStderr, STDERR_FILENO);
        close(m_realStderr);
        std::fclose(m_file);
        g_instance = nullptr;
    }

    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

    void reset() {
        requireActive();
        drain();
        m_count = 0;
    }

    int count() {
        requireActive();
        drain();
        return m_count;
    }

    // Byte-for-byte copy with no counting and no allocation, so it is safe from a signal
    // handler. Deliberately does not touch m_pending or m_count: a handler runs at an
    // arbitrary point and must not leave those half-updated for a resumed thread.
    void drainRaw() {
        if (!m_active) return;
        char buffer[4096];
        while (true) {
            const ssize_t n = pread(m_fd, buffer, sizeof(buffer), static_cast<off_t>(m_rawOffset));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            m_rawOffset += static_cast<size_t>(n);
            writeAll(buffer, static_cast<size_t>(n));
        }
    }

private:
    // A capture that never installed is not a run with no warnings, and must never be
    // reported as one. Every caller of count() treats the result as the suite's only
    // GPU-side oracle, so a silent 0 here converts a harness fault into a green run
    // across eight binaries at once.
    void requireActive() {
        if (m_active) return;
        // stderr is untouched on every failure path, so this reaches the terminal.
        std::fprintf(stderr,
                     "FATAL: the validation stderr capture could not be installed (%s). The "
                     "zero-validation-warning oracle cannot run, and reporting 0 warnings here would be "
                     "indistinguishable from a clean run.\n",
                     std::strerror(m_initErrno));
        std::fflush(nullptr);
        std::_Exit(1);
    }

    // Reads everything written since the last call, counts the warnings among it, and
    // forwards it to the real stderr. pread() is used so the shared write offset that
    // STDERR_FILENO points at is never disturbed -- a plain read() would move it and the
    // next message would overwrite what was just consumed.
    void drain() {
        std::fflush(stderr);
        char buffer[4096];
        while (true) {
            const ssize_t n = pread(m_fd, buffer, sizeof(buffer), static_cast<off_t>(m_offset));
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) {
                // Distinguished from EOF deliberately: treating a hard read error as
                // "nothing more to read" would stop counting for the rest of the run and
                // leave every later check passing on an unread file.
                std::fprintf(stderr, "FATAL: could not read the validation capture (%s)\n", std::strerror(errno));
                std::fflush(nullptr);
                std::_Exit(1);
            }
            if (n == 0) break;
            m_offset += static_cast<size_t>(n);
            m_pending.append(buffer, static_cast<size_t>(n));
            writeAll(buffer, static_cast<size_t>(n));
        }
        if (m_offset > m_rawOffset) m_rawOffset = m_offset;

        size_t start = 0;
        while (true) {
            const size_t eol = m_pending.find('\n', start);
            if (eol == std::string::npos) break;
            countLine(m_pending.substr(start, eol - start));
            start = eol + 1;
        }
        m_pending.erase(0, start);
    }

    void countLine(const std::string& line) {
        if (line.compare(0, std::strlen(kWarningPrefix), kWarningPrefix) == 0 &&
            line.find(kNotAWarning) == std::string::npos) {
            ++m_count;
        }
    }

    void writeAll(const char* data, size_t size) {
        size_t written = 0;
        while (written < size) {
            const ssize_t n = write(m_realStderr, data + written, size - written);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return;  // the terminal is gone; nothing useful is left to do
            written += static_cast<size_t>(n);
        }
    }

    // The signals that end a probe without unwinding. SIGABRT above all: it is how the
    // affinity guard is *designed* to fire, and how an assertion failure or a validation
    // abort arrives, so it is the single most important case for the output to survive.
    static void installDeathHandlers() {
        for (const int sig : {SIGABRT, SIGSEGV, SIGBUS, SIGILL, SIGFPE}) {
            struct sigaction sa {};
            sa.sa_handler = &Capture::onDeadlySignal;
            sigemptyset(&sa.sa_mask);
            sa.sa_flags = SA_RESETHAND;  // re-raise below hits the default disposition
            sigaction(sig, &sa, nullptr);
        }
    }

    static void onDeadlySignal(int sig) {
        if (g_instance) g_instance->drainRaw();
        std::raise(sig);
    }

    std::FILE* m_file = nullptr;
    int m_fd = -1;
    int m_realStderr = -1;
    bool m_active = false;
    int m_initErrno = 0;
    size_t m_offset = 0;     // consumed by drain(), which also counts
    size_t m_rawOffset = 0;  // consumed by drainRaw(), which only copies
    int m_count = 0;
    std::string m_pending;  // bytes read but not yet terminated by a newline
};

// Constructed before main, so the capture is in place before any Device exists. A
// function-local static would be installed on first use, which is after the Vulkan
// output a test wants to count.
Capture g_capture;

}  // namespace

void resetValidationWarningCount() { g_capture.reset(); }

int validationWarningCount() { return g_capture.count(); }

void drainValidationCapture() { g_capture.drainRaw(); }

bool validationLayerActive() {
    uint32_t count = 0;
    VkResult result = vkEnumerateInstanceLayerProperties(&count, nullptr);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) return false;
    std::vector<VkLayerProperties> layers(count);
    result = vkEnumerateInstanceLayerProperties(&count, layers.data());
    // VK_INCOMPLETE is a normal outcome, not a failure: a layer registering between the
    // two calls leaves the array truncated but valid. Treating it as "no layers" made
    // this report the validation layer missing on a host where it is installed, with an
    // instruction to install it that could not help.
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) return false;
    layers.resize(count);
    for (const VkLayerProperties& layer : layers) {
        if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0) return true;
    }
    return false;
}

}  // namespace wallpaperx
