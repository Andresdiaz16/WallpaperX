// SPDX-License-Identifier: GPL-2.0-or-later

// S15.970, compiled by the app_kscreen_client_header entry: kscreen_client.h stands on its own
// without the D-Bus source.
#include "kscreen_client.h"

#include <QDBusConnection>

#include <type_traits>

namespace wallpaperx {
// GOAL: S15.970 -- kscreen_client.h does not include kscreen_dbus_source.h, directly or through
//       another header.
// WHY-THIS-SHAPE: the header's own class is redefined here, so any path that brings it in is a
//       redefinition error; a grep of the one header would miss an include through another. Keyed on
//       KScreenDBusSource, not on a helper type that could move to a shared header.
// CONTROL: app/kde/kscreen_client.h given `#include "kscreen_dbus_source.h"` -> red, "redefinition
//       of 'class wallpaperx::KScreenDBusSource'" (control_batch.sh 2026-10-26).
class KScreenDBusSource {};
} // namespace wallpaperx

// GOAL: S15.970 -- KScreenClient has no constructor taking a D-Bus connection.
// CONTROL: app/kde/kscreen_client.h given `KScreenClient(QDBusConnection bus, const QString& service,
//       QObject* parent = nullptr);` -> red, "static assertion failed" (control_batch.sh 2026-10-26).
static_assert(!std::is_constructible_v<wallpaperx::KScreenClient, QDBusConnection>);
static_assert(!std::is_constructible_v<wallpaperx::KScreenClient, QDBusConnection, QString>);
