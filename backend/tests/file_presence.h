// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// "Does the package ship this file?", answered so that the two ways of not finding it stay
// apart.
//
// Both test binaries that grade backend/kde/kpackage/ ask this, and they must draw the same
// line: a traversal error -- an unreadable parent directory, EIO -- is this host failing to
// look, which is a skip, while a clean answer of "not a regular file" is the package failing
// to ship it, which is a failure. Collapsing the two blames production for a broken checkout,
// or, in the direction that matters more, grades a deleted package file as ctest's "Skipped".
//
// One definition rather than one per binary, for the reason tests/test_report.h gives about
// the pass/fail/skip contract: a rule stated in two files is two things that can disagree, and
// the copy that is missed still compiles, still passes and still reads as coverage.
//
// Dependency-free apart from the standard library, so the binaries that link nothing but
// nlohmann_json can include it.

#include <filesystem>
#include <string>
#include <system_error>

namespace wallpaperx::testing {

enum class Presence { Present, Missing, Unknowable };

// GOAL: a package file that is not there is graded as the package being wrong, not as this
//       host being unable to look. False if a deleted contents/ui/main.qml makes either test
//       exit 77, which ctest reports as Skipped -- not-a-failure over a missing entry point.
// WHY-THIS-SHAPE: the error_code is inspected for ENOENT/ENOTDIR rather than the simpler
//       `return std::filesystem::exists(path);`. libstdc++ reports "not there" through ec, so
//       an implementation that treated every ec as "could not look" would classify exactly the
//       one condition Missing exists for as Unknowable, and the two tests would go quiet
//       together on the loudest possible package defect.
// CONTROL: the ENOENT/ENOTDIR test replaced with `if (false)`, with main.qml moved aside in a
//       /tmp copy of backend/ -- both binaries went from exit 1 and `FAIL: the package ships
//       contents/ui/main.qml ...` to exit 77 and `NOT VERIFIED -- ... could not be determined`.
//       One mutation here moved both, which is what the sharing is for. Reverted.
inline Presence presenceOf(const std::filesystem::path& path, std::string& why) {
    std::error_code ec;
    const bool regular = std::filesystem::is_regular_file(path, ec);
    if (ec) {
        // ENOTDIR belongs with ENOENT: a path component replaced by a file is the package
        // being wrong, not the host.
        if (ec == std::errc::no_such_file_or_directory || ec == std::errc::not_a_directory) {
            return Presence::Missing;
        }
        why = path.string() + ": " + ec.message();
        return Presence::Unknowable;
    }
    return regular ? Presence::Present : Presence::Missing;
}

}  // namespace wallpaperx::testing
