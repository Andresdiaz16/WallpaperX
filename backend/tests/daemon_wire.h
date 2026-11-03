// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "daemon/daemon_bus.h"

namespace wallpaperx::testing {

// The bus name, path and interface the plugin talks to, from the one definition the daemon and the
// plugin share. dbus_service_test.cpp (S6.1) pins those to their documented strings.
inline constexpr const char* kService = kDaemonBusName;
inline constexpr const char* kPath = kDaemonObjectPath;
inline constexpr const char* kInterface = kDaemonInterface;

}  // namespace wallpaperx::testing
