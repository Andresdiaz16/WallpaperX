// SPDX-License-Identifier: GPL-2.0-or-later

#include "wallpaper_file.h"

#include "vulkan/gpu_fault.h"

#include <fcntl.h>
#include <linux/magic.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cstdint>
#include <cstring>
#include <deque>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace wallpaperx {

namespace {

// Kernel filesystems whose reads go over a network, so a server that stops answering leaves them
// waiting, often for ever.
constexpr std::string_view kNetworkTypes[] = {
    "nfs",   "nfs4",   "cifs",   "smb3",     "smbfs",  "ncpfs", "afs",       "ceph",  "9p",
    "coda",  "lustre", "gpfs",   "beegfs",   "orangefs", "pvfs2", "glusterfs", "davfs",
};

// The FUSE filesystems read from: disk drivers (NTFS, exFAT, btrfs) and the encrypting and pooling
// layers people keep local folders on. The layers can sit over a network folder too, and are
// allowed anyway. Every other FUSE subtype is refused, known or not: a FUSE mount is a program
// answering each read, and the kernel cannot say whether a network is behind it -- sshfs, rclone,
// gvfs and kio-fuse are, and so may any tool not named here.
constexpr std::string_view kLocalFuseTypes[] = {
    "fuseblk",       "fuse.ntfs-3g", "fuse.lowntfs-3g", "fuse.ntfs",    "fuse.exfat",  "fuse.exfat-fuse",
    "fuse.btrfs-fuse", "fuse.btrfs", "fuse.gocryptfs",  "fuse.encfs",   "fuse.cryfs",  "fuse.mergerfs",
    "fuse.bindfs",
};

constexpr std::string_view kFusePrefix = "fuse";

// The type as a message may show it, or nothing for one that is read from. A FUSE subtype is the
// mounting program's choice, so one that is not plainly a name is shown as "fuse" alone.
std::optional<std::string> refusedType(std::string_view type) {
    if (const auto* found = std::find(std::begin(kNetworkTypes), std::end(kNetworkTypes), type);
        found != std::end(kNetworkTypes)) {
        return std::string(*found);
    }
    if (!type.starts_with(kFusePrefix)) return std::nullopt;
    if (std::find(std::begin(kLocalFuseTypes), std::end(kLocalFuseTypes), type) != std::end(kLocalFuseTypes)) {
        return std::nullopt;
    }
    const bool plain = type.size() <= 64 && std::all_of(type.begin(), type.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ||
               c == '_';
    });
    return plain ? std::string(type) : std::string(kFusePrefix);
}

struct Mount {
    int id = 0;
    int parent = 0;
    std::string point;
    dev_t device = 0;
    std::string type;
};

// mountinfo writes space, tab, newline and backslash in a path as three octal digits.
std::string unescapeMountField(std::string_view field) {
    std::string out;
    out.reserve(field.size());
    const auto octal = [](char c) { return c >= '0' && c <= '7'; };
    for (size_t i = 0; i < field.size(); ++i) {
        if (field[i] == '\\' && field.size() - i > 3 && octal(field[i + 1]) && octal(field[i + 2]) &&
            octal(field[i + 3])) {
            out += static_cast<char>((field[i + 1] - '0') * 64 + (field[i + 2] - '0') * 8 + (field[i + 3] - '0'));
            i += 3;
        } else {
            out += field[i];
        }
    }
    return out;
}

std::optional<Mount> parseMountLine(std::string_view line) {
    std::vector<std::string_view> fields;
    for (size_t start = 0; start <= line.size();) {
        const size_t end = std::min(line.find(' ', start), line.size());
        fields.push_back(line.substr(start, end - start));
        start = end + 1;
    }
    // id, parent, major:minor, root, mount point, options, optional fields, "-", type, source, ...
    const auto separator = std::find(fields.begin() + std::min<size_t>(fields.size(), 6), fields.end(), "-");
    if (fields.size() < 6 || separator == fields.end() || separator + 1 == fields.end()) return std::nullopt;

    const auto number = [](std::string_view text, auto& out) {
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), out);
        return parsed.ec == std::errc() && parsed.ptr == text.data() + text.size();
    };
    Mount mount;
    const std::string_view numbers = fields[2];
    const size_t colon = numbers.find(':');
    unsigned major = 0;
    unsigned minor = 0;
    if (colon == std::string_view::npos || !number(fields[0], mount.id) || !number(fields[1], mount.parent) ||
        !number(numbers.substr(0, colon), major) || !number(numbers.substr(colon + 1), minor)) {
        return std::nullopt;
    }
    mount.point = unescapeMountField(fields[4]);
    mount.device = makedev(major, minor);
    mount.type = std::string(*(separator + 1));
    return mount;
}

bool isShortage(int err) { return err == EMFILE || err == ENFILE || err == ENOMEM; }

// Procfs, so reading it asks no filesystem anything: a mount whose server is gone is listed like any
// other. Nothing when it cannot be read -- a sandbox may hide it -- and ResourceExhausted when that
// is the daemon's shortage, since a descriptor is then also what the wallpaper's own open lacks.
std::optional<std::vector<Mount>> readMountTable(const char* component) {
    const auto unreadable = [component](int err) -> std::nullopt_t {
        if (isShortage(err)) {
            throw ResourceExhausted(std::string(component) + ": cannot tell what filesystem the wallpaper is on, "
                                    "the mount table is unreadable (" + std::strerror(err) + ")");
        }
        return std::nullopt;
    };
    const int fd = ::open("/proc/self/mountinfo", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return unreadable(errno);
    std::string text;
    char buffer[8192];
    for (;;) {
        const ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            const int failed = errno;
            ::close(fd);
            return unreadable(failed);
        }
        if (n == 0) break;
        text.append(buffer, static_cast<size_t>(n));
    }
    ::close(fd);

    std::vector<Mount> mounts;
    for (size_t start = 0; start < text.size();) {
        const size_t end = std::min(text.find('\n', start), text.size());
        if (std::optional<Mount> mount = parseMountLine(std::string_view(text).substr(start, end - start))) {
            mounts.push_back(std::move(*mount));
        }
        start = end + 1;
    }
    return mounts;
}

// This process's root: the last mount on "/" whose parent is not listed. Null when there is none.
const Mount* rootMount(const std::vector<Mount>& mounts) {
    std::unordered_set<int> ids;
    for (const Mount& mount : mounts) ids.insert(mount.id);
    const Mount* root = nullptr;
    for (const Mount& mount : mounts) {
        if (mount.point == "/" && ids.count(mount.parent) == 0) root = &mount;
    }
    return root;
}

// The refused type of the mount the absolute, resolved `path` lies in, found as the kernel finds it:
// down the mount tree from `root`, at each directory crossing into the topmost mount on it whose
// parent is the mount already reached. A mount a later one has covered is never reached.
std::optional<std::string> refusedTypeAt(const std::vector<Mount>& mounts, const Mount& root, std::string_view path) {
    const Mount* current = &root;

    // Mounts stacked on one directory each name the one below as parent; later entries are newer.
    const auto climb = [&mounts, &current](std::string_view point) {
        for (bool moved = true; moved;) {
            moved = false;
            for (const Mount& mount : mounts) {
                if (&mount != current && mount.parent == current->id && mount.point == point) {
                    current = &mount;
                    moved = true;
                }
            }
        }
    };
    climb("/");
    for (size_t slash = path.find('/', 1); ; slash = path.find('/', slash + 1)) {
        const std::string_view prefix = path.substr(0, slash);
        if (prefix.size() > 1) climb(prefix);
        if (slash == std::string_view::npos) break;
    }
    return refusedType(current->type);
}

void prependComponents(std::deque<std::string>& pending, std::string_view path) {
    std::vector<std::string> parts;
    for (size_t start = 0; start <= path.size();) {
        const size_t end = std::min(path.find('/', start), path.size());
        if (end > start) parts.emplace_back(path.substr(start, end - start));
        start = end + 1;
    }
    pending.insert(pending.begin(), parts.begin(), parts.end());
}

constexpr int kMaxSymlinks = 40;  // the kernel's own limit on one lookup

[[noreturn]] void throwShortage(const char* component, int err, const std::string& file) {
    const char* what = err == ENOMEM ? "kernel memory" : "file descriptors";
    throw ResourceExhausted(describeRejection(
        component, std::string("the daemon has run out of ") + what + " (" + std::strerror(err) + ")", file));
}

// The refused type a filesystem's magic number shows, for when the mount table cannot be read. Every
// FUSE mount shares one number and shows no subtype, so none is read from then, local or not.
std::string refusedMagic(uint32_t magic) {
    switch (magic) {
    case NFS_SUPER_MAGIC: return "nfs";
    case SMB_SUPER_MAGIC: return "smbfs";
    case CIFS_SUPER_MAGIC: return "cifs";
    case SMB2_SUPER_MAGIC: return "smb3";
    case NCP_SUPER_MAGIC: return "ncpfs";
    case AFS_SUPER_MAGIC:
    case AFS_FS_MAGIC: return "afs";
    case CEPH_SUPER_MAGIC: return "ceph";
    case V9FS_MAGIC: return "9p";
    case CODA_SUPER_MAGIC: return "coda";
    case FUSE_SUPER_MAGIC: return std::string(kFusePrefix);
    default: return {};
    }
}

// The check after the open, by the device `file` lives on: what catches a mount the path walk could
// not see, such as an automount the walk itself triggered. When the table cannot be read, `fd`'s
// filesystem is asked its magic number instead. Nothing when neither can answer.
std::string refusedFilesystemOfOpen(int fd, const struct stat& file, const char* component) {
    if (const std::optional<std::vector<Mount>> mounts = readMountTable(component)) {
        // Bind mounts share a device and so a type; the first entry for it answers.
        for (const Mount& mount : *mounts) {
            if (mount.device == file.st_dev) return refusedType(mount.type).value_or(std::string());
        }
        return {};
    }
    struct statfs fs{};
    if (::fstatfs(fd, &fs) != 0) return {};
    return refusedMagic(static_cast<uint32_t>(fs.f_type));
}

}  // namespace

std::string refusedFilesystemOf(const std::string& path, const char* component) {
    if (path.empty()) return {};
    const std::optional<std::vector<Mount>> table = readMountTable(component);
    if (!table) return {};
    const std::vector<Mount>& mounts = *table;
    const Mount* root = rootMount(mounts);
    if (root == nullptr) return {};

    std::deque<std::string> pending;
    prependComponents(pending, path);
    if (path.front() != '/') {
        char cwd[PATH_MAX];
        if (::getcwd(cwd, sizeof(cwd)) == nullptr) return {};
        prependComponents(pending, cwd);
    }

    // Resolved the way the kernel would, but asking the mount table about each prefix before it is
    // looked up: an lstat of a mount point is a question to its server.
    std::string resolved;  // empty is the root
    int links = 0;
    while (!pending.empty()) {
        std::string part = std::move(pending.front());
        pending.pop_front();
        if (part == ".") continue;
        if (part == "..") {
            const size_t slash = resolved.rfind('/');
            resolved.erase(slash == std::string::npos ? 0 : slash);
            continue;
        }
        std::string next = resolved + "/" + part;
        if (std::optional<std::string> type = refusedTypeAt(mounts, *root, next)) return std::move(*type);

        // What cannot be looked up is left to the open, which says why in its own words.
        struct stat info{};
        if (::lstat(next.c_str(), &info) != 0) return {};
        if (!S_ISLNK(info.st_mode)) {
            resolved = std::move(next);
            continue;
        }
        if (++links > kMaxSymlinks) return {};
        std::string target(PATH_MAX, '\0');
        const ssize_t n = ::readlink(next.c_str(), target.data(), target.size());
        if (n <= 0 || static_cast<size_t>(n) >= target.size()) return {};
        target.resize(static_cast<size_t>(n));
        prependComponents(pending, target);
        if (target.front() == '/') resolved.clear();
    }
    return {};
}

std::string refusedFilesystemReason(const std::string& type) {
    if (type.starts_with(kFusePrefix)) {
        return "the wallpaper is on a FUSE filesystem (" + type +
               "), which is not read from unless it is a known local disk driver: a network drive behind it "
               "that stops answering would freeze every screen; copy it to a local disk";
    }
    return "the wallpaper is on a network filesystem (" + type +
           "), which is not read from: a server that stops answering would freeze every screen; copy it to a "
           "local disk";
}

WallpaperOpen openWallpaper(const std::string& file, const char* component) {
    // Before the open: opening a file on a mount whose server has gone is itself the wait.
    if (const std::string type = refusedFilesystemOf(file, component); !type.empty()) {
        return WallpaperOpen::Refused{refusedFilesystemReason(type)};
    }

    const int fd = ::open(file.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        const int failed = errno;
        if (isShortage(failed)) throwShortage(component, failed, file);
        return WallpaperOpen::Failed{failed};
    }

    struct stat info{};
    if (::fstat(fd, &info) != 0) {
        const int failed = errno;
        ::close(fd);
        if (isShortage(failed)) throwShortage(component, failed, file);
        return WallpaperOpen::Failed{failed};
    }
    if (!S_ISREG(info.st_mode)) {
        ::close(fd);
        return WallpaperOpen::Refused{"the wallpaper is not a regular file"};
    }
    std::string refused;
    try {
        refused = refusedFilesystemOfOpen(fd, info, component);
    } catch (...) {
        ::close(fd);
        throw;
    }
    if (!refused.empty()) {
        ::close(fd);
        return WallpaperOpen::Refused{refusedFilesystemReason(refused)};
    }
    // The open is what it was for, and a regular file's reads ignore it; cleared so nothing reading
    // through this descriptor has to be written against one set two ways.
    if (const int flags = ::fcntl(fd, F_GETFL); flags >= 0) ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    return WallpaperOpen::Opened{fd};
}

int openWallpaperFile(const std::string& file, const char* component) {
    const WallpaperOpen opened = openWallpaper(file, component);
    if (const auto* refused = std::get_if<WallpaperOpen::Refused>(&opened.result)) {
        throw WallpaperContentError(describeRejection(component, refused->reason, file));
    }
    if (const auto* failed = std::get_if<WallpaperOpen::Failed>(&opened.result)) {
        errno = failed->error;
        return -1;
    }
    return std::get<WallpaperOpen::Opened>(opened.result).fd;
}

std::string describeRejection(const char* component, const std::string& reason, const std::string& path) {
    return std::string(component) + ": " + reason + ": " + path;
}

}  // namespace wallpaperx
