// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The ten fields GetFrameFd answers with, marshalled in one place for the Qt-side stub daemons.
//
// Two tests now stand a stub in front of WallpaperXItem -- one serving a frame the importer must
// refuse, one serving a frame it must accept -- and the point of both is that the item reads the
// DAEMON'S reply shape. Two hand-written copies of a ten-field signature is two things that can
// disagree, and the one that drifts still compiles, still runs and still reads as coverage; the
// same duplication in the client-side wire helpers has already shipped half-applied once.
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
// sync_fds is not a parameter because the daemon sends none and the item REFUSES a reply that
// carries any -- a stub that could pass some would be exercising a path the daemon does not have.
inline QList<QVariant> frameReplyFields(const QDBusUnixFileDescriptor& handle, quint32 width, quint32 height,
                                        quint64 memorySize, int handleType, quint64 generation, quint32 drmFormat,
                                        quint64 drmModifier, const QList<FramePlane>& planes) {
    return {QVariant::fromValue(handle),
            QVariant::fromValue(width),
            QVariant::fromValue(height),
            QVariant::fromValue(memorySize),
            QVariant::fromValue(handleType),
            QVariant::fromValue(QList<QDBusUnixFileDescriptor>{}),
            QVariant::fromValue(generation),
            QVariant::fromValue(drmFormat),
            QVariant::fromValue(drmModifier),
            QVariant::fromValue(planes)};
}

}  // namespace wallpaperx::testing
