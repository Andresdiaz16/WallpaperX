// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The daemon's identity on the session bus and the error names it answers with: a contract with
// separately built programs, so it lives here with no dependencies, for the daemon and the Plasma
// plugin to share.

namespace wallpaperx {

inline constexpr const char* kDaemonBusName = "io.wallpaperx.Daemon";
inline constexpr const char* kDaemonObjectPath = "/io/wallpaperx/Daemon";
inline constexpr const char* kDaemonInterface = "io.wallpaperx.Daemon";

// Error names the client can branch on. NoFrame is an ordinary state (nothing has been set on that
// monitor yet), not a malfunction -- keeping it distinct from ExportFailed is what lets a polling
// consumer tell "not ready" from "broken".
inline constexpr const char* kErrorNoFrame = "io.wallpaperx.Daemon.Error.NoFrame";
inline constexpr const char* kErrorExportFailed = "io.wallpaperx.Daemon.Error.ExportFailed";
inline constexpr const char* kErrorRenderFailed = "io.wallpaperx.Daemon.Error.RenderFailed";
// The third answer to "why is there no frame", separate because it calls for a third response:
// NoFrame means retry, RenderFailed means stop and show the reason, Unsupported means stop asking --
// nothing about this build changes while it runs.
inline constexpr const char* kErrorUnsupported = "io.wallpaperx.Daemon.Error.Unsupported";
// The fourth: there is a frame, but handing it over would leave the daemon nowhere to draw the next
// one. Retryable like NoFrame, and the daemon says when -- a FrameReady follows every release and
// every commit, so a consumer that waits for one is not polling.
inline constexpr const char* kErrorNoFreeImage = "io.wallpaperx.Daemon.Error.NoFreeImage";
// The fifth, and the only one that is the caller's own doing: it has been sent as many descriptors
// as it may hold without giving any back. Nothing announces the cure, since only the caller can
// apply it -- a release, or leaving the bus.
inline constexpr const char* kErrorTooManyDescriptors = "io.wallpaperx.Daemon.Error.TooManyDescriptors";
// The sixth, also the caller's own doing: it already has its share of the worker's queue waiting.
// Distinct from Busy, which is everyone's queue being full.
inline constexpr const char* kErrorTooManyRequests = "io.wallpaperx.Daemon.Error.TooManyRequests";
// The seventh, also the caller's own doing: it has been sent every copy of its acquire timeline one
// connection is given. Not retryable on that connection.
inline constexpr const char* kErrorTooManyTimelineCopies = "io.wallpaperx.Daemon.Error.TooManyTimelineCopies";
inline constexpr const char* kErrorShuttingDown = "io.wallpaperx.Daemon.Error.ShuttingDown";
inline constexpr const char* kErrorBusy = "io.wallpaperx.Daemon.Error.Busy";
// A call about a screen no monitor answers to. Not InvalidArgs: nothing in the call was wrong, and
// the same call succeeds once SetMonitorOutput has named the screen.
inline constexpr const char* kErrorUnknownOutput = "io.wallpaperx.Daemon.Error.UnknownOutput";

}  // namespace wallpaperx
