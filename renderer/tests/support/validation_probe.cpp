#include "support/validation_probe.h"

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <cerrno>

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

// Redirects stderr to a temporary file, counts the validation warnings written to it,
// and copies everything through to the real stderr as it is read.
//
// WHY A FILE AND NO THREAD. The obvious shape is a pipe with a reader thread forwarding
// bytes live. That version deadlocked: renderer/tests/device_affinity_probe.cpp forks,
// and fork() in a process with a second thread gives the child a copy of a heap whose
// allocator lock that thread may have been holding -- the child then wedges in its first
// malloc, the parent blocks reading a pipe that never reaches EOF, and the probe hangs
// forever (observed 3 runs out of 3, killed at 25 s). A file needs no reader running
// concurrently with the fork, so there is no second thread to inherit a lock from.
//
// WHY NOT READ THE CALLBACK, which would be simpler still: the callback is production
// code in device.cpp, and giving it a test-visible sink is precisely the test-only hook
// this file exists to avoid. stderr is an interface production already commits to.
//
// The cost is that messages reach the terminal in batches -- at each count() and at
// exit -- rather than the instant they are printed. Ordering among them is preserved,
// and ctest reads the process's output at the end regardless.
class Capture {
public:
    Capture() {
        m_file = std::tmpfile();
        if (!m_file) return;
        m_fd = fileno(m_file);
        if (m_fd < 0) return;
        m_realStderr = dup(STDERR_FILENO);
        if (m_realStderr < 0) return;
        if (dup2(m_fd, STDERR_FILENO) < 0) return;
        m_active = true;
    }

    ~Capture() {
        if (!m_active) return;
        drain();
        std::fflush(stderr);
        dup2(m_realStderr, STDERR_FILENO);
        close(m_realStderr);
        std::fclose(m_file);
    }

    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

    void reset() {
        if (!m_active) return;
        drain();
        m_count = 0;
    }

    int count() {
        if (!m_active) return 0;
        drain();
        return m_count;
    }

private:
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
            if (n <= 0) break;
            m_offset += static_cast<size_t>(n);
            m_pending.append(buffer, static_cast<size_t>(n));
            forward(buffer, static_cast<size_t>(n));
        }

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

    void forward(const char* data, size_t size) {
        size_t written = 0;
        while (written < size) {
            const ssize_t n = write(m_realStderr, data + written, size - written);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return;
            written += static_cast<size_t>(n);
        }
    }

    std::FILE* m_file = nullptr;
    int m_fd = -1;
    int m_realStderr = -1;
    bool m_active = false;
    size_t m_offset = 0;
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

bool validationLayerActive() {
    uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) return false;
    std::vector<VkLayerProperties> layers(count);
    if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS) return false;
    for (const VkLayerProperties& layer : layers) {
        if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0) return true;
    }
    return false;
}

}  // namespace wallpaperx
