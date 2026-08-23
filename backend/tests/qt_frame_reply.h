// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The ten fields GetFrameFd answers with, marshalled in one place for the Qt-side stub daemons.
//
// Three tests stand a stub in front of WallpaperXItem -- one serving a frame the importer must
// refuse, two serving frames it must accept -- and the point of all three is that the item reads
// the DAEMON'S reply shape. Hand-written copies of a ten-field signature are things that can
// disagree, and the one that drifts still compiles, still runs and still reads as coverage.
//
// Deliberately not an adaptor over a C++ signature: the field order and the exact D-Bus types are
// what is under test, so Qt's marshalling of a method signature must not sit between the stub and
// the wire.

#include <QDBusArgument>
#include <QDBusMetaType>
#include <QDBusUnixFileDescriptor>
#include <QList>
#include <QVariant>
#include <QtGlobal>

namespace wallpaperx::testing {

// The per-plane (offset, rowPitch) struct the tenth field carries.
struct FramePlane {
    quint64 offset = 0;
    quint64 pitch = 0;
};

inline QDBusArgument& operator<<(QDBusArgument& arg, const FramePlane& plane) {
    arg.beginStructure();
    arg << plane.offset << plane.pitch;
    arg.endStructure();
    return arg;
}

inline const QDBusArgument& operator>>(const QDBusArgument& arg, FramePlane& plane) {
    arg.beginStructure();
    arg >> plane.offset >> plane.pitch;
    arg.endStructure();
    return arg;
}

}  // namespace wallpaperx::testing

Q_DECLARE_METATYPE(wallpaperx::testing::FramePlane)

namespace wallpaperx::testing {

// Call once before a stub answers anything: without the plane types registered the reply
// marshals as a variant Qt cannot write, and the item sees a short reply instead of a frame.
inline void registerFrameReplyTypes() {
    qDBusRegisterMetaType<FramePlane>();
    qDBusRegisterMetaType<QList<FramePlane>>();
    qDBusRegisterMetaType<QList<QDBusUnixFileDescriptor>>();
}

// GetFrameFd's out arguments, in the order the daemon declares them:
// fd, width, height, memory_size, handle_type, sync_fds, frame_generation, drm_format,
// drm_modifier, planes.
//
// sync_fds defaults to empty because that is what the daemon sends, but it IS a parameter: the
// item refuses a reply carrying any, and a helper that cannot express one leaves that refusal
// unreachable from every test -- the branch would exist with nothing able to enter it.
inline QList<QVariant> frameReplyFields(const QDBusUnixFileDescriptor& handle, quint32 width, quint32 height,
                                        quint64 memorySize, int handleType, quint64 generation, quint32 drmFormat,
                                        quint64 drmModifier, const QList<FramePlane>& planes,
                                        const QList<QDBusUnixFileDescriptor>& syncFds = {}) {
    return {QVariant::fromValue(handle),
            QVariant::fromValue(width),
            QVariant::fromValue(height),
            QVariant::fromValue(memorySize),
            QVariant::fromValue(handleType),
            QVariant::fromValue(syncFds),
            QVariant::fromValue(generation),
            QVariant::fromValue(drmFormat),
            QVariant::fromValue(drmModifier),
            QVariant::fromValue(planes)};
}

}  // namespace wallpaperx::testing
