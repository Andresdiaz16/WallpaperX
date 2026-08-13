// SPDX-License-Identifier: GPL-2.0-or-later

// Step 9's Plasma wallpaper package: backend/kde/kpackage/, the directory Plasma reads to
// learn that a wallpaper plugin called WallpaperX exists, what to call it in the wallpaper
// chooser, and which QML file to load.
//
// Why this is worth a test at all: the same handful of facts are written in three places --
// the package's metadata.json, the install rule in backend/CMakeLists.txt, and the
// JavaScript the app will hand Plasma to switch a desktop over -- and *every* way they can
// disagree fails silently. KPackage skips a package whose metadata will not parse, a wrong
// KPackageStructure gives a black desktop with nothing logged, and an id mismatch makes
// Plasma switch a desktop to a plugin that resolves to nothing, so the desktop simply keeps
// what it had. None of those produce an error a developer would see.
//
// Of those three, only two are readable from here: the activation code is Step 15 and does
// not exist yet, so its id is pinned as a literal and the goal is covered two ways out of
// three until then. GOALS.md's S9.3 records that as an obligation on Step 15.
//
// Its own target rather than a phase of an existing test, and deliberately outside the
// if(WALLPAPERX_BUILD_KDE_PLUGIN) guard: everything here is text in the source tree, so it
// needs no Qt, no KDE libraries, no kpackagetool6, no running Plasma and no GPU. It links
// nlohmann_json alone -- not the daemon library -- so it stays runnable in the sanitizer
// tree, which is configured with the KDE plugin off, and on a hosted runner, which selects
// it by its `novulkan` label.
//
// This test only ever opens files for reading, and only under the backend source directory
// baked in below. It creates nothing, so it cannot reach a developer's own
// ~/.config/wallpaperx.
//
// The skip is file-granular: one arm that could not be exercised makes the whole binary exit
// 77, so ctest says "Skipped" even where four of the five goals ran and passed. Splitting it
// would mean a phase-selecting argument and five registrations, which is more machinery than
// a more precise word for a broken checkout is worth. A check that actually fails always
// wins over a skip, so this cannot turn a failure into one.
//
// What it cannot establish, by construction, since the suite runs headless: that the
// wallpaper chooser actually lists the plugin, that the declared icon resolves in the user's
// icon theme, and that activation changes a real desktop. Those were checked by hand on
// Plasma 6.7.4 and are recorded in PLAN.md, not here.

#include "tests/test_report.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cerrno>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

// The pass / fail / skip contract lives in tests/test_report.h: check() and its counter,
// notVerified() and its counter, and exit 77 -- which ctest reads as SKIP through
// SKIP_RETURN_CODE in backend/CMakeLists.txt -- reserved for "this host could not set the
// check up", never for "the check did not like what it saw".
using wallpaperx::testing::check;
using wallpaperx::testing::notVerified;

int g_harnessFaults = 0;

// A defect in this file rather than in the package it reads. Counted separately, not just
// printed differently: the summary line otherwise reports a broken helper as "N check(s)
// failed", which sends the reader to backend/kde/kpackage/ for a fault that is here. Still
// exits nonzero -- a harness that cannot be trusted must stay red, never become a skip.
void harnessFailure(const std::string& what) {
    std::fprintf(stderr, "FAIL (harness): %s\n", what.c_str());
    ++g_harnessFaults;
}

// The wallpaper plugin's identifier, pinned here as a literal on purpose.
//
// It is one of the three places this string lives. Two of them this process can read:
// metadata.json's KPlugin.Id, and the component argument of plasma_install_package() in
// backend/CMakeLists.txt. The third is the JavaScript the app evaluates through Plasma's
// scripting interface to switch a desktop over -- `d.wallpaperPlugin =
// "com.wallpaperx.wallpaper"` -- which is Step 15 and does not exist in the tree yet, and
// whose only written record is PLAN.md, which is permanently untracked. So there is nowhere
// to read it from and it is written out here instead.
//
// Be clear about what that buys and what it does not. It catches metadata.json and the
// install rule being renamed together -- a change that keeps them agreeing with each other
// while walking away from the string the app will ask for. It cannot catch the activation
// code being written with a different string, because nothing here reads that code. Closing
// that is Step 15's obligation, recorded as such in GOALS.md's S9.3 rather than left to
// whoever notices: when plasma_controller.cpp exists, this arm reads the id out of it.
constexpr std::string_view kActivationPluginId = "com.wallpaperx.wallpaper";

// KPackage's structure name for a Plasma wallpaper. Anything else and Plasma loads nothing.
constexpr std::string_view kWallpaperStructure = "Plasma/Wallpaper";

// The package directory as plasma_install_package() names it, relative to backend/.
constexpr std::string_view kPackageDirArgument = "kde/kpackage";

// The subdirectory of the data directory that Plasma's wallpaper package structure scans.
// Installed anywhere else, the package is complete, correctly named, and never found.
constexpr std::string_view kInstallRoot = "wallpapers";

std::optional<std::string> readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) return std::nullopt;
    return text;
}

bool isIdentifierChar(char c) {
    const auto u = static_cast<unsigned char>(c);
    return std::isalnum(u) != 0 || c == '_';
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) text.remove_suffix(1);
    return text;
}

// --------------------------------------------------------------------------- //
// backend/CMakeLists.txt: the install rule's arguments
// --------------------------------------------------------------------------- //

struct CMakeCall {
    std::vector<std::string> args;
};

// Splits on whitespace, honouring double quotes. Used for a CMake argument list once its
// comments have been blanked, and for a QML line once comments and string bodies have been.
// CMake's full argument grammar (bracket arguments, escapes, variable references) is not
// implemented. A component argument this cannot read is reported as a skip by its caller
// rather than as the package disagreeing with the build -- see runIdentifierAgreementChecks.
std::vector<std::string> splitWords(std::string_view text) {
    std::vector<std::string> args;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) != 0) ++i;
        if (i >= text.size()) break;
        std::string token;
        if (text[i] == '"') {
            ++i;
            while (i < text.size() && text[i] != '"') {
                if (text[i] == '\\' && i + 1 < text.size()) ++i;
                token += text[i++];
            }
            if (i < text.size()) ++i;
        } else {
            while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) == 0) token += text[i++];
        }
        args.push_back(std::move(token));
    }
    return args;
}

// Blanks CMake comments: `#` to end of line, and the bracket forms `#[[ ... ]]` and
// `#[=[ ... ]=]`, which span lines. Every removed byte becomes a space and newlines are
// kept, so offsets stay valid. A `#` inside a double-quoted argument is not a comment,
// which is why quotes are tracked rather than each line scanned for the first `#`.
//
// The bracket forms are the reason this exists at all. Scanning for a `#` earlier on the
// same line reads a rule disabled by `#[[ ... ]]` as live, and that fails silently: the
// package is then never installed and the check written to catch exactly that reports green.
std::string blankCmakeComments(std::string_view src) {
    std::string out(src);
    std::size_t i = 0;
    const auto blankTo = [&](std::size_t from, std::size_t to) {
        for (std::size_t k = from; k < to && k < out.size(); ++k) {
            if (out[k] != '\n') out[k] = ' ';
        }
    };
    while (i < out.size()) {
        if (out[i] == '"') {
            std::size_t k = i + 1;
            while (k < out.size() && out[k] != '"') {
                if (out[k] == '\\' && k + 1 < out.size()) ++k;
                ++k;
            }
            i = (k < out.size()) ? k + 1 : k;
        } else if (out[i] == '#') {
            std::size_t equals = 0;
            std::size_t bodyStart = 0;
            if (i + 1 < out.size() && out[i + 1] == '[') {
                std::size_t j = i + 2;
                while (j < out.size() && out[j] == '=') { ++equals; ++j; }
                if (j < out.size() && out[j] == '[') bodyStart = j + 1;
            }
            std::size_t end = 0;
            if (bodyStart != 0) {
                const std::string closer = "]" + std::string(equals, '=') + "]";
                const std::size_t found = out.find(closer, bodyStart);
                // An unterminated bracket comment runs to end of file, as CMake reads it.
                end = (found == std::string::npos) ? out.size() : found + closer.size();
            } else {
                end = out.find('\n', i);
                if (end == std::string::npos) end = out.size();
            }
            blankTo(i, end);
            i = end;
        } else if (out[i] == '[') {
            // A bracket argument -- `[[ ... ]]`, `[=[ ... ]=]` -- is content, not a comment,
            // so it is stepped over without blanking. It has to be recognised all the same:
            // its body is not string-escaped, so a lone `"` inside one would otherwise put
            // the scan into string mode for the rest of the file and stop every `#` after it
            // from being blanked -- the silent direction, since a dead rule then reads live.
            std::size_t equals = 0;
            std::size_t j = i + 1;
            while (j < out.size() && out[j] == '=') { ++equals; ++j; }
            if (j < out.size() && out[j] == '[') {
                const std::string closer = "]" + std::string(equals, '=') + "]";
                const std::size_t found = out.find(closer, j + 1);
                i = (found == std::string::npos) ? out.size() : found + closer.size();
            } else {
                ++i;
            }
        } else {
            ++i;
        }
    }
    return out;
}

// Every live plasma_install_package(...) call in the file, comments already blanked out.
std::vector<CMakeCall> findInstallPackageCalls(std::string_view rawText) {
    static constexpr std::string_view kName = "plasma_install_package";
    const std::string text = blankCmakeComments(rawText);
    std::vector<CMakeCall> calls;
    std::size_t at = 0;
    while ((at = text.find(kName, at)) != std::string::npos) {
        const std::size_t nameEnd = at + kName.size();
        const bool wholeWord = (at == 0 || !isIdentifierChar(text[at - 1]));

        std::size_t open = nameEnd;
        while (open < text.size() && std::isspace(static_cast<unsigned char>(text[open])) != 0) ++open;

        if (wholeWord && open < text.size() && text[open] == '(') {
            // The closing paren, skipping any inside a quoted argument -- a path or message
            // containing ')' would otherwise cut the argument list short.
            std::size_t close = open + 1;
            while (close < text.size() && text[close] != ')') {
                if (text[close] == '"') {
                    ++close;
                    while (close < text.size() && text[close] != '"') {
                        if (text[close] == '\\' && close + 1 < text.size()) ++close;
                        ++close;
                    }
                }
                if (close < text.size()) ++close;
            }
            if (close < text.size()) {
                calls.push_back(CMakeCall{splitWords(std::string_view(text).substr(open + 1, close - open - 1))});
            }
        }
        at = nameEnd;
    }
    return calls;
}

// The reader models exactly one way a rule stops running: being commented out. It is checked
// against the forms that decide that before it is trusted, because a reader that stopped
// understanding bracket comments would grade a dead install rule as live -- a defect in this
// file, not in the package, so a failure here is a harness fault.
//
// What it does NOT model: reachability. A rule sealed inside an `if()` no build satisfies is
// indistinguishable from a live one here, and the rule this file reads really is inside
// `if(WALLPAPERX_BUILD_KDE_PLUGIN)`. Following that would mean tracking conditions and the
// options that satisfy them -- a CMake evaluator in a test -- and the two defects already
// found in this reader argue against more parsing, not less. Recorded as an uncovered row
// under S9.3 in GOALS.md rather than implied to be covered.
bool cmakeReaderWorks() {
    struct Case {
        std::string_view source;
        std::size_t expectedCalls;
        std::vector<std::string> expectedArgs;  // of the first call, empty to not check
        const char* what;
    };
    const std::vector<Case> kCases = {
        {"plasma_install_package(kde/kpackage com.x wallpapers wallpaper)\n", 1,
         {"kde/kpackage", "com.x", "wallpapers", "wallpaper"}, "a plain call"},
        {"# plasma_install_package(kde/kpackage com.x wallpapers wallpaper)\n", 0, {},
         "a call behind a line comment"},
        {"#[[\nplasma_install_package(kde/kpackage com.x wallpapers wallpaper)\n]]\n", 0, {},
         "a call inside a bracket comment"},
        {"#[=[\nplasma_install_package(kde/kpackage com.x wallpapers wallpaper)\n]=]\n", 0, {},
         "a call inside an equals-padded bracket comment"},
        {"#[[\nplasma_install_package(kde/kpackage com.x wallpapers wallpaper)\n", 0, {},
         "a call inside an unterminated bracket comment"},
        {"plasma_install_package(a \"b#c\" d)\n", 1, {"a", "b#c", "d"},
         "a hash inside a quoted argument is not a comment"},
        {"plasma_install_package(a  # note\n    b)\n", 1, {"a", "b"},
         "a comment between arguments is dropped, not the arguments after it"},
        {"plasma_install_package(a \"x)y\" b)\n", 1, {"a", "x)y", "b"},
         "a paren inside a quoted argument does not end the call"},
        {"my_plasma_install_package(a b)\n", 0, {}, "a longer name is not this call"},
        {"set(m [[a \" b]])\n# plasma_install_package(kde/kpackage com.x wallpapers wallpaper)\n"
         "plasma_install_package(kde/kpackage com.x wallpapers wallpaper)\n",
         1, {"kde/kpackage", "com.x", "wallpapers", "wallpaper"},
         "an unpaired quote inside a bracket argument does not stop later comments being blanked"},
        {"plasma_install_package(a [[b]] c)\n", 1, {"a", "[[b]]", "c"},
         "a bracket argument inside the call is kept, not blanked"},
    };

    bool ok = true;
    for (const Case& c : kCases) {
        const std::vector<CMakeCall> calls = findInstallPackageCalls(c.source);
        if (calls.size() != c.expectedCalls) {
            std::fprintf(stderr, "  the CMake reader is wrong about \"%s\": found %zu call(s), expected %zu\n",
                         c.what, calls.size(), c.expectedCalls);
            ok = false;
            continue;
        }
        if (!c.expectedArgs.empty() && calls.front().args != c.expectedArgs) {
            std::fprintf(stderr, "  the CMake reader is wrong about \"%s\": arguments came out as", c.what);
            for (const std::string& a : calls.front().args) std::fprintf(stderr, " [%s]", a.c_str());
            std::fprintf(stderr, "\n");
            ok = false;
        }
    }
    return ok;
}

// --------------------------------------------------------------------------- //
// contents/ui/main.qml: the root element
// --------------------------------------------------------------------------- //

// Blanks out //-comments, /*...*/ comments and the insides of string literals, replacing
// each removed byte with a space so line numbers and offsets survive. Without it, the word
// WallpaperItem inside a comment or a string would satisfy a search for the root element.
std::string blankCommentsAndStrings(std::string_view src) {
    std::string out(src);
    std::size_t i = 0;
    const auto blankTo = [&](std::size_t from, std::size_t to) {
        for (std::size_t k = from; k < to && k < out.size(); ++k) {
            if (out[k] != '\n') out[k] = ' ';
        }
    };
    while (i < out.size()) {
        if (out[i] == '/' && i + 1 < out.size() && out[i + 1] == '/') {
            std::size_t end = out.find('\n', i);
            if (end == std::string::npos) end = out.size();
            blankTo(i, end);
            i = end;
        } else if (out[i] == '/' && i + 1 < out.size() && out[i + 1] == '*') {
            std::size_t end = out.find("*/", i + 2);
            end = (end == std::string::npos) ? out.size() : end + 2;
            blankTo(i, end);
            i = end;
        } else if (out[i] == '"' || out[i] == '\'') {
            const char quote = out[i];
            std::size_t k = i + 1;
            while (k < out.size() && out[k] != quote && out[k] != '\n') {
                if (out[k] == '\\' && k + 1 < out.size()) ++k;
                ++k;
            }
            const std::size_t end = (k < out.size() && out[k] == quote) ? k + 1 : k;
            blankTo(i, end);
            i = end;
        } else {
            ++i;
        }
    }
    return out;
}

struct QmlImport {
    std::string module;
    // `import X as Y` -- the module's types are then spelled Y.Type, and the bare name does
    // not resolve. Both spellings are legal QML, so the alias is kept, not just its presence:
    // the root element's expected spelling follows from it.
    std::string alias;
};

struct QmlShape {
    std::vector<QmlImport> imports;
    std::string rootType;             // first declared object's type name, empty if none found
    bool rootFollowedByBrace = false; // the identifier is an object declaration, not a stray word
    int contentLines = 0;             // non-blank lines after stripping
};

// Reads the file as text and reports the imports and the first object declaration. QML is
// line-oriented at the top of a file -- imports and pragmas each occupy one line -- so the
// scan walks lines until it reaches one that is neither, then parses an identifier and the
// brace that must follow it.
QmlShape scanQml(std::string_view source) {
    // A byte-order mark is a legal, invisible prefix that Qt accepts. It is neither
    // whitespace nor an identifier byte here, so leaving it in makes the first line fail the
    // `import` compare and be read as the root object declaration.
    static constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";
    if (source.substr(0, kUtf8Bom.size()) == kUtf8Bom) source.remove_prefix(kUtf8Bom.size());
    const std::string text = blankCommentsAndStrings(source);
    QmlShape shape;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        const std::string_view raw(text.data() + pos, eol - pos);
        const std::string_view line = trim(raw);
        if (!line.empty()) {
            ++shape.contentLines;
            const std::vector<std::string> tokens = splitWords(line);
            if (!tokens.empty() && tokens.front() == "import") {
                QmlImport imp;
                if (tokens.size() > 1) imp.module = tokens[1];
                for (std::size_t t = 0; t + 1 < tokens.size(); ++t) {
                    if (tokens[t] == "as") imp.alias = tokens[t + 1];
                }
                shape.imports.push_back(std::move(imp));
            } else if (!tokens.empty() && tokens.front() == "pragma") {
                // no-op: pragma lines precede the root object
            } else {
                std::size_t i = pos;
                while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) != 0) ++i;
                std::string name;
                while (i < text.size() && (isIdentifierChar(text[i]) || text[i] == '.')) name += text[i++];
                while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) != 0) ++i;
                shape.rootType = name;
                shape.rootFollowedByBrace = i < text.size() && text[i] == '{';
                return shape;
            }
        }
        if (eol >= text.size()) break;
        pos = eol + 1;
    }
    return shape;
}

// The blanking helper is what makes the root-element check more than a substring search, so
// it is checked against cases where a naive search is wrong. A helper that stopped blanking
// would leave the S9.4 check unable to tell a declaration from a mention, which is a defect
// in this file and not in the package -- so it is reported as a harness failure and S9.4's
// verdict is withheld rather than being printed against production.
bool commentStripperWorks() {
    bool ok = true;

    // The blanking contract, asserted directly on the helper. The scan cases below cannot
    // reach the string arm -- QML has no string at top level, and the scan stops at the
    // first object declaration -- so going only through scanQml would leave string blanking
    // unexercised while looking covered.
    struct Blanking {
        std::string_view source;
        std::string_view mustNotSurvive;
        const char* what;
    };
    static constexpr Blanking kBlanking[] = {
        {"// WallpaperItem\n", "WallpaperItem", "a line comment's body"},
        {"/* WallpaperItem */\n", "WallpaperItem", "a block comment's body"},
        {"text: \"WallpaperItem\"\n", "WallpaperItem", "a double-quoted string's body"},
        {"text: 'WallpaperItem'\n", "WallpaperItem", "a single-quoted string's body"},
        {"text: \"/*\"\nItem {}\n", "/*", "a comment delimiter inside a string"},
        {"/* a\n WallpaperItem \n*/\nItem {}\n", "WallpaperItem", "a block comment spanning lines"},
    };
    for (const Blanking& c : kBlanking) {
        const std::string blanked = blankCommentsAndStrings(c.source);
        if (blanked.find(c.mustNotSurvive) != std::string::npos) {
            std::fprintf(stderr, "  the QML scanner does not blank %s\n", c.what);
            ok = false;
        }
        // Newlines, not length. Length is preserved by construction -- the helper only
        // overwrites bytes in a same-size copy -- so asserting it could never fail. What
        // can break is the exemption that keeps '\n' intact, and losing that would silently
        // destroy the line walk below, since the whole file would collapse to one line.
        const auto newlines = [](std::string_view s) { return std::count(s.begin(), s.end(), '\n'); };
        if (newlines(blanked) != newlines(c.source)) {
            std::fprintf(stderr, "  the QML scanner lost line boundaries while blanking %s\n", c.what);
            ok = false;
        }
    }

    struct Case {
        std::string_view source;
        std::string_view expectedRoot;
        bool expectBrace;
        const char* what;
    };
    static constexpr Case kCases[] = {
        {"// WallpaperItem {\nimport QtQuick\nItem {\n}\n", "Item", true, "a line comment is not a declaration"},
        {"/* WallpaperItem { */\nItem {\n}\n", "Item", true, "a block comment is not a declaration"},
        {"import QtQuick\nWallpaperItem\n{\n}\n", "WallpaperItem", true, "a brace on the next line still declares"},
        {"import QtQuick\nWallpaperItem\n", "WallpaperItem", false, "a bare word is not an object declaration"},
        {"import QtQuick\n", "", false, "a file with no object declares no root"},
        {"\xEF\xBB\xBF" "import QtQuick\nItem {\n}\n", "Item", true, "a byte-order mark before the first import"},
    };
    for (const Case& c : kCases) {
        const QmlShape shape = scanQml(c.source);
        if (shape.rootType != c.expectedRoot || shape.rootFollowedByBrace != c.expectBrace) {
            std::fprintf(stderr, "  the QML scanner is wrong about \"%s\": root came out as \"%s\"%s\n", c.what,
                         shape.rootType.c_str(), shape.rootFollowedByBrace ? " followed by a brace" : "");
            ok = false;
        }
    }

    // The alias itself, not merely its presence: it decides which spelling of the root
    // element is legal, so the check downstream needs the name, and `P.WallpaperItem {` is
    // a correct file that an alias-blind scanner reports as a wrong root.
    const QmlShape aliased = scanQml("import org.kde.plasma.plasmoid as P\nP.WallpaperItem {\n}\n");
    if (aliased.imports.size() != 1 || aliased.imports.front().alias != "P" ||
        aliased.rootType != "P.WallpaperItem" || !aliased.rootFollowedByBrace) {
        std::fprintf(stderr, "  the QML scanner does not read an aliased import and its qualified root\n");
        ok = false;
    }
    return ok;
}

// --------------------------------------------------------------------------- //
// Phases
// --------------------------------------------------------------------------- //

std::filesystem::path g_backendDir;
std::filesystem::path g_packageDir;

// Three phases ask the same question about a file and must draw the same distinction: a
// traversal error -- an unreadable parent directory, EIO -- is this host failing to look,
// which is a skip, while a clean answer of "not a regular file" is the package failing to
// ship it, which is a failure. Collapsing the two blames production for a broken checkout.
enum class Presence { Present, Missing, Unknowable };

Presence presenceOf(const std::filesystem::path& path, std::string& why) {
    std::error_code ec;
    const bool regular = std::filesystem::is_regular_file(path, ec);
    if (ec) {
        // libstdc++ reports "not there" through ec rather than returning a clean false, so
        // the one condition Missing exists for is the one that arrives here. Treating every
        // ec as "could not look" made a deleted package file a skip, which ctest grades as
        // green -- the package vanishing would have been the quietest result of all.
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

// Parsed once and shared: S9.2 and S9.3 both read it, and reparsing would report one broken
// file as two unrelated failures.
std::optional<nlohmann::json> g_metadata;

// S9.1
void runMetadataParseChecks() {
    const std::filesystem::path metadataPath = g_packageDir / "metadata.json";
    std::string why;
    switch (presenceOf(metadataPath, why)) {
        case Presence::Unknowable:
            notVerified("S9.1-S9.3", "whether the package ships a metadata.json could not be determined -- " + why);
            return;
        case Presence::Missing:
            check(false, "the wallpaper package ships a metadata.json");
            std::fprintf(stderr, "  expected it at %s\n", metadataPath.string().c_str());
            return;
        case Presence::Present:
            break;
    }

    const std::optional<std::string> text = readFile(metadataPath);
    if (!text) {
        // Present but unreadable is a host condition, not a package property: install rules
        // set the shipped modes, so the source tree's permissions say nothing about what a
        // user would receive. Same channel the build file already uses -- a skip, not a FAIL.
        notVerified("S9.1-S9.3", "metadata.json is present but could not be read from " + metadataPath.string());
        return;
    }

    // GOAL: S9.1 -- backend/kde/kpackage/metadata.json parses as JSON. False if it does not:
    //       KPackage skips a package whose metadata it cannot read, and a skipped package is
    //       indistinguishable from one that was never installed.
    // WHY-THIS-SHAPE: parses with the exception-throwing overload and prints what the parser
    //       said, rather than the quiet nlohmann::json::parse(text, nullptr, false) that
    //       returns a discarded value. A discarded value compares unequal to everything, so
    //       the quiet form would leave S9.2 and S9.3 failing too and bury the one real cause
    //       under three symptoms.
    // CONTROL: deleted the comma after "Plasma/Wallpaper" in backend/kde/kpackage/metadata.json;
    //       observed `FAIL: the package's metadata.json parses as JSON` followed by the
    //       parser's "parse error at line 3, column 13 ... expected '}'", and S9.2 and S9.3
    //       printing that their metadata arms were withheld rather than failing too.
    //       Reverted. The three arms above it were controlled the same way, each restored and
    //       hash-compared: the file moved aside gave `FAIL: the wallpaper package ships a
    //       metadata.json`; chmod 000 gave `kpackage_metadata_test: NOT VERIFIED --
    //       S9.1-S9.3 could not be exercised: metadata.json is present but could not be
    //       read ...` and exit 77, a ctest skip rather than an accusation against the
    //       package (and not a spurious parse failure); wrapping the object in [ ] gave
    //       `FAIL: the package's metadata.json is a JSON object`.
    try {
        g_metadata = nlohmann::json::parse(*text);
    } catch (const nlohmann::json::exception& e) {
        check(false, "the package's metadata.json parses as JSON");
        std::fprintf(stderr, "  %s: %s\n", metadataPath.string().c_str(), e.what());
        return;
    }
    check(g_metadata->is_object(), "the package's metadata.json is a JSON object");
}

// S9.2
void runStructureChecks() {
    if (!g_metadata || !g_metadata->is_object()) {
        std::fprintf(stderr, "  S9.2 withheld: metadata.json is not a readable JSON object (reported above)\n");
        return;
    }
    // GOAL: S9.2 -- the package declares KPackageStructure with the exact value
    //       "Plasma/Wallpaper". False if the key is absent, differently cased, or any other
    //       value; the recorded symptom is a black desktop with nothing logged anywhere.
    // WHY-THIS-SHAPE: three separate assertions (present / is a string / equals) rather than
    //       one `md.value("KPackageStructure", "") == "Plasma/Wallpaper"`. The one-liner
    //       cannot distinguish a missing key from a misspelled value, and value() on a
    //       non-string throws -- turning a wrong type into a thrown phase instead of a
    //       named failure. contains() is case-sensitive, which is what makes the
    //       "differently cased key" arm of the goal reachable.
    // CONTROL: three mutations of backend/kde/kpackage/metadata.json, one per arm, each
    //       reverted. Key renamed to "KPackagestructure": `FAIL: metadata.json declares a
    //       KPackageStructure`. Value set to 42: `FAIL: the declared KPackageStructure is a
    //       string`. Value set to "Plasma/Wallpapers": `FAIL: the declared KPackageStructure
    //       is exactly "Plasma/Wallpaper"`, with both strings printed.
    const bool hasKey = g_metadata->contains("KPackageStructure");
    check(hasKey, "metadata.json declares a KPackageStructure");
    if (!hasKey) return;

    const nlohmann::json& structure = g_metadata->at("KPackageStructure");
    const bool isString = structure.is_string();
    check(isString, "the declared KPackageStructure is a string");
    if (!isString) return;

    const auto value = structure.get<std::string>();
    check(value == kWallpaperStructure, "the declared KPackageStructure is exactly \"Plasma/Wallpaper\"");
    if (value != kWallpaperStructure) {
        std::fprintf(stderr, "  metadata.json says \"%s\", Plasma loads only \"%s\"\n", value.c_str(),
                     std::string(kWallpaperStructure).c_str());
    }
}

// The install rule's own checks, and the package name it installs under if that name is a
// literal this reader can compare. Returns nothing when it cannot be established -- the
// caller's other comparison does not read it, and must still run.
std::optional<std::string> readInstalledIdFromBuild(const std::string& cmakeText) {
    if (!cmakeReaderWorks()) {
        harnessFailure("this file's CMake reader failed its own cases, so S9.3's metadata-versus-build "
                       "comparison is withheld -- the fault is in "
                       "backend/tests/kpackage_metadata_test.cpp, not in the package");
        return std::nullopt;
    }

    const std::vector<CMakeCall> calls = findInstallPackageCalls(cmakeText);
    std::vector<const CMakeCall*> forThisPackage;
    bool unexpandableDir = false;
    for (const CMakeCall& call : calls) {
        if (call.args.empty()) continue;
        if (call.args.front().find('$') != std::string::npos) {
            unexpandableDir = true;
            continue;
        }
        // Compared as a directory, not as a spelling: `kde/kpackage/` and `./kde/kpackage`
        // name the package this test is about, and reporting "no rule for this package"
        // because of a separator would blame the build for this reader's literalism.
        std::filesystem::path named = call.args.front();
        if (named.is_relative()) named = g_backendDir / named;
        if (named.lexically_normal() == g_packageDir.lexically_normal()) forThisPackage.push_back(&call);
    }

    if (forThisPackage.empty() && unexpandableDir) {
        notVerified("S9.3's build arm", "a plasma_install_package rule names its directory through a variable "
                                        "this reader does not expand, so no rule could be matched");
        return std::nullopt;
    }
    const bool exactlyOne = forThisPackage.size() == 1;
    check(exactlyOne, "backend/CMakeLists.txt has exactly one plasma_install_package rule for kde/kpackage");
    if (!exactlyOne) {
        std::fprintf(stderr, "  found %zu (of %zu plasma_install_package calls in the file)\n",
                     forThisPackage.size(), calls.size());
        return std::nullopt;
    }

    const CMakeCall& call = *forThisPackage.front();

    // Where the package lands, not just what it is called. Plasma's wallpaper structure
    // scans one subdirectory; installed anywhere else the package is complete, correctly
    // named, and never found. (S9.6.)
    const bool hasRoot = call.args.size() >= 3;
    check(hasRoot, "the install rule names the subdirectory it installs into");
    if (hasRoot && call.args[2].find('$') != std::string::npos) {
        notVerified("S9.6", "the install rule's subdirectory is not a literal word (\"" + call.args[2] +
                                "\"), which this reader does not expand");
    } else if (hasRoot) {
        check(call.args[2] == kInstallRoot,
              "the install rule installs into the subdirectory Plasma's wallpaper structure scans");
        if (call.args[2] != kInstallRoot) {
            std::fprintf(stderr, "  backend/CMakeLists.txt installs into \"%s\", Plasma scans only \"%s\"\n",
                         call.args[2].c_str(), std::string(kInstallRoot).c_str());
        }
    }
    // The fourth argument is deliberately not checked: Plasma 6's macro assigns it to a
    // variable it never reads, so a check on it would assert against no behaviour.

    const bool hasComponent = call.args.size() >= 2;
    check(hasComponent, "the install rule names the package it installs under");
    if (!hasComponent) return std::nullopt;

    // A component written as ${SOMEVAR} is a form this reader does not expand. Saying
    // "metadata.json and the build disagree" would then be a claim about the reader, not
    // about the tree, so it withholds instead -- the same channel an unreadable build file
    // uses. What it does not do is pass.
    if (call.args[1].find_first_of("$#()") != std::string::npos) {
        notVerified("S9.3's build arm", "the install rule's package name is not a literal word (\"" +
                                            call.args[1] + "\"), which this reader does not expand");
        return std::nullopt;
    }
    return call.args[1];
}

// S9.3
void runIdentifierAgreementChecks() {
    std::optional<std::string> metadataId;
    if (!g_metadata || !g_metadata->is_object()) {
        std::fprintf(stderr,
                     "  S9.3's metadata arm withheld: metadata.json is not a readable JSON object (reported above)\n");
    } else {
        // CONTROL: renamed KPlugin to KPlugins in backend/kde/kpackage/metadata.json; observed
        //       `FAIL: metadata.json has a KPlugin object` and the comparison withheld rather
        //       than passing. Renaming Id to id gave `FAIL: metadata.json declares KPlugin.Id
        //       as a string`. Both reverted.
        const bool hasPlugin = g_metadata->contains("KPlugin") && g_metadata->at("KPlugin").is_object();
        check(hasPlugin, "metadata.json has a KPlugin object");
        if (hasPlugin) {
            const nlohmann::json& plugin = g_metadata->at("KPlugin");
            const bool hasId = plugin.contains("Id") && plugin.at("Id").is_string();
            check(hasId, "metadata.json declares KPlugin.Id as a string");
            if (hasId) metadataId = plugin.at("Id").get<std::string>();
        }
    }

    const std::filesystem::path cmakePath = g_backendDir / "CMakeLists.txt";
    const std::optional<std::string> cmakeText = readFile(cmakePath);
    std::optional<std::string> installedId;
    if (!cmakeText) {
        // A build file this process cannot read is this run failing to set up, not the
        // package disagreeing with it, so it is a skip and not a FAIL against the package.
        // CONTROL: chmod 000 backend/CMakeLists.txt; observed the NOT VERIFIED line, the
        //       comparison printing that it was withheld, and exit 77 with no FAIL. Mode
        //       restored to 644.
        notVerified("S9.3's build arm", "backend/CMakeLists.txt could not be read from " + cmakePath.string());
    } else {
        // GOAL: S9.3 -- the package name the build installs under is the same string as
        //       metadata.json's KPlugin.Id. False if either is changed alone; Plasma is then
        //       asked to switch a desktop to an id that resolves to nothing and the desktop
        //       silently keeps what it had.
        // WHY-THIS-SHAPE: the call is located by argument, not by position -- every
        //       plasma_install_package() in backend/CMakeLists.txt is parsed and the one
        //       whose first argument names this package directory is selected, and exactly
        //       one must match. A plain text search for "com.wallpaperx.wallpaper" would
        //       pass on a commented-out rule, on a rule for a different package, and on no
        //       rule at all, since a search finding nothing proves nothing. Comments are
        //       blanked first, bracket forms included, for that reason.
        // CONTROL: commented out the plasma_install_package line in backend/CMakeLists.txt;
        //       observed `FAIL: backend/CMakeLists.txt has exactly one plasma_install_package
        //       rule for kde/kpackage`, "found 0 (of 0 ...)", and the comparison printing that
        //       it was withheld rather than passing. Wrapping the same line in a `#[[ ]]`
        //       bracket comment gives the identical FAIL -- before the reader understood
        //       bracket comments it printed "all checks passed", which is the silent direction.
        //       Separately changed the second argument to com.wallpaperx.wallpaper2; observed
        //       the metadata-versus-build check below fail naming both strings. Changed the
        //       third to plasmoids; observed the install-root check fail naming both. Reduced
        //       to plasma_install_package(kde/kpackage); observed `FAIL: the install rule names
        //       the package it installs under`. All reverted.
        //       Two more, both about this arm not silencing the other: with the CMake
        //       reader deliberately broken, the harness fault is reported and the
        //       activation-id comparison below still ran and still went red on a mutated
        //       KPlugin.Id -- an early return here used to suppress it.
        // COVERAGE-ASSERT: the "exactly one" count is asserted, so a parser that silently
        //       matched nothing fails here instead of leaving the comparisons unrun; and the
        //       reader is put through eleven cases of its own first, so a reader that had
        //       stopped understanding comments is a harness fault, not a package verdict.
        // A withheld build arm must not become a withheld phase: the activation-id
        // comparison below reads only metadata.json, so this reports what it can and
        // returns nothing rather than aborting the phase.
        installedId = readInstalledIdFromBuild(*cmakeText);
    }

    // Each comparison is guarded by what it actually consumes. Guarding both on "we have
    // both" made an unreadable build file suppress the activation-id check as well, which
    // needs only metadata.json -- so a package renamed away from the id the app will ask
    // for was reported as a skip on any host where CMakeLists.txt could not be read.
    if (metadataId && installedId) {
        check(*metadataId == *installedId,
              "the id in metadata.json is the name the build installs the package under");
        if (*metadataId != *installedId) {
            std::fprintf(stderr, "  metadata.json says \"%s\", backend/CMakeLists.txt installs \"%s\"\n",
                         metadataId->c_str(), installedId->c_str());
        }
    } else {
        std::fprintf(stderr, "  S9.3's metadata-versus-build comparison withheld: %s could not be read\n",
                     metadataId ? "the install rule's package name" : "metadata.json's KPlugin.Id");
    }

    // The third place the same string lives; see kActivationPluginId above for why it is a
    // literal here and not read from anywhere.
    // CONTROL: changed KPlugin.Id in backend/kde/kpackage/metadata.json to
    //       com.wallpaperx.wallpaperx; observed this check and the one above both fail,
    //       this one printing `the activation script assigns wallpaperPlugin =
    //       "com.wallpaperx.wallpaper"`. Re-run with backend/CMakeLists.txt at chmod 000 as
    //       well: the build arm reports NOT VERIFIED while this check still runs and still
    //       goes red, which is the behaviour the shared guard used to suppress. Reverted.
    if (metadataId) {
        check(*metadataId == kActivationPluginId,
              "the id in metadata.json is the plugin id the app's activation script will ask Plasma for");
        if (*metadataId != kActivationPluginId) {
            std::fprintf(stderr,
                         "  metadata.json says \"%s\", the activation script assigns wallpaperPlugin = \"%s\"\n",
                         metadataId->c_str(), std::string(kActivationPluginId).c_str());
        }
    } else {
        std::fprintf(stderr, "  S9.3's activation-id comparison withheld: metadata.json's KPlugin.Id "
                             "could not be read\n");
    }
}

// S9.4
void runWallpaperQmlChecks() {
    const std::filesystem::path mainQml = g_packageDir / "contents" / "ui" / "main.qml";
    std::string why;
    switch (presenceOf(mainQml, why)) {
        case Presence::Unknowable:
            notVerified("S9.4", "whether the package ships contents/ui/main.qml could not be determined -- " + why);
            return;
        case Presence::Missing:
            check(false, "the package ships contents/ui/main.qml, the wallpaper's entry point");
            std::fprintf(stderr, "  expected it at %s\n", mainQml.string().c_str());
            return;
        case Presence::Present:
            break;
    }

    const std::optional<std::string> source = readFile(mainQml);
    if (!source) {
        notVerified("S9.4", "contents/ui/main.qml is present but could not be read from " + mainQml.string());
        return;
    }

    if (!commentStripperWorks()) {
        harnessFailure("this file's QML scanner failed its own cases, so S9.4's root-element verdict is withheld "
                       "-- the fault is in backend/tests/kpackage_metadata_test.cpp, not in the package");
        return;
    }

    // GOAL: S9.4 -- main.qml's root element is WallpaperItem from org.kde.plasma.plasmoid.
    //       False if the root is a plain Item, or if the module is not imported. A
    //       non-WallpaperItem root renders off-screen at zero size, which looks like nothing
    //       happening rather than like an error.
    // WHY-THIS-SHAPE: reading text is all that is available -- instantiating the type needs
    //       Qt, a QML engine and the Plasma module, none of which this suite may depend on.
    //       So the check is a proxy, and its limits are real: it establishes that the module
    //       is imported and that the first object declared is spelled the way that import
    //       licenses. It cannot establish that the name *resolves* to Plasma's type -- a
    //       local WallpaperItem.qml beside it would win and this would not notice -- nor
    //       that the file loads. The proxy is tightened past a bare
    //       source.find("WallpaperItem"), which passes on the word appearing in a comment,
    //       in a string, as a child element, or as a property value: comments and string
    //       bodies are blanked first, imports and pragmas are skipped, and the identifier
    //       must be the first declaration in the file and be followed by `{`. The expected
    //       spelling is derived from the import rather than hard-coded, because
    //       `import ... as P` with a `P.WallpaperItem` root is equally correct QML and a
    //       fixed expectation would fail a file that is right.
    // CONTROL: five mutations of backend/kde/kpackage/contents/ui/main.qml, each reverted.
    //       The file moved aside: `FAIL: the package ships contents/ui/main.qml, the
    //       wallpaper's entry point` with the expected path -- a failure, not a skip, which
    //       is what an absent file must be. Root changed to Item: `FAIL: the root element
    //       of main.qml is the module's
    //       WallpaperItem`, naming "Item". The same, plus "WallpaperItem { }" added to the
    //       header comment -- the case a substring search would pass: still red, unchanged.
    //       `import org.kde.plasma.plasmoid` deleted: `FAIL: main.qml imports
    //       org.kde.plasma.plasmoid ...`. The root's opening brace removed: `FAIL:
    //       main.qml's WallpaperItem is an object declaration, not a bare word`. Aliasing
    //       the import alone is no longer a failure, and must not be: `import ... as P` with
    //       `P.WallpaperItem {` is correct, and the scanner self-test covers that pair.
    // COVERAGE-ASSERT: the scanner is checked against thirteen cases of its own before it is
    //       trusted -- six that its blanking removes comment and string bodies while keeping
    //       line boundaries, six that it picks the right root, one that it reads an alias and
    //       the qualified root it licenses -- and the count of non-blank lines it saw is
    //       asserted. Controls: the blanking replaced by a no-op produced named scanner
    //       errors and withheld S9.4 as a harness failure; dropping the `out[k] != '\n'`
    //       exemption reddened the line-boundary arm; main.qml wrapped entirely in /* */
    //       produced `FAIL: main.qml has content the QML scanner could see`. All reverted.
    const QmlShape shape = scanQml(*source);

    check(shape.contentLines > 0, "main.qml has content the QML scanner could see");
    if (shape.contentLines == 0) return;

    // Every import of the module, not one of them. QML permits the same module to be
    // imported more than once, and the root is correct if it matches the spelling *any* of
    // them licenses -- unaliased, `WallpaperItem`; behind `as P`, `P.WallpaperItem`. Taking
    // a single import and treating it as authoritative made the verdict depend on which one
    // came last in the file, which is not a property the goal cares about.
    std::vector<std::string> licensed;
    for (const QmlImport& imp : shape.imports) {
        if (imp.module != "org.kde.plasma.plasmoid") continue;
        licensed.push_back(imp.alias.empty() ? "WallpaperItem" : imp.alias + ".WallpaperItem");
    }
    check(!licensed.empty(), "main.qml imports org.kde.plasma.plasmoid, the module WallpaperItem comes from");
    if (licensed.empty()) return;

    // A bare name under an aliased-only import still fails, and must: that name really does
    // not resolve.
    const bool rootOk = std::find(licensed.begin(), licensed.end(), shape.rootType) != licensed.end();
    check(rootOk, "the root element of main.qml is the module's WallpaperItem");
    if (!rootOk) {
        std::fprintf(stderr, "  the first object declared in %s is \"%s\"; this file's imports spell the type",
                     mainQml.string().c_str(), shape.rootType.c_str());
        for (const std::string& s : licensed) std::fprintf(stderr, " \"%s\"", s.c_str());
        std::fprintf(stderr, "\n");
    } else {
        check(shape.rootFollowedByBrace, "main.qml's WallpaperItem is an object declaration, not a bare word");
    }
}

// S9.5
void runConfigQmlChecks() {
    const std::filesystem::path configQml = g_packageDir / "contents" / "ui" / "config.qml";
    // GOAL: S9.5 -- the settings page the package promises, contents/ui/config.qml, exists.
    //       False if it is missing; Plasma opens it when the user picks WallpaperX in the
    //       wallpaper chooser and clicks its settings, and an absent file gives an empty
    //       dialog with nothing logged.
    // WHY-THIS-SHAPE: is_regular_file rather than exists(), and non-empty on top of that. A
    //       directory named config.qml and a zero-byte config.qml both satisfy "exists"
    //       while producing exactly the empty dialog the goal is about, so bare existence
    //       would pass on two of the states it is meant to catch.
    // CONTROL: moved backend/kde/kpackage/contents/ui/config.qml aside and ran; observed
    //       `FAIL: the package ships contents/ui/config.qml, the settings page` with the
    //       expected path printed. Separately emptied it; observed `FAIL: contents/ui/
    //       config.qml is not empty`. Both restored and compared byte for byte against a
    //       copy taken first -- identical.
    std::string why;
    switch (presenceOf(configQml, why)) {
        case Presence::Unknowable:
            notVerified("S9.5", "whether the package ships contents/ui/config.qml could not be determined -- " + why);
            return;
        case Presence::Missing:
            check(false, "the package ships contents/ui/config.qml, the settings page");
            std::fprintf(stderr, "  expected a regular file at %s\n", configQml.string().c_str());
            return;
        case Presence::Present:
            break;
    }

    // A stat that fails is not the file being empty, and announcing it as such would send
    // the reader to the package for a host fault.
    std::error_code ec;
    const auto size = std::filesystem::file_size(configQml, ec);
    if (ec) {
        notVerified("S9.5", "contents/ui/config.qml is present but its size could not be read -- " + ec.message());
        return;
    }
    check(size > 0, "contents/ui/config.qml is not empty");
}

}  // namespace

int main() {
    wallpaperx::testing::g_testName = "kpackage_metadata_test";

    g_backendDir = std::filesystem::path(WALLPAPERX_BACKEND_SOURCE_DIR);
    g_packageDir = g_backendDir / kPackageDirArgument;

    // The source tree missing is this run's setup failing, not the package being wrong, so it
    // is a named skip. A source tree that is present but has no kde/kpackage directory is the
    // opposite -- that is the package being absent, and every phase below says so.
    // CONTROL: built this file by hand with WALLPAPERX_BACKEND_SOURCE_DIR pointing at
    //       /nonexistent/backend; observed the NOT VERIFIED line and exit 77, which ctest
    //       reads as a skip -- no FAIL was printed against backend/kde/kpackage/.
    std::error_code ec;
    if (!std::filesystem::is_directory(g_backendDir, ec) || ec) {
        notVerified("S9.1-S9.5", "the backend source directory baked in at build time is not there now: " +
                                     g_backendDir.string());
        std::printf("kpackage_metadata_test: nothing could be checked -- reported to ctest as a skip, not a pass.\n");
        return wallpaperx::testing::kNotVerifiedExitCode;
    }

    struct Phase {
        const char* name;
        void (*run)();
    };
    static const Phase kPhases[] = {
        {"metadata.json parses (S9.1)", runMetadataParseChecks},
        {"the package structure Plasma looks for (S9.2)", runStructureChecks},
        {"one plugin id, in the two places this process can read it (S9.3)", runIdentifierAgreementChecks},
        {"the wallpaper's QML entry point (S9.4)", runWallpaperQmlChecks},
        {"the settings page (S9.5)", runConfigQmlChecks},
    };
    for (const Phase& phase : kPhases) {
        try {
            phase.run();
        } catch (const std::exception& e) {
            // Attributed to this file, not to the package: an exception escaping a phase
            // comes from the parsing helpers here at least as often as it says anything
            // about backend/kde/kpackage/.
            harnessFailure(std::string("the \"") + phase.name + "\" phase threw: " + e.what());
        }
    }

    // Asked before the not-verified count, so a run that is both broken and incomplete
    // reports as broken rather than hiding failures behind ctest's "Skipped". Harness faults
    // are counted apart from check failures so the summary cannot send a reader to the
    // package for a fault in this file -- but they are equally red, never a skip, because a
    // harness that cannot be trusted must not report a verdict.
    const bool checksFailed = wallpaperx::testing::reportCheckFailures();
    if (g_harnessFaults > 0) {
        std::fprintf(stderr, "kpackage_metadata_test: %d fault(s) in this test file, not in the package\n",
                     g_harnessFaults);
    }
    if (checksFailed || g_harnessFaults > 0) {
        return 1;
    }
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
