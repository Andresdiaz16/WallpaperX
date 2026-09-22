// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <utility>
#include <variant>

namespace wallpaperx {

// Opens a wallpaper's own file for reading without ever parking the calling thread in the kernel,
// which is what a named pipe with no writer, a device or a socket do to an ordinary open -- and
// every renderer here runs on the daemon's one GPU thread, so one such path stops every monitor.
// O_NONBLOCK covers the open and the descriptor is then what says what was opened: only a regular
// file is accepted, and reading through the descriptor rather than opening the path a second time
// leaves no moment for the path to become something else.
//
// A path validated when the wallpaper was chosen is not enough on its own: the file is opened again
// on every render, when a stopped video's session is reopened, and when a screen reports a new size,
// and the path may name something different by then. So a file on a refused filesystem -- see
// refusedFilesystemOf() -- is refused here as well as when it is chosen.
//
// Throws ResourceExhausted when the process or the system is out of descriptors or kernel memory --
// the daemon's shortage, not the file's. Every other answer is returned, so a caller words it.
struct WallpaperOpen {
    struct Opened {
        int fd;  // the caller's to close
    };
    struct Refused {
        std::string reason;  // for describeRejection(): not a regular file, or a refused filesystem
    };
    struct Failed {
        int error;  // errno from the open or the fstat
    };
    std::variant<Opened, Refused, Failed> result;

    WallpaperOpen(Opened opened) : result(opened) {}
    WallpaperOpen(Refused refused) : result(std::move(refused)) {}
    WallpaperOpen(Failed failed) : result(failed) {}
};
WallpaperOpen openWallpaper(const std::string& file, const char* component);

// openWallpaper() for a renderer: -1 with errno set, as open(2) does, so a caller can phrase that
// failure in its own words, and WallpaperContentError, worded by describeRejection(), for a refusal.
int openWallpaperFile(const std::string& file, const char* component);

// The type of the filesystem holding `path` when wallpapers are not read from it -- a network one
// such as "nfs4" or "cifs", or any FUSE mount not on a short list of local ones -- or empty when it is
// read from. Answered from /proc's mount table, and every symlink on the way is resolved against
// that table before anything inside it is touched, so a mount whose server has gone is refused
// rather than asked and waited on. Empty when the table cannot be read, since nothing else can
// answer before the lookup; openWallpaper() then asks the opened file's filesystem its type, and
// refuses every FUSE mount, since that answer carries no subtype to allow. Throws
// ResourceExhausted, naming `component`, when that is because the process is out of descriptors
// or memory.
std::string refusedFilesystemOf(const std::string& path, const char* component);

// Why a file on the filesystem `type` is refused, for a message that then names the path.
std::string refusedFilesystemReason(const std::string& type);

// How a rejection naming a wallpaper file is worded: `component: reason: path`. The path goes last
// because the daemon keeps only the head of a recorded error, and a long path ahead of the reason
// would be all that survived. Build every such message here rather than by hand.
std::string describeRejection(const char* component, const std::string& reason, const std::string& path);

}  // namespace wallpaperx
