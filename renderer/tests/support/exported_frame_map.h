// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Test-only support for reading an exported frame's bytes back through nothing but
// its file descriptor.
//
// Header-only, and deliberately outside the renderer library: nothing WallpaperX ships
// ever CPU-maps an exported frame. A real consumer imports the fd into a graphics API,
// which is why every field the export path publishes (plane offset, rowPitch, fourcc,
// modifier) is metadata *for someone else* and therefore not checkable from inside the
// exporter. This exists so tests can hold the published layout to account against the
// actual pixels, on the one path where the CPU can address them.
//
// Shared between renderer/tests/frame_export_probe.cpp (which checks the export in
// isolation) and backend/tests/dbus_service_test.cpp (which checks what survives a
// round trip over DBus) -- the same twenty lines of mmap/ioctl bookkeeping, and a
// second copy of it would be a second thing to get wrong.

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>

namespace wallpaperx::testing {

// DRM_FORMAT_MOD_LINEAR. Zero is a *valid* modifier, not a sentinel -- the
// not-populated sentinel is kDrmModifierInvalid (see frame_export.h). Only under this
// modifier are an image's rows CPU-addressable at a constant stride; a tiled or
// compressed modifier reports a rowPitch that is not a linear row length, so mapping
// one and comparing bytes would fail a perfectly correct export.
inline constexpr uint64_t kDrmModifierLinear = 0;

// DMA_BUF_IOCTL_SYNC, from linux/dma-buf.h. Redefined here for the same reason the DRM
// fourcc codes are in frame_export.h: it saves a build dependency on kernel headers for
// three ABI-frozen constants. Bracketing a CPU read of a dma_buf with START/END is what
// makes the mapping coherent with the device's writes; skipping it reads stale bytes
// wherever the buffer is not CPU-cache-coherent, which is most places.
struct DmaBufSyncArgs {
    uint64_t flags = 0;
};
inline constexpr uint64_t kDmaBufSyncRead = 1 << 0;
inline constexpr uint64_t kDmaBufSyncStart = 0 << 2;
inline constexpr uint64_t kDmaBufSyncEnd = 1 << 2;

// What the kernel calls the object behind an fd -- "/dmabuf:", "/memfd:name", and so
// on. Diagnostic only: which of these a driver returns for a DMA_BUF export is not
// contractual, so callers print it and never assert on it.
inline std::string fdDescription(int fd) {
    const std::string link = "/proc/self/fd/" + std::to_string(fd);
    char buf[256];
    const ssize_t n = readlink(link.c_str(), buf, sizeof(buf) - 1);
    if (n < 0) return "<unreadable>";
    buf[n] = '\0';
    return buf;
}

// A read-only CPU mapping of an exported frame, synced for host reads for its lifetime.
//
// Construction does not throw: a driver refusing to let the CPU map its dma_buf is a
// policy choice, not a defect (a GPU importer never needs the mapping), so callers skip
// on !valid() rather than fail. They must say so out loud, though -- a silent skip here
// is the whole vacuous-green failure mode this class exists to avoid.
class MappedFrame {
public:
    MappedFrame(int fd, uint64_t size) : m_fd(fd), m_size(static_cast<size_t>(size)) {
        void* mapped = mmap(nullptr, m_size, PROT_READ, MAP_SHARED, fd, 0);
        if (mapped == MAP_FAILED) {
            m_error = std::strerror(errno);
            return;
        }
        m_base = static_cast<const unsigned char*>(mapped);
        sync(kDmaBufSyncStart | kDmaBufSyncRead);
    }

    ~MappedFrame() {
        if (!m_base) return;
        sync(kDmaBufSyncEnd | kDmaBufSyncRead);
        munmap(const_cast<unsigned char*>(m_base), m_size);
    }

    MappedFrame(const MappedFrame&) = delete;
    MappedFrame& operator=(const MappedFrame&) = delete;

    bool valid() const { return m_base != nullptr; }
    const std::string& error() const { return m_error; }

    // The first byte of row `y` of the plane starting at `planeOffset` with stride
    // `rowPitch`. Returns nullptr if the row -- as described by the frame's own
    // published layout -- would fall outside the memory that was actually exported,
    // which is itself a finding and not something to read past.
    //
    // The bound is computed by subtraction and division rather than by forming
    // `planeOffset + y*rowPitch + rowBytes` and comparing. That sum is unsigned 64-bit
    // arithmetic over values that arrive straight off the DBus wire, so a daemon
    // regression (or anything else publishing a plane offset or pitch near 2^64) makes
    // it wrap to a small number, sail through the comparison, and hand back a pointer
    // far outside the mapping. Each step below is bounded by m_size before it happens,
    // so nothing can wrap.
    const unsigned char* row(uint64_t planeOffset, uint64_t rowPitch, uint32_t y, size_t rowBytes) const {
        if (!m_base) return nullptr;
        if (rowBytes > m_size) return nullptr;
        const uint64_t maxStart = m_size - rowBytes;
        if (planeOffset > maxStart) return nullptr;
        // rowPitch == 0 is legal for a plane whose contents are not row-addressable; the
        // division would trap on it, and every row of such a plane starts at planeOffset.
        if (rowPitch != 0 && static_cast<uint64_t>(y) > (maxStart - planeOffset) / rowPitch) return nullptr;
        return m_base + planeOffset + static_cast<uint64_t>(y) * rowPitch;
    }

private:
    void sync(uint64_t flags) {
        DmaBufSyncArgs args{flags};
        // Best-effort. ENOTTY means the fd is not a real dma_buf (Lavapipe can hand back
        // a memfd), whose mapping is coherent by construction. Any other failure is
        // equally non-fatal: the byte comparison is the actual assertion, and a missed
        // sync surfaces there as a mismatch rather than as a pass.
        ioctl(m_fd, _IOW('b', 0, DmaBufSyncArgs), &args);
    }

    int m_fd = -1;
    size_t m_size = 0;
    const unsigned char* m_base = nullptr;
    std::string m_error;
};

}  // namespace wallpaperx::testing
