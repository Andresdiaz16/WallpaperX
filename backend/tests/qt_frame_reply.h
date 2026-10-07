// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The fourteen fields GetFrame answers with, marshalled in one place for the Qt-side stubs.
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

#include <vector>

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

// GetFrame's out arguments, in the order the daemon declares them:
// memory_fd, width, height, memory_size, handle_type, sync_fds, frame_generation, drm_format,
// drm_modifier, planes, device_uuid, buffer_id, acquire_value, handover.
//
// `memory` is an array because the daemon leaves the memory out for an image the request said
// the caller can still draw; a list of one is the ordinary reply.
//
// deviceUuid and handover have no default: the item refuses a reply with the wrong field count
// before reading any of it, so a helper that could omit one would turn every caller's chosen
// refusal into that one instead, and still pass; and a defaulted hand-over number would let a stub
// serve every reply as number 0, which no daemon sends and a release could not name.
inline QList<QVariant> frameReplyFields(const QList<QDBusUnixFileDescriptor>& memory, quint32 width, quint32 height,
                                        quint64 memorySize, int handleType, quint64 generation, quint32 drmFormat,
                                        quint64 drmModifier, const QList<FramePlane>& planes,
                                        const QByteArray& deviceUuid, quint64 bufferId, quint64 handover,
                                        const QList<QDBusUnixFileDescriptor>& syncFds = {},
                                        quint64 acquireValue = 0) {
    return {QVariant::fromValue(memory),
            QVariant::fromValue(width),
            QVariant::fromValue(height),
            QVariant::fromValue(memorySize),
            QVariant::fromValue(handleType),
            QVariant::fromValue(syncFds),
            QVariant::fromValue(generation),
            QVariant::fromValue(drmFormat),
            QVariant::fromValue(drmModifier),
            QVariant::fromValue(planes),
            QVariant::fromValue(deviceUuid),
            QVariant::fromValue(bufferId),
            QVariant::fromValue(static_cast<qulonglong>(acquireValue)),
            QVariant::fromValue(static_cast<qulonglong>(handover))};
}

// ReleaseBuffer's in arguments as a stub receives them. `ok` is false for a call carrying anything
// but (i monitor_id, t buffer_id, t handover), which a stub must count rather than read as a release.
struct ReleaseArgs {
    bool ok = false;
    int monitorId = -1;
    quint64 bufferId = 0;
    quint64 handover = 0;
};

inline ReleaseArgs readReleaseArgs(const QList<QVariant>& args) {
    ReleaseArgs out;
    if (args.size() != 3 || args.at(0).typeId() != QMetaType::Int || args.at(1).typeId() != QMetaType::ULongLong ||
        args.at(2).typeId() != QMetaType::ULongLong) {
        return out;
    }
    out.monitorId = args.at(0).toInt();
    out.bufferId = args.at(1).toULongLong();
    out.handover = args.at(2).toULongLong();
    out.ok = true;
    return out;
}

// GetFrame's in arguments as a stub receives them: the images the caller says it can still draw,
// and whether it asked on its timeline. `ok` is false for a request that is not (i monitor_id,
// at known_buffer_ids, b on_timeline) exactly -- the daemon refuses any other signature -- which a
// stub must answer as such rather than serve as if nothing were declared.
struct FrameRequestArgs {
    bool ok = false;
    int monitorId = -1;
    std::vector<quint64> declared;
    bool onTimeline = false;
};

// An `at` argument (an array of unsigned 64-bit integers) into `out`; false, reading nothing, for
// any other signature. The one decoder of it, so the two parsers below cannot disagree on what is
// malformed.
inline bool readUint64Array(const QDBusArgument& list, std::vector<quint64>& out) {
    if (list.currentSignature() != QLatin1String("at")) return false;
    list.beginArray();
    while (!list.atEnd()) {
        quint64 value = 0;
        list >> value;
        out.push_back(value);
    }
    list.endArray();
    return true;
}

inline FrameRequestArgs readFrameRequest(const QList<QVariant>& args) {
    FrameRequestArgs out;
    if (args.size() != 3 || args.at(0).typeId() != QMetaType::Int || !args.at(1).canConvert<QDBusArgument>() ||
        args.at(2).typeId() != QMetaType::Bool) {
        return out;
    }
    out.monitorId = args.at(0).toInt();
    if (!readUint64Array(args.at(1).value<QDBusArgument>(), out.declared)) return out;
    out.onTimeline = args.at(2).toBool();
    out.ok = true;
    return out;
}

// SetImportModifiers' in arguments as a stub receives them: `ok` is false for anything but
// (u drm_format, at modifiers), the daemon's signature.
struct ModifierReportArgs {
    bool ok = false;
    quint32 drmFormat = 0;
    std::vector<quint64> modifiers;
};

inline ModifierReportArgs readModifierReport(const QList<QVariant>& args) {
    ModifierReportArgs out;
    if (args.size() != 2 || args.at(0).typeId() != QMetaType::UInt || !args.at(1).canConvert<QDBusArgument>()) {
        return out;
    }
    if (!readUint64Array(args.at(1).value<QDBusArgument>(), out.modifiers)) return out;
    out.drmFormat = args.at(0).toUInt();
    out.ok = true;
    return out;
}

}  // namespace wallpaperx::testing
