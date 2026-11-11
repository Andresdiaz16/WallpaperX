// SPDX-License-Identifier: GPL-2.0-or-later

#include "package.h"

#include "byte_cursor.h"
#include "plain_path.h"
#include "system_error.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <format>
#include <utility>

namespace wallpaperx::we {

namespace {

constexpr std::size_t kMagicBytes = 8;
// What the first read takes: the whole table of every real package, so the second is rare.
constexpr std::size_t kFirstTableRead = 64u << 10;

std::unexpected<std::string> refuse(std::string_view what)
{
    return std::unexpected(std::format("The package {}", what));
}

// Reads exactly `count` bytes at `offset`, or says why it could not.
std::expected<std::string, std::string> readAt(int fd, std::uint64_t offset, std::size_t count)
{
    std::string out(count, '\0');
    std::size_t done = 0;
    while (done < count) {
        const ssize_t got = ::pread(fd, out.data() + done, count - done, off_t(offset + done));
        if (got < 0) {
            const int error = errno;
            if (error == EINTR) continue;
            return refuse(std::format("could not be read ({}).", manifest::systemError(error)));
        }
        if (got == 0) return refuse("ended early: the file is shorter than when it was opened.");
        done += std::size_t(got);
    }
    return out;
}

struct TableRead {
    // Set when the table ran past the bytes given: read more of the file and parse again.
    bool needMore = false;
    std::string refusal;
    std::string version;
    std::vector<Package::Entry> entries;
};

TableRead parseTable(std::string_view head, std::uint64_t fileSize)
{
    TableRead result;
    const auto shortOf = [&result] {
        result.needMore = true;
        return std::move(result);
    };
    const auto refused = [&result](std::string reason) {
        result.refusal = std::move(reason);
        return std::move(result);
    };

    ByteCursor in(head);
    const auto magicLength = in.u32();
    if (!magicLength) return shortOf();
    if (*magicLength != kMagicBytes) return refused("does not start with a package version.");
    const auto magic = in.take(kMagicBytes);
    if (!magic) return shortOf();
    if (!magic->starts_with("PKGV") ||
        !std::ranges::all_of(magic->substr(4), [](char c) { return c >= '0' && c <= '9'; })) {
        return refused("does not start with a package version.");
    }
    const auto count = in.u32();
    if (!count) return shortOf();
    // Each entry takes at least 13 bytes: a name of one byte and its three numbers.
    if (*count > kMaxPackageEntries || std::uint64_t(*count) * 13 > fileSize) {
        return refused(std::format("claims {} files, more than it can hold.", *count));
    }
    result.version = std::string(*magic);

    result.entries.reserve(*count);
    for (std::uint32_t i = 0; i < *count; ++i) {
        const auto nameLength = in.u32();
        if (!nameLength) return shortOf();
        if (*nameLength == 0 || *nameLength > kMaxPackageNameBytes) {
            return refused(std::format("has a file name of {} bytes.", *nameLength));
        }
        const auto name = in.take(*nameLength);
        if (!name) return shortOf();
        const auto offset = in.u32();
        const auto size = in.u32();
        if (!offset || !size) return shortOf();
        if (!manifest::isPlainRelativePath(*name)) {
            return refused("has a file name that is not a plain relative path.");
        }
        result.entries.push_back({std::string(*name), *offset, *size});
    }

    // Offsets count from the end of the table.
    const std::uint64_t dataBase = in.position();
    for (Package::Entry& entry : result.entries) {
        entry.offset += dataBase;
        if (entry.offset + entry.size > fileSize) return refused("has a file that runs past its end.");
    }
    std::ranges::sort(result.entries, {}, &Package::Entry::name);
    const auto same = std::ranges::adjacent_find(result.entries, {}, &Package::Entry::name);
    if (same != result.entries.end()) return refused("holds two files of the same name.");
    return result;
}

} // namespace

std::expected<Package, std::string> Package::open(UniqueFd fd)
{
    struct stat st{};
    if (::fstat(fd.get(), &st) != 0) {
        const int error = errno;
        return refuse(std::format("could not be examined ({}).", manifest::systemError(error)));
    }
    if (!S_ISREG(st.st_mode)) return refuse("is not a regular file.");
    const auto fileSize = std::uint64_t(st.st_size);

    std::string head;
    for (const std::size_t want : {kFirstTableRead, kMaxPackageTableBytes}) {
        const auto length = std::size_t(std::min<std::uint64_t>(want, fileSize));
        auto more = readAt(fd.get(), head.size(), length - head.size());
        if (!more) return std::unexpected(std::move(more.error()));
        head += *more;
        TableRead table = parseTable(head, fileSize);
        if (!table.refusal.empty()) return refuse(table.refusal);
        if (!table.needMore) return Package(std::move(fd), std::move(table.version), std::move(table.entries));
        if (length == fileSize) return refuse("ends inside its table of contents.");
    }
    return refuse(std::format("has a table of contents larger than {} MiB.", kMaxPackageTableBytes >> 20));
}

const Package::Entry* Package::find(std::string_view name) const
{
    const auto it = std::ranges::lower_bound(m_entries, name, {}, &Entry::name);
    return it != m_entries.end() && it->name == name ? &*it : nullptr;
}

std::expected<std::string, std::string> Package::read(std::string_view name, std::size_t limit) const
{
    const Entry* entry = find(name);
    if (entry == nullptr) return refuse("holds no such file.");
    if (entry->size > limit) {
        return refuse(std::format("holds the file at {} bytes, over the {} allowed for it.", entry->size, limit));
    }
    return readAt(m_fd.get(), entry->offset, entry->size);
}

} // namespace wallpaperx::we
