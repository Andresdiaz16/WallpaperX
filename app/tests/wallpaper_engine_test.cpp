// SPDX-License-Identifier: GPL-2.0-or-later

// Wallpaper Engine's Workshop items in the library: shared/steam_libraries.cpp (libraryfolders.vdf),
// shared/we_project.cpp (project.json), app/library/wallpaper_engine.cpp (finding and reading items)
// and app/library/library_manager.cpp listing and watching them. Every Steam folder here is a fake one
// in the test's own scratch folder. No bus.

#include "library_manager.h"
#include "steam_libraries.h"
#include "wallpaper_engine.h"
#include "we_fixtures.h"
#include "we_project.h"
#include "wpx_fixtures.h"
#include "wpx_package.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QTemporaryDir>
#include <QThreadPool>

#include <QSet>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <future>
#include <memory>

#include <dirent.h>
#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace wallpaperx;
using namespace wallpaperx::apptest;
using wallpaperx::testing::harnessFailure;
using wallpaperx::testing::notVerified;

namespace {

constexpr int kSettleMs = 30;
constexpr int kWaitMs = 5000;

QString rows(const QList<Wallpaper>& list) {
    QStringList out;
    for (const Wallpaper& w : list) {
        out.append(QStringLiteral("%1%2%3").arg(w.displayTitle(), w.usable() ? QString() : QStringLiteral("(unusable)"),
                                                w.fromWallpaperEngine() ? QStringLiteral("[WE]") : QString()));
    }
    return out.join(u',');
}

const Wallpaper* row(const QList<Wallpaper>& list, const QString& path) {
    const auto it = std::ranges::find(list, path, &Wallpaper::filePath);
    return it == list.end() ? nullptr : &*it;
}

std::unique_ptr<LibraryManager> manager(const QString& library, const QStringList& steamRoots) {
    return std::make_unique<LibraryManager>(library, kSettleMs, 200, LibraryManager::Watching::Inotify, 2000, steamRoots);
}

bool listsWith(LibraryManager& m, const std::function<bool(const QList<Wallpaper>&)>& pred) {
    return waitUntil([&] { return m.state() == LibraryManager::State::Ready && pred(m.wallpapers()); }, kWaitMs);
}

// ---- libraryfolders.vdf ------------------------------------------------------------------------

void libraryList() {
    // Steam's own layout: nested blocks, tabs, quoted keys and values, plus what the format allows
    // beside it: a comment, a bare word, an escaped quote and backslash.
    const std::string text = "// written by Steam\n"
                             "\"libraryfolders\"\n{\n"
                             "\t\"0\"\n\t{\n\t\t\"path\"\t\t\"/home/u/.local/share/Steam\"\n\t\t\"label\"\t\t\"\"\n"
                             "\t\t\"apps\"\n\t\t{\n\t\t\t\"228980\"\t\t\"1\"\n\t\t}\n\t}\n"
                             "\t\"1\"\n\t{\n\t\t\"PATH\"\t\t\"/mnt/games \\\"quoted\\\" \\\\ lib\"\n\t}\n"
                             "\t\"2\" { path relative/not/kept }\n"
                             "\tcontentstatsid \"-123\"\n"
                             "}\n";
    // GOAL: P4.1 -- every absolute library `path` is returned in file order, keys matched without case.
    // CONTROL: shared/steam_libraries.cpp, 2026-11-11, control_batch, each alone, reverted: the "path" key's
    //       `equalsIgnoringAsciiCase(field.key, "path")` made `field.key == "path"` -> red, "libraryfolders.vdf
    //       read as [...]"; that line's `path->starts_with('/')` made `true` -> red, same line.
    const auto paths = steam::libraryFolders(text);
    const std::vector<std::string> want{"/home/u/.local/share/Steam", "/mnt/games \"quoted\" \\ lib"};
    expect(paths && *paths == want,
           QStringLiteral("libraryfolders.vdf read as [%1], error \"%2\"")
               .arg(paths ? QString::fromStdString([&] {
                                std::string all;
                                for (const auto& p : *paths) all += p + '|';
                                return all;
                            }())
                          : QString(),
                    paths ? QString() : QString::fromStdString(paths.error())));

    const std::pair<const char*, const char*> refused[] = {
        {"\"libraryfolders\" { \"0\" { \"path\" \"/a", "quoted string that never ends"},
        {"\"libraryfolders\" { \"0\" { \"path\" \"/a\" }", "ends inside a block"},
        {"\"libraryfolders\" { } }", "no block open"},
        {"\"libraryfolders\" { \"0\" }", "key with no value"},
        {"\"other\" { }", "no \"libraryfolders\" block"},
        {"{ }", "block with no key"},
    };
    for (const auto& [bad, why] : refused) {
        const auto read = steam::libraryFolders(bad);
        expect(!read && read.error().find(why) != std::string::npos && read.error().starts_with("libraryfolders.vdf "),
               QStringLiteral("the list %1 was not refused for \"%2\": %3")
                   .arg(QString::fromLatin1(bad), QString::fromLatin1(why),
                        read ? QStringLiteral("accepted") : QString::fromStdString(read.error())));
    }
    // GOAL: P4.1 -- a list nested deeper than 32 blocks is refused for its depth.
    // WHY-THIS-SHAPE: every block is closed, so the depth limit is the only reason to refuse it; an unclosed
    //       deep list would be refused as "ends inside a block" with the limit gone.
    // CONTROL: shared/steam_libraries.cpp, 2026-11-11, control_batch, reverted: `if (depth + 1 > kMaxDepth)`
    //       made `if (false)` -> red, "a list nested 40 blocks deep was not refused for its depth".
    std::string deep = "\"libraryfolders\" { ";
    for (int i = 0; i < 40; ++i) deep += "\"k\" { ";
    deep += std::string(41, '}');
    const auto deepRead = steam::libraryFolders(deep);
    expect(!deepRead && deepRead.error().find("nests more than 32 blocks deep") != std::string::npos,
           QStringLiteral("a list nested 40 blocks deep was not refused for its depth: %1")
               .arg(deepRead ? QStringLiteral("accepted") : QString::fromStdString(deepRead.error())));

    // GOAL: P4.19 -- in the older layout a key of digits only, mapping straight to an absolute path, is a
    //       library; a key not all digits (Steam's own counters, an empty key) or a relative path is not.
    // WHY-THIS-SHAPE: each refused entry has an absolute-looking value or a numbered key, so dropping
    //       either half of the test lets exactly one wrong entry through and the whole list is compared.
    // CONTROL: shared/steam_libraries.cpp, 2026-11-11, control_batch, each alone, reverted, each red with
    //       "P4.19: the older libraryfolders.vdf read as [...]": `numbered` made `true` -> [/1700000000|...|/empty/key|...];
    //       `numbered && direct->starts_with('/')` made `numbered` -> [...|relative/lib|...]; the
    //       `!library.key.empty() &&` deleted -> [...|/empty/key|...]; the direct `paths.push_back` deleted -> [].
    const std::string older = "\"LibraryFolders\"\n{\n\t\"TimeNextStatsReport\"\t\t\"/1700000000\"\n"
                              "\t\"ContentStatsID\"\t\t\"-4\"\n\t\"1\"\t\t\"/mnt/games/SteamLibrary\"\n"
                              "\t\"2\"\t\t\"relative/lib\"\n\t\"\"\t\t\"/empty/key\"\n\t\"3\"\t\t\"/media/second\"\n}\n";
    const auto olderPaths = steam::libraryFolders(older);
    const std::vector<std::string> olderWant{"/mnt/games/SteamLibrary", "/media/second"};
    expect(olderPaths && *olderPaths == olderWant,
           QStringLiteral("P4.19: the older libraryfolders.vdf read as [%1], error \"%2\"")
               .arg(olderPaths ? QString::fromStdString([&] {
                                     std::string all;
                                     for (const auto& p : *olderPaths) all += p + '|';
                                     return all;
                                 }())
                               : QString(),
                    olderPaths ? QString() : QString::fromStdString(olderPaths.error())));
}

// ---- project.json ------------------------------------------------------------------------------

void projectFile() {
    // Every setting type the spec lists, in an order different from the file's.
    const std::string full = R"({
        "type" : "Scene", "file" : "scene.json", "preview" : "preview.gif", "title" : "Night",
        "tags" : ["Anime"], "workshopid" : "99",
        "general" : { "supportsaudioprocessing" : true, "properties" : {
            "schemecolor" : { "order" : 0, "text" : "ui_browse_properties_scheme_color", "type" : "color", "value" : "0.98039 0.84314 0.67843" },
            "clock" : { "order" : 105, "index" : 5, "text" : "<b>Clock</b>", "type" : "bool", "value" : true },
            "size" : { "order" : 102, "text" : "Size", "type" : "slider", "value" : 3, "min" : 1, "max" : 10, "step" : 1, "precision" : 0, "fraction" : false },
            "mode" : { "order" : 103, "text" : "Mode", "type" : "combo", "value" : "2", "options" : [ { "label" : "One", "value" : "1" }, { "label" : "Two", "value" : "2" } ], "condition" : "clock.value" },
            "name" : { "order" : 104, "text" : "Name", "type" : "textinput", "value" : "x" },
            "note" : { "order" : 101, "text" : "<h3>Note</h3>", "type" : "text", "value" : false },
            "grp" : { "order" : 106, "text" : "Group", "type" : "group", "value" : "" },
            "bg" : { "order" : 107, "text" : "Background Image", "type" : "scenetexture", "value" : "" },
            "heading" : { "order" : 108, "text" : "Heading only" },
            "later" : { "order" : 109, "text" : "?", "type" : "hologram", "value" : 1 }
        } } })";
    const auto project = we::parseProject(full);
    if (!project) {
        expect(false, QStringLiteral("a full scene project.json was refused: %1").arg(QString::fromStdString(project.error())));
    } else {
        QStringList order;
        for (const we::Setting& s : project->settings) order.append(QString::fromStdString(s.name));
        using T = we::Setting::Type;
        const auto type = [&](const char* name) {
            const auto it = std::ranges::find(project->settings, std::string(name), &we::Setting::name);
            return it == project->settings.end() ? std::optional<we::Setting>() : std::optional(*it);
        };
        const auto colour = type("schemecolor");
        const auto size = type("size");
        const auto mode = type("mode");
        const auto heading = type("heading");
        const auto later = type("later");
        const auto bg = type("bg");
        const auto note = type("note");
        expect(project->kind == we::Project::Kind::Scene && project->file == "scene.json" && project->preview == "preview.gif" &&
                   project->title == "Night" && project->tags == std::vector<std::string>{"Anime"} &&
                   order.join(u',') == u"schemecolor,note,size,mode,name,clock,grp,bg,heading,later",
               QStringLiteral("the scene project.json read as kind %1, settings [%2]").arg(int(project->kind)).arg(order.join(u',')));
        // A missing setting or a value of another type fails here; no read can throw or dereference nothing.
        // CONTROL: shared/we_project.cpp, 2026-11-11, control_batch, reverted: the slider's value stored as
        //       text -> red, "the scene's settings were not read as written", the next checks still ran.
        const auto* rgb = colour ? std::get_if<we::Setting::Rgb>(&colour->value) : nullptr;
        const auto* sizeValue = size ? std::get_if<double>(&size->value) : nullptr;
        const auto* modeValue = mode ? std::get_if<std::string>(&mode->value) : nullptr;
        expect(colour && colour->type == T::Color && rgb && (*rgb)[1] == 0.84314 &&
                   size && size->type == T::Slider && sizeValue && *sizeValue == 3 && size->minimum == 1 && size->maximum == 10 &&
                   size->step == 1.0 && size->precision == 0 && !size->fraction && mode && mode->type == T::Combo &&
                   modeValue && *modeValue == "2" && mode->options.size() == 2 && mode->condition == "clock.value" &&
                   heading && heading->type == T::Label && later && later->type == T::Unknown && later->typeName == "hologram" &&
                   bg && bg->type == T::SceneTexture && note && note->type == T::Text,
               QStringLiteral("the scene's settings were not read as written"));
    }

    // A video as the spec shows them: "Video" in capitals, "general" null, an integer workshop id.
    const auto video = we::parseProject(R"({"type":"Video","file":"Clip (1) it's.mp4","preview":"preview.jpg","title":"T","general":null,"workshopid":805})");
    expect(video && video->kind == we::Project::Kind::Video && video->file == "Clip (1) it's.mp4" && video->settings.empty(),
           QStringLiteral("a video with a null \"general\" was not read: %1").arg(video ? QString() : QString::fromStdString(video.error())));

    const std::pair<const char*, const char*> refused[] = {
        {"{", "is not valid JSON"},
        {"[]", "is not a JSON object"},
        {R"({"file":"a.mp4"})", "has no type"},
        {R"({"type":"video"})", "does not name its file"},
        {R"({"type":"video","file":"../a.mp4"})", "does not name its file"},
        {R"({"type":"video","file":"/a.mp4"})", "does not name its file"},
        {R"({"type":"video","file":"a.mp4","preview":"x/../../p.jpg"})", "does not name its preview"},
        {R"({"type":"video","file":"a.mp4","tags":"x"})", "tags that are not a list"},
        {R"({"type":"video","file":"a.mp4","preview":"../x"})", "does not name its preview"},
    };
    // Settings never refuse the item: a malformed one is left out with its sentence.
    const std::pair<const char*, const char*> dropped[] = {
        {R"({"type":"video","file":"a.mp4","general":5})", "\"general\" that is not an object"},
        {R"({"type":"video","file":"a.mp4","general":{"properties":[]}})", "settings that are not an object"},
        {R"({"type":"video","file":"a.mp4","general":{"properties":{"c":{"type":"color","value":"1 2"}}}})", "three numbers"},
        {R"({"type":"video","file":"a.mp4","general":{"properties":{"c":{"type":"color","value":"1  2 3"}}}})", "three numbers"},
        {R"({"type":"video","file":"a.mp4","general":{"properties":{"s":{"type":"slider","value":1,"min":0}}}})", "numeric value, min and max"},
        {R"({"type":"video","file":"a.mp4","general":{"properties":{"m":{"type":"combo","value":"1","options":[{"label":"a","value":1}]}}}})", "an option that is not"},
        {R"({"type":"video","file":"a.mp4","general":{"properties":{"b":{"type":"bool","value":1}}}})", "true or false"},
        {R"({"type":"video","file":"a.mp4","general":{"properties":{"b":{"type":"bool","value":true,"order":1.5}}}})", "order is not a whole number"},
    };
    // GOAL: P4.4 -- each malformed form of the item is refused, and each malformed setting left out, with
    //       its own "project.json ..." sentence (the scene above shows settings sorted by `order`).
    // CONTROL: shared/we_project.cpp, 2026-11-11, control_batch, each alone, reverted: the order check's
    //       `is_number_integer()` made `is_number()` -> red, "... was not left out for "order is not a whole
    //       number""; the switch's sentence changed -> red, "... was not left out for "true or false""; the
    //       `stable_sort` by order deleted -> red, "the scene project.json read as kind 0, settings [...]". All
    //       three re-run 2026-11-11, same lines.
    for (const auto& [bad, why] : refused) {
        const auto read = we::parseProject(bad);
        expect(!read && read.error().find(why) != std::string::npos && read.error().starts_with("project.json "),
               QStringLiteral("project.json %1 was not refused for \"%2\": %3")
                   .arg(QString::fromLatin1(bad), QString::fromLatin1(why),
                        read ? QStringLiteral("accepted") : QString::fromStdString(read.error())));
    }
    for (const auto& [bad, why] : dropped) {
        const auto read = we::parseProject(bad);
        const bool left = read && read->settings.empty() && read->settingProblems.size() == 1 &&
                          read->settingProblems[0].find(why) != std::string::npos && read->settingProblems[0].starts_with("project.json ");
        expect(left, QStringLiteral("project.json %1 was not left out for \"%2\": %3")
                         .arg(QString::fromLatin1(bad), QString::fromLatin1(why),
                              !read ? QString::fromStdString(read.error())
                                    : QStringLiteral("%1 setting(s), problems [%2]")
                                          .arg(read->settings.size())
                                          .arg(QString::fromStdString(read->settingProblems.empty() ? std::string() : read->settingProblems[0]))));
    }

    // GOAL: P4.21 -- an empty or null preview is no preview, not a refusal.
    // CONTROL: shared/we_project.cpp, 2026-11-11, control_batch, each alone, reverted: parseProject()'s
    //       `!preview->is_null() &&` deleted -> red, "P4.21: project.json {...,"preview":null} read as project.json
    //       does not name its preview ..."; its empty-string clause deleted -> red, the same for "preview":"".
    for (const char* json : {R"({"type":"video","file":"a.mp4","preview":""})", R"({"type":"video","file":"a.mp4","preview":null})"}) {
        const auto read = we::parseProject(json);
        expect(read && read->preview.empty() && read->file == "a.mp4",
               QStringLiteral("P4.21: project.json %1 read as %2").arg(QString::fromLatin1(json), read ? QString::fromStdString(read->preview) : QString::fromStdString(read.error())));
    }

    // GOAL: P4.23 -- a setting type name is matched without case; P4.24 -- an `order` above the largest
    //       signed 64-bit value sorts last instead of wrapping to a negative.
    // WHY-THIS-SHAPE: a wrapped order (-1) still sorts after -3, so a negative order is in the list too, and
    //       the whole order is compared; a label (no type) is used for the orders so no value can refuse them.
    // CONTROL: shared/we_project.cpp, 2026-11-11, control_batch, each alone, reverted: wholeNumber()'s clamp
    //       deleted -> red, "P4.24: settings sorted [low,big,c,five]"; typeOf()'s `equalsIgnoringAsciiCase` made
    //       `==` -> red, "P4.23: a "Color" setting read as type 9, problems 0".
    const auto ordered = we::parseProject(
        R"({"type":"video","file":"a.mp4","general":{"properties":{"big":{"order":18446744073709551615},"c":{"type":"Color","value":"0.1 0.2 0.3"},"five":{"order":5},"low":{"order":-3}}}})");
    QStringList names;
    if (ordered) {
        for (const we::Setting& s : ordered->settings) names.append(QString::fromStdString(s.name));
    }
    expect(ordered && names.join(u',') == u"low,c,five,big",
           QStringLiteral("P4.24: settings sorted [%1], error \"%2\"").arg(names.join(u','), ordered ? QString() : QString::fromStdString(ordered.error())));
    const auto colour = ordered ? std::ranges::find(ordered->settings, std::string("c"), &we::Setting::name) : std::vector<we::Setting>::const_iterator();
    const bool isColour = ordered && colour != ordered->settings.end() && colour->type == we::Setting::Type::Color && ordered->settingProblems.empty();
    expect(isColour, QStringLiteral("P4.23: a \"Color\" setting read as type %1, problems %2")
                         .arg(ordered && colour != ordered->settings.end() ? int(colour->type) : -1)
                         .arg(ordered ? int(ordered->settingProblems.size()) : -1));
}

// ---- finding Steam -----------------------------------------------------------------------------

void steamRoots(const QString& root) {
    // Resolved, as defaultSteamRoots() resolves a root that exists: a link above the scratch folder
    // must not read as a wrong root.
    const QString base = QFileInfo(root).canonicalFilePath();
    if (base.isEmpty()) {
        harnessFailure("steamRoots: the scratch folder could not be resolved");
        return;
    }
    const QString home = base + QStringLiteral("/roots-home");
    const QString data = base + QStringLiteral("/roots-data");
    const QByteArray savedHome = qgetenv("HOME");
    const QByteArray savedData = qgetenv("XDG_DATA_HOME");
    qputenv("HOME", QFile::encodeName(home));
    qputenv("XDG_DATA_HOME", QFile::encodeName(data));
    // ~/.steam/steam is a link to $XDG_DATA_HOME/Steam, as Steam makes it; ~/.steam/root does not exist.
    const bool made = QDir().mkpath(data + QStringLiteral("/Steam")) && QDir().mkpath(home + QStringLiteral("/.steam")) &&
                      QFile::link(data + QStringLiteral("/Steam"), home + QStringLiteral("/.steam/steam"));
    const QStringList roots = defaultSteamRoots();
    qputenv("HOME", savedHome);
    qputenv("XDG_DATA_HOME", savedData);
    if (!made) {
        harnessFailure("steamRoots: the fake home could not be made");
        return;
    }
    const QStringList want{data + QStringLiteral("/Steam"), home + QStringLiteral("/.local/share/Steam"),
                           home + QStringLiteral("/.steam/root")};
    expect(roots == want, QStringLiteral("the Steam roots are [%1], not [%2]").arg(roots.join(u", "), want.join(u", ")));

    // No list: the root is its own only library. A list: the root first, then each it names, once.
    // GOAL: P4.2 -- a missing list is the root alone; a list that cannot be parsed is an error.
    // CONTROL: app/library/wallpaper_engine.cpp, 2026-11-11, control_batch, reverted (re-run on the resolving
    //       steamLibrariesOf()): its ENOENT `return QStringList{root};` made an error -> red, "a root with no
    //       list has libraries [x]".
    const QString bare = root + QStringLiteral("/bare-steam");
    const auto alone = steamLibrariesOf(bare);
    expect(alone && *alone == QStringList{bare}, QStringLiteral("a root with no list has libraries [%1]").arg(alone ? alone->join(u",") : alone.error()));
    const QString listed = root + QStringLiteral("/listed-steam");
    if (const QString why = writeSteamRoot(listed, {listed, QStringLiteral("/other/lib/"), QStringLiteral("/other/lib")}); !why.isEmpty()) {
        harnessFailure("steamRoots: " + why.toStdString());
        return;
    }
    const auto libraries = steamLibrariesOf(listed);
    expect(libraries && *libraries == QStringList{listed, QStringLiteral("/other/lib")},
           QStringLiteral("a root listing itself and /other/lib twice has libraries [%1]").arg(libraries ? libraries->join(u",") : libraries.error()));
    if (!writeBytes(listed + QStringLiteral("/steamapps/libraryfolders.vdf"), "\"libraryfolders\" {")) {
        harnessFailure("steamRoots: the list could not be broken");
        return;
    }
    const auto broken = steamLibrariesOf(listed);
    expect(!broken && broken.error().contains(QStringLiteral("ends inside a block")),
           QStringLiteral("a cut-off list read as [%1]").arg(broken ? broken->join(u",") : broken.error()));

    // GOAL: P4.20 -- one library written two ways is listed once: the root, given through a link and named
    //       in the list by that link, adds nothing; a link to another library beside its real path gives it one
    //       entry, its resolved path.
    // WHY-THIS-SHAPE: the root is given through a link, so it is kept as given while the list's entry for it
    //       resolves to the real path: only the compare with the resolved root drops that entry, not the
    //       duplicate check. Given by its real path, the duplicate check alone would hide a missing compare.
    // CONTROL: app/library/wallpaper_engine.cpp, 2026-11-11, control_batch, each alone, reverted: resolvedPath()
    //       never resolving links -> red, "P4.20: ... read as [.../twoways-rootlink,.../twoways-otherlink,...]";
    //       steamLibrariesOf()'s `library != self &&` deleted -> red, "... read as [.../twoways-rootlink,
    //       .../twoways-steam,...]" (with the root given by its real path, that break stayed green). Both re-run
    //       2026-11-11, same lines.
    const QString twoWays = base + QStringLiteral("/twoways-steam");
    const QString other = base + QStringLiteral("/twoways-lib");
    const bool linked = QDir().mkpath(other) && QDir().mkpath(twoWays) &&
                        QFile::link(twoWays, base + QStringLiteral("/twoways-rootlink")) &&
                        QFile::link(other, base + QStringLiteral("/twoways-otherlink")) &&
                        writeSteamRoot(twoWays, {base + QStringLiteral("/twoways-rootlink"), base + QStringLiteral("/twoways-otherlink"), other}).isEmpty();
    if (!linked) {
        harnessFailure("steamRoots: the linked libraries could not be made");
        return;
    }
    const QString rootLink = base + QStringLiteral("/twoways-rootlink");
    const auto once = steamLibrariesOf(rootLink);
    expect(once && *once == QStringList{rootLink, other},
           QStringLiteral("P4.20: a root named through a link and a library named two ways read as [%1]").arg(once ? once->join(u",") : once.error()));
}

// ---- listing items -----------------------------------------------------------------------------

struct Steam {
    QString root;
    QString second;
    QString video;
    QString scene;
    QString web;
    QString broken;
    QString missing;
    QString odd;
};

// A Steam root listing itself and a second library; items in both, the install in the second.
[[nodiscard]] QString makeSteam(const QString& base, Steam& s) {
    s.root = base + QStringLiteral("/steam");
    s.second = base + QStringLiteral("/games");
    s.video = steamItemFolder(s.root) + QStringLiteral("/1001");
    s.scene = steamItemFolder(s.second) + QStringLiteral("/2002");
    s.web = steamItemFolder(s.second) + QStringLiteral("/2003");
    s.broken = steamItemFolder(s.second) + QStringLiteral("/2004");
    s.missing = steamItemFolder(s.second) + QStringLiteral("/2005");
    s.odd = steamItemFolder(s.second) + QStringLiteral("/2006");
    Entries web{{QStringLiteral("project.json"), projectBytes("web", QStringLiteral("Page"), QStringLiteral("index.html"))},
                {QStringLiteral("preview.jpg"), pngBytes(16, 9)},
                {QStringLiteral("index.html"), QByteArray("<html></html>")}};
    Entries missing = videoItem(QStringLiteral("Gone"));
    missing.removeLast();
    for (const QString& why : {writeSteamRoot(s.root, {s.root, s.second}),
                               writeItem(s.root, QStringLiteral("1001"), videoItem(QStringLiteral("Rain"))),
                               writeItem(s.second, QStringLiteral("2002"), sceneItem(QStringLiteral("Night City"))),
                               writeItem(s.second, QStringLiteral("2003"), web),
                               writeItem(s.second, QStringLiteral("2004"), {{QStringLiteral("project.json"), QByteArray("{ not json")}}),
                               writeItem(s.second, QStringLiteral("2005"), missing),
                               writeItem(s.second, QStringLiteral("2006"),
                                         {{QStringLiteral("project.json"), projectBytes("application", QStringLiteral("App"), QStringLiteral("a.exe"), {})}}),
                               writeFolder(steamInstallFolder(s.second) + QStringLiteral("/assets"), {})}) {
        if (!why.isEmpty()) {
            return why;
        }
    }
    return {};
}

void listing(const QString& base) {
    Steam s;
    if (const QString why = makeSteam(base + QStringLiteral("/listing"), s); !why.isEmpty()) {
        harnessFailure("listing: " + why.toStdString());
        return;
    }
    const QString wpx = base + QStringLiteral("/listing/wallpapers");
    if (const QString why = writeFolder(wpx + QStringLiteral("/ocean"), planEntries(kPlanManifest)); !why.isEmpty()) {
        harnessFailure("listing: " + why.toStdString());
        return;
    }
    auto m = manager(wpx, {s.root});
    m->rescan();
    if (!listsWith(*m, [](const QList<Wallpaper>& l) { return l.size() == 7; })) {
        expect(false, QStringLiteral("the library never listed the .wpx folder and six items: [%1]").arg(rows(m->wallpapers())));
        return;
    }
    const QList<Wallpaper> list = m->wallpapers();

    // GOAL: P4.5 -- the video is usable as a WallpaperX video, read in place (asset `<item>/<file>`), with
    //       the sound controls every video with sound gets and none of its Wallpaper Engine settings.
    // CONTROL: app/library/wallpaper_engine.cpp, 2026-11-11, control_batch, reverted: readWorkshopItem()'s
    //       usable row given `item.assetPath = path` after usableFolder() -> red, "the video item is [usable 1,
    //       WE 1, type video, asset <item>, ...]". (Its local `assetPath` now feeds only the sound probe.)
    const Wallpaper* video = row(list, s.video);
    QStringList keys;
    if (video) {
        for (const WallpaperProperty& p : video->properties) keys.append(p.key);
    }
    expect(video && video->usable() && video->fromWallpaperEngine() && video->type == u"video" && video->title == u"Rain" &&
               video->tags == QStringList{QStringLiteral("Landscape")} && video->fileName == u"1001" &&
               video->assetPath == s.video + QStringLiteral("/clip [1] it's.flv") && keys == QStringList{QStringLiteral("mute"), QStringLiteral("volume")} &&
               !video->previewSource().isEmpty() && video->audio == AudioStreams::Present,
           QStringLiteral("the video item is [usable %1, WE %2, type %3, asset %4, keys %5, preview %6]")
               .arg(video && video->usable()).arg(video && video->fromWallpaperEngine())
               .arg(video ? video->type : QString(), video ? video->assetPath : QString(), keys.join(u','),
                    video ? video->previewSource().toString() : QString()));
    if (video) {
        const auto preview = readWallpaperEntry(video->filePath, video->previewName, kMaxPreviewBytes);
        expect(preview && *preview == pngBytes(16, 9), QStringLiteral("the video item's preview could not be read through the provider's path"));
    }

    // GOAL: P4.6 -- a scene and a web item are listed, not selectable, keeping their title and preview.
    // CONTROL: app/library/wallpaper_engine.cpp, 2026-11-11, control_batch, reverted: the unsupported branch's
    //       `item.title = ...` deleted -> red, "... is [usable 0, title , problem ...]". Re-run 2026-11-11, same line.
    for (const auto& [path, title, problem] : {std::tuple{s.scene, "Night City", "Wallpaper Engine scenes are not supported yet."},
                                               std::tuple{s.web, "Page", "Wallpaper Engine web wallpapers are not supported."}}) {
        const Wallpaper* item = row(list, path);
        expect(item && !item->usable() && item->fromWallpaperEngine() && item->title == QString::fromUtf8(title) &&
                   item->problem == QString::fromUtf8(problem) && !item->previewSource().isEmpty() && !m->select(path),
               QStringLiteral("%1 is [usable %2, title %3, problem \"%4\", preview %5]")
                   .arg(path).arg(item && item->usable()).arg(item ? item->title : QString(), item ? item->problem : QString(),
                                                              item ? item->previewSource().toString() : QString()));
    }

    // Malformed ones are refused with a plain reason, as a broken .wpx folder is.
    for (const auto& [path, problem] : {std::pair{s.broken, QStringLiteral("project.json is not valid JSON (at byte 4).")},
                                        std::pair{s.missing, QStringLiteral("The folder has no clip [1] it's.flv, which project.json names.")},
                                        std::pair{s.odd, QStringLiteral("It is a Wallpaper Engine \"application\" wallpaper, which WallpaperX does not know.")}}) {
        const Wallpaper* item = row(list, path);
        expect(item && !item->usable() && item->problem == problem,
               QStringLiteral("%1's problem is \"%2\", not \"%3\"").arg(path, item ? item->problem : QStringLiteral("(not listed)"), problem));
    }
    // Usable first, then by title; a broken item has only its folder's name.
    expect(rows(list) == u"Ocean Waves,Rain[WE],2004(unusable)[WE],2005(unusable)[WE],App(unusable)[WE],Night City(unusable)[WE],Page(unusable)[WE]",
           QStringLiteral("the library lists [%1]").arg(rows(list)));
    expect(m->wallpaperEngineInstall() == steamInstallFolder(s.second),
           QStringLiteral("Wallpaper Engine's install is \"%1\", not \"%2\"").arg(m->wallpaperEngineInstall(), steamInstallFolder(s.second)));

    // Selecting the video gives the values Apply sends: the sound and position keys, nothing else.
    expect(m->select(s.video) && m->propertyValues().contains(QStringLiteral("volume")) && !m->propertyValues().contains(QStringLiteral("schemecolor")),
           QStringLiteral("the video item could not be selected with its values"));
}

// What every file below `top` is, so a scan can be shown to have changed none of it.
QStringList snapshot(const QString& top) {
    QStringList out;
    QDirIterator it(top, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        struct stat st {};
        ::lstat(QFile::encodeName(path).constData(), &st);
        out.append(QStringLiteral("%1 %2 %3 %4.%5").arg(path).arg(st.st_mode).arg(st.st_size).arg(st.st_mtim.tv_sec).arg(st.st_mtim.tv_nsec));
    }
    out.sort();
    return out;
}

// Gives every path back its owner's write bit when it goes, so an early return or a throw leaves no
// tree the scratch folder's removal cannot delete.
struct ModesRestored {
    QStringList paths;
    ModesRestored() = default;
    ModesRestored(const ModesRestored&) = delete;
    ModesRestored& operator=(const ModesRestored&) = delete;
    ~ModesRestored() {
        for (const QString& p : std::as_const(paths)) static_cast<void>(setMode(p, 0755));
    }
};

// P4.7: a Steam folder the user cannot write to still lists.
void readOnly(const QString& base) {
    Steam s;
    if (const QString why = makeSteam(base + QStringLiteral("/readonly"), s); !why.isEmpty()) {
        harnessFailure("readOnly: " + why.toStdString());
        return;
    }
    const QString top = base + QStringLiteral("/readonly");
    QStringList made;
    QDirIterator walk(top, QDir::AllEntries | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (walk.hasNext()) made.append(walk.next());
    ModesRestored restore;
    for (const QString& path : made) {
        restore.paths.append(path);
        if (!setMode(path, QFileInfo(path).isDir() ? 0555 : 0444)) {
            harnessFailure("readOnly: the fake Steam could not be made read-only");
            return;
        }
    }
    // CONTROL: app/library/wallpaper_engine.cpp, 2026-11-11, control_batch, reverted: readWorkshopItem()
    //       refusing an item folder it cannot write (`faccessat(..., W_OK)`) -> red, "a read-only Steam folder
    //       was not listed: [1001(unusable)[WE],...]".
    auto m = manager(base + QStringLiteral("/readonly-wallpapers"), {s.root});
    m->rescan();
    const bool listed = listsWith(*m, [](const QList<Wallpaper>& l) { return l.size() == 6; });
    expect(listed && m->select(s.video), QStringLiteral("a read-only Steam folder was not listed: [%1]").arg(rows(m->wallpapers())));
}

// P4.7: listing, selecting and the follow-up scan write nothing under the Steam folders.
void writesNothing(const QString& base) {
    Steam s;
    const QString top = base + QStringLiteral("/writes");
    if (const QString why = makeSteam(top, s); !why.isEmpty()) {
        harnessFailure("writesNothing: " + why.toStdString());
        return;
    }
    // GOAL: P4.7 -- scanning and selecting changes no entry, byte, mode or time under a Steam folder the
    //       app could write to.
    // WHY-THIS-SHAPE: the tree is left writable, since on a read-only one the kernel refuses a write before
    //       it shows; the pool is drained after the manager goes, so the follow-up scan every start runs has
    //       finished before the second snapshot, and no in-flight scan writes after it.
    // CONTROL: app/library/wallpaper_engine.cpp, 2026-11-11, control_batch, each alone, reverted: readWorkshopItem()
    //       creating `<item>/.wpx-cache` -> red, "listing the Workshop items changed something in the Steam
    //       folders"; readWorkshopItem() calling `::futimens(dir.get(), nullptr)` -> red, same line.
    const QStringList before = snapshot(top);
    {
        auto m = manager(base + QStringLiteral("/writes-wallpapers"), {s.root});
        m->rescan();
        const bool listed = listsWith(*m, [](const QList<Wallpaper>& l) { return l.size() == 6; });
        if (!listed || !m->select(s.video)) {
            expect(false, QStringLiteral("writesNothing: the fake Steam was not listed: [%1]").arg(rows(m->wallpapers())));
            return;
        }
    }
    QThreadPool::globalInstance()->waitForDone();
    const QStringList after = snapshot(top);
    QStringList changed;
    for (const QString& line : after) {
        if (!before.contains(line)) changed.append(line);
    }
    for (const QString& line : before) {
        if (!after.contains(line)) changed.append(QStringLiteral("gone: ") + line);
    }
    expect(changed.isEmpty() && before.size() > 10,
           QStringLiteral("listing the Workshop items changed something in the Steam folders (%1 entries): [%2]")
               .arg(before.size()).arg(changed.join(u" | ")));
}

// ---- Steam changing its folders ----------------------------------------------------------------

void steamChanges(const QString& base) {
    Steam s;
    if (const QString why = makeSteam(base + QStringLiteral("/changes"), s); !why.isEmpty()) {
        harnessFailure("steamChanges: " + why.toStdString());
        return;
    }
    auto m = manager(base + QStringLiteral("/changes-wallpapers"), {s.root});
    m->rescan();
    if (!listsWith(*m, [](const QList<Wallpaper>& l) { return l.size() == 6; }) || !m->select(s.video)) {
        expect(false, QStringLiteral("the fake Steam never listed: [%1]").arg(rows(m->wallpapers())));
        return;
    }

    // GOAL: P4.8, P4.3 -- items and libraries come and go with no rescan asked for; a cut-off list keeps
    //       the libraries it last listed and the selection in one.
    // CONTROL: app/library/library_manager.cpp, 2026-11-11, control_batch, each alone, reverted:
    //       watchItemFolders()'s connect deleted -> red, "an item added to a listed library never appeared";
    //       publish() forgetting a root's trusted list (`m_steamLibraries.remove(root)`) when it cannot be
    //       read -> red, "a cut-off libraryfolders.vdf lost the libraries it had listed"; publish()'s
    //       "may be missing" append deleted -> red, "no scan reported the cut-off libraryfolders.vdf".
    //       steamArrives is reached by either watcher alone: both connects deleted -> red, "items of a
    //       Steam root made after the app started never appeared: []".
    // An item subscribed to appears with no rescan asked for.
    const QString added = steamItemFolder(s.second) + QStringLiteral("/3003");
    if (const QString why = writeItem(s.second, QStringLiteral("3003"), videoItem(QStringLiteral("Snow"))); !why.isEmpty()) {
        harnessFailure("steamChanges: " + why.toStdString());
        return;
    }
    expect(listsWith(*m, [&](const QList<Wallpaper>& l) { const Wallpaper* w = row(l, added); return w && w->usable(); }),
           QStringLiteral("an item added to a listed library never appeared: [%1]").arg(rows(m->wallpapers())));

    // The open item removed: its row goes and the selection with it.
    QDir(s.video).removeRecursively();
    expect(listsWith(*m, [&](const QList<Wallpaper>& l) { return !row(l, s.video); }) && m->selectedPath().isEmpty(),
           QStringLiteral("a removed item stayed listed or selected: [%1], selected \"%2\"").arg(rows(m->wallpapers()), m->selectedPath()));

    // A library added to Steam's list is read, the list watched for it.
    const QString third = base + QStringLiteral("/changes/third");
    const QString thirdItem = steamItemFolder(third) + QStringLiteral("/4004");
    if (const QString why = writeItem(third, QStringLiteral("4004"), videoItem(QStringLiteral("Dunes"))); !why.isEmpty()) {
        harnessFailure("steamChanges: " + why.toStdString());
        return;
    }
    if (const QString why = writeSteamRoot(s.root, {s.root, s.second, third}); !why.isEmpty()) {
        harnessFailure("steamChanges: " + why.toStdString());
        return;
    }
    expect(listsWith(*m, [&](const QList<Wallpaper>& l) { return row(l, thirdItem) != nullptr; }),
           QStringLiteral("a library added to libraryfolders.vdf was never read: [%1]").arg(rows(m->wallpapers())));
    // Its new folder is watched too.
    const QString thirdAdded = steamItemFolder(third) + QStringLiteral("/4005");
    if (const QString why = writeItem(third, QStringLiteral("4005"), sceneItem(QStringLiteral("Moon"))); !why.isEmpty()) {
        harnessFailure("steamChanges: " + why.toStdString());
        return;
    }
    expect(listsWith(*m, [&](const QList<Wallpaper>& l) { return row(l, thirdAdded) != nullptr; }),
           QStringLiteral("an item added to the new library never appeared: [%1]").arg(rows(m->wallpapers())));

    // A list that cannot be read counts as what it last said; once whole again it is read again.
    if (!m->select(thirdItem) || !writeBytes(s.root + QStringLiteral("/steamapps/libraryfolders.vdf"), "\"libraryfolders\" { \"0\" {")) {
        harnessFailure("steamChanges: the list could not be cut off");
        return;
    }
    // Graded only on a published scan that read the cut-off list: its problem is the barrier, so a scan
    // still running cannot pass for one that kept the libraries.
    const bool reported = waitUntil([&] {
        return m->state() == LibraryManager::State::Ready &&
               std::ranges::any_of(m->problems(), [](const QString& p) { return p.startsWith(u"Some Wallpaper Engine items may be missing."); });
    }, kWaitMs);
    if (!reported) {
        expect(false, QStringLiteral("no scan reported the cut-off libraryfolders.vdf: problems [%1]").arg(m->problems().join(u'|')));
        return;
    }
    expect(row(m->wallpapers(), thirdItem) && m->selectedPath() == thirdItem,
           QStringLiteral("a cut-off libraryfolders.vdf lost the libraries it had listed: [%1]").arg(rows(m->wallpapers())));
    if (const QString why = writeSteamRoot(s.root, {s.root, s.second}); !why.isEmpty()) {
        harnessFailure("steamChanges: " + why.toStdString());
        return;
    }
    expect(listsWith(*m, [&](const QList<Wallpaper>& l) { return !row(l, thirdItem) && row(l, added); }) && m->selectedPath().isEmpty(),
           QStringLiteral("a library taken off the list stayed listed: [%1]").arg(rows(m->wallpapers())));
}

// A Steam root that does not exist yet lists nothing, then its items once it appears.
void steamArrives(const QString& base) {
    const QString root = base + QStringLiteral("/arrives/steam");
    auto m = manager(base + QStringLiteral("/arrives-wallpapers"), {root});
    m->rescan();
    expect(listsWith(*m, [](const QList<Wallpaper>& l) { return l.isEmpty(); }) && m->wallpaperEngineInstall().isEmpty(),
           QStringLiteral("a missing Steam root listed [%1], install \"%2\"").arg(rows(m->wallpapers()), m->wallpaperEngineInstall()));
    const QString item = steamItemFolder(root) + QStringLiteral("/5005");
    if (const QString why = writeItem(root, QStringLiteral("5005"), videoItem(QStringLiteral("Late"))); !why.isEmpty()) {
        harnessFailure("steamArrives: " + why.toStdString());
        return;
    }
    if (const QString why = writeSteamRoot(root, {root}); !why.isEmpty()) {
        harnessFailure("steamArrives: " + why.toStdString());
        return;
    }
    expect(listsWith(*m, [&](const QList<Wallpaper>& l) { const Wallpaper* w = row(l, item); return w && w->usable(); }),
           QStringLiteral("items of a Steam root made after the app started never appeared: [%1]").arg(rows(m->wallpapers())));
}

// ---- each source fails on its own --------------------------------------------------------------

// P4.12, P4.13: the .wpx folder failing costs only its own rows.
void sourceFails(const QString& base) {
    const QString top = base + QStringLiteral("/fails");
    const QString wpx = top + QStringLiteral("/wallpapers");
    const QString steam = top + QStringLiteral("/steam");
    const QString item = steamItemFolder(steam) + QStringLiteral("/1001");
    if (!writeFolder(wpx + QStringLiteral("/ocean"), planEntries(kPlanManifest)).isEmpty() || !writeSteamRoot(steam, {steam}).isEmpty() ||
        !writeItem(steam, QStringLiteral("1001"), videoItem(QStringLiteral("Rain"))).isEmpty()) {
        harnessFailure("sourceFails: the fake library and Steam could not be written");
        return;
    }
    auto m = manager(wpx, {steam});
    m->rescan();
    if (!listsWith(*m, [](const QList<Wallpaper>& l) { return l.size() == 2; }) || !m->select(item)) {
        expect(false, QStringLiteral("P4.12: the .wpx folder and the video never listed: [%1]").arg(rows(m->wallpapers())));
        return;
    }
    QList<LibraryManager::State> seen;
    QObject::connect(m.get(), &LibraryManager::stateChanged, [&] { seen.append(m->state()); });

    // GOAL: P4.12 -- with the .wpx folder made a file while a Steam root lists a video, the library is
    //       Ready, lists the video still selected, `problems` is exactly the folder's sentence and
    //       `unavailableReason` is empty.
    // WHY-THIS-SHAPE: the selection is made before the failure, so a failure that drops the list for
    //       a moment (or for good) shows as a lost selection, not only as a row count.
    // CONTROL: app/library/library_manager.cpp, 2026-11-11, control_batch, reverted: state()'s `||
    //       !m_model.wallpapers().isEmpty()` deleted -> red, "P4.12: with the .wpx folder a file the
    //       library is state 2, ..."; the scan returning before scanWorkshop when the folder fails -> red, same line.
    //       Both re-run 2026-11-11 on P4-1D's state() and readSources(), same line.
    const QString moved = wpx + QStringLiteral(".moved");
    if (!QDir().rename(wpx, moved) || !writeBytes(wpx, "a file")) {
        harnessFailure("sourceFails: the .wpx folder could not be replaced by a file");
        return;
    }
    m->rescan();
    const bool failed = waitUntil([&] { return !m->problems().isEmpty(); }, kWaitMs);
    expect(failed && m->state() == LibraryManager::State::Ready && m->problems() == QStringList{wpx + QStringLiteral(" is not a folder.")} &&
               m->unavailableReason().isEmpty() && rows(m->wallpapers()) == u"Rain[WE]" && m->selectedPath() == item,
           QStringLiteral("P4.12: with the .wpx folder a file the library is state %1, problems [%2], reason \"%3\", rows [%4], selected \"%5\"")
               .arg(int(m->state())).arg(m->problems().join(u'|'), m->unavailableReason(), rows(m->wallpapers()), m->selectedPath()));

    // GOAL: P4.13 -- when the last Wallpaper Engine item goes while the .wpx folder is failing, the
    //       state moves Ready -> Unavailable with exactly one stateChanged, the reason the folder's sentence.
    // WHY-THIS-SHAPE: the problems do not change here, only the list, so a manager that emitted only on a
    //       change of problems would miss the move; a pump after the wait lets any second emission land.
    // CONTROL: app/library/library_manager.cpp, 2026-11-11, control_batch, reverted: settle()'s `if
    //       (problemsChange || state() != stateBefore)` made `if (problemsChange)` -> red, "P4.13: after the
    //       last item went ... 0 stateChanged". Re-run 2026-11-11 on P4-1D's settle(), same red line.
    seen.clear();
    QDir(item).removeRecursively();
    const bool gone = waitUntil([&] { return m->state() == LibraryManager::State::Unavailable; }, kWaitMs);
    pump(100);
    expect(gone && seen == QList{LibraryManager::State::Unavailable} && m->unavailableReason() == wpx + QStringLiteral(" is not a folder.") &&
               m->wallpapers().isEmpty() && m->selectedPath().isEmpty(),
           QStringLiteral("P4.13: after the last item went the library is state %1, reason \"%2\", %3 row(s), with %4 stateChanged")
               .arg(int(m->state())).arg(m->unavailableReason()).arg(m->wallpapers().size()).arg(seen.size()));
    static_cast<void>(QFile::remove(wpx));
}

// Runs every scan queued or in flight, and the scans their results start, to the end. Rounds, not a
// check of the pool: a finished scan's result waits as a posted event while the pool reads idle.
void finishScans() {
    for (int i = 0; i < 4; ++i) {
        QThreadPool::globalInstance()->waitForDone();
        QCoreApplication::sendPostedEvents();
    }
}

// P4.14, P4.16, P4.17: lists and Workshop folders that cannot be read, and the order of problems.
void problemOrder(const QString& base) {
    if (::geteuid() == 0) {
        notVerified("P4.14, P4.16, P4.17", "running as root, which can list a mode-000 folder");
        return;
    }
    const QString top = base + QStringLiteral("/order");
    const QString wpx = top + QStringLiteral("/wallpapers");
    // Given in an order that is not the alphabetical one, so root order is told apart from a sorted map's.
    const QString zeta = top + QStringLiteral("/zeta-steam");
    const QString alpha = top + QStringLiteral("/alpha-steam");
    const QString beta = top + QStringLiteral("/beta-steam");
    const QString closedLibrary = top + QStringLiteral("/closed-games");
    const QString closed = steamItemFolder(closedLibrary);
    const QString beforeBlock = QStringLiteral("\"libraryfolders\" {");
    const bool made = QDir().mkpath(top) && writeBytes(wpx, "a file") && writeItem(zeta, QStringLiteral("1001"), videoItem(QStringLiteral("Zeta"))).isEmpty() &&
                      writeBytes(zeta + QStringLiteral("/steamapps/libraryfolders.vdf"), beforeBlock.toUtf8()) &&
                      writeItem(alpha, QStringLiteral("2001"), videoItem(QStringLiteral("Alpha"))).isEmpty() &&
                      writeBytes(alpha + QStringLiteral("/steamapps/libraryfolders.vdf"), beforeBlock.toUtf8()) &&
                      writeSteamRoot(beta, {beta, closedLibrary}).isEmpty() &&
                      writeItem(beta, QStringLiteral("3001"), videoItem(QStringLiteral("Beta"))).isEmpty() &&
                      writeItem(closedLibrary, QStringLiteral("4001"), videoItem(QStringLiteral("Hidden"))).isEmpty() && setMode(closed, 0);
    if (!made) {
        harnessFailure("problemOrder: the fake Steam roots could not be written");
        return;
    }
    const auto listProblem = [](const QString& root) {
        return QStringLiteral("Some Wallpaper Engine items may be missing. %1/steamapps/libraryfolders.vdf cannot be used: "
                              "libraryfolders.vdf ends inside a block (at byte 18).").arg(root);
    };
    auto m = manager(wpx, {zeta, alpha, beta});
    m->rescan();
    const bool settled = listsWith(*m, [](const QList<Wallpaper>& l) { return l.size() == 3; });
    const QStringList problems = m->problems();
    // GOAL: P4.14, P4.16, P4.17 -- a list that cannot be read adds one problem naming its path and its
    //       root's own items still list; an unlistable Workshop folder adds a problem and no row while the
    //       other items list and can be selected; the problems run library folder, lists in root order,
    //       then Workshop folders.
    // WHY-THIS-SHAPE: one manager holds all four failures, so order is asserted on one list rather than
    //       inferred from separate runs; the whole sentence is compared, so a list problem naming no path fails.
    // CONTROL: app/library/library_manager.cpp, 2026-11-11, control_batch, each alone, reverted: rescan()
    //       handing the scan its roots from `m_libraryListWatchers` (sorted by root) for `m_steamRoots` -> red,
    //       "P4.17: the problems are [...alpha-steam...zeta-steam...]"; problems() appending
    //       `m_problems->library` after the scan failure and `m_problems->steam` -> red, same line; publish()'s "may be missing" append deleted -> red,
    //       same line; publish()'s `problems.steam.append(unread->problem)` deleted -> red, same line;
    //       app/library/wallpaper_engine.cpp scanWorkshop()'s `scan.unread.append(...)` deleted -> red, same line.
    //       All five re-run 2026-11-11 on P4-1D's problems() and scanWorkshop(), same line.
    const bool order = problems.size() == 4 && problems[0] == wpx + QStringLiteral(" is not a folder.") && problems[1] == listProblem(zeta) &&
                       problems[2] == listProblem(alpha) && problems[3].startsWith(QStringLiteral("Wallpaper Engine's items cannot be listed. ")) &&
                       problems[3].contains(closed);
    expect(settled && order, QStringLiteral("P4.17: the problems are [%1] with rows [%2]").arg(problems.join(u'|'), rows(m->wallpapers())));
    const bool rowsRight = std::ranges::none_of(m->wallpapers(), [&](const Wallpaper& w) { return w.filePath.startsWith(closed); }) &&
                           m->select(steamItemFolder(beta) + QStringLiteral("/3001")) && m->select(steamItemFolder(zeta) + QStringLiteral("/1001"));
    expect(settled && rowsRight, QStringLiteral("P4.16: beside an unlistable Workshop folder the rows are [%1]").arg(rows(m->wallpapers())));

    // GOAL: P4.14 -- a list repaired clears its problem with no rescan asked for; the others stay.
    // WHY-THIS-SHAPE: the start-up follow-up scan is run to its end first. Repaired while it is still
    //       queued, the list is read whole by that follow-up, and the check passes with no list watcher.
    // CONTROL: app/library/library_manager.cpp, 2026-11-11, control_batch, reverted: the constructor's connect of each list watcher's scanDue deleted -> red,
    //       "P4.14: after zeta's list was repaired the problems are [...zeta-steam...]".
    finishScans();
    if (!writeSteamRoot(zeta, {zeta}).isEmpty()) {
        harnessFailure("problemOrder: zeta's list could not be repaired");
    } else {
        const bool cleared = waitUntil([&] { return m->problems().size() == 3; }, kWaitMs);
        expect(cleared && m->problems()[1] == listProblem(alpha) && m->selectedPath() == steamItemFolder(zeta) + QStringLiteral("/1001"),
               QStringLiteral("P4.14: after zeta's list was repaired the problems are [%1]").arg(m->problems().join(u'|')));
    }
    static_cast<void>(setMode(closed, 0755));
}

// Holds the global thread pool's only thread until released, so a scan started meanwhile waits.
class PoolHold {
public:
    PoolHold() : m_saved(QThreadPool::globalInstance()->maxThreadCount()) {
        QThreadPool::globalInstance()->waitForDone();
        QThreadPool::globalInstance()->setMaxThreadCount(1);
        std::shared_future<void> gate = m_gate.get_future().share();
        QThreadPool::globalInstance()->start([gate] { gate.wait(); });
    }
    PoolHold(const PoolHold&) = delete;
    PoolHold& operator=(const PoolHold&) = delete;
    void release() {
        if (!m_released) {
            m_released = true;
            m_gate.set_value();
        }
    }
    ~PoolHold() {
        release();
        QThreadPool::globalInstance()->waitForDone();
        QThreadPool::globalInstance()->setMaxThreadCount(m_saved);
    }

private:
    std::promise<void> m_gate;
    int m_saved;
    bool m_released = false;
};

// P4.15
void listBeingWritten(const QString& base) {
    const QString root = base + QStringLiteral("/written/steam");
    const QString list = root + QStringLiteral("/steamapps/libraryfolders.vdf");
    if (!writeSteamRoot(root, {root}).isEmpty() || !writeItem(root, QStringLiteral("1001"), videoItem(QStringLiteral("Rain"))).isEmpty()) {
        harnessFailure("listBeingWritten: the fake Steam could not be written");
        return;
    }
    auto m = manager(base + QStringLiteral("/written/wallpapers"), {root});
    QStringList seen;
    QObject::connect(m.get(), &LibraryManager::stateChanged, [&] { seen.append(m->problems().join(u'|')); });
    // GOAL: P4.15 -- a list a scan reads half-written, and that is written whole before the scan's result
    //       is published, is not reported as a problem: the scan after it reads it whole.
    // WHY-THIS-SHAPE: the manager's first scan is the held one -- a rescan() on a manager that has scanned
    //       may be absorbed by the follow-up scan every start runs, which read the list whole. The list is
    //       cut off after rescan() began, then the pool is drained with no event loop run, so the worker has
    //       provably read the cut-off list and its result waits, unpublished, while the list is written
    //       whole. The marker item can only be read by a later scan, so its row shows that the scan which
    //       read the cut-off list has been published.
    // CONTROL: app/library/library_manager.cpp, 2026-11-11, control_batch, reverted: publish()'s `if
    //       (!held.contains(libraryListOf(root)))` made `if (true)` -> red, "P4.15: a list caught being
    //       written was reported: [...may be missing...]".
    const QString marker = steamItemFolder(root) + QStringLiteral("/1002");
    {
        PoolHold hold;
        m->rescan();
        if (!writeBytes(list, "\"libraryfolders\" { \"0\" {")) {
            harnessFailure("listBeingWritten: the list could not be cut off");
            return;
        }
    }
    if (!writeSteamRoot(root, {root}).isEmpty() || !writeItem(root, QStringLiteral("1002"), sceneItem(QStringLiteral("Marker"))).isEmpty()) {
        harnessFailure("listBeingWritten: the list could not be written whole, or the marker written");
        return;
    }
    const bool after = listsWith(*m, [&](const QList<Wallpaper>& l) { return row(l, marker) != nullptr; });
    pump(100);
    expect(after && m->problems().isEmpty() &&
               std::ranges::none_of(seen, [](const QString& p) { return p.contains(QStringLiteral("may be missing")); }),
           QStringLiteral("P4.15: a list caught being written was reported: [%1]; now [%2]; marker listed %3")
               .arg(seen.join(u" / "), m->problems().join(u'|')).arg(after));
}

// P4.9
void probeReuse(const QString& base) {
    const QString root = base + QStringLiteral("/reuse/steam");
    const QString item = steamItemFolder(root) + QStringLiteral("/1001");
    const QString asset = item + QStringLiteral("/clip [1] it's.flv");
    if (!writeSteamRoot(root, {root}).isEmpty() || !writeItem(root, QStringLiteral("1001"), videoItem(QStringLiteral("Rain"))).isEmpty()) {
        harnessFailure("probeReuse: the fake Steam could not be written");
        return;
    }
    // A quiet period no test outlasts, so only rescan() starts a scan.
    LibraryManager m(base + QStringLiteral("/reuse/wallpapers"), 600000, 200, LibraryManager::Watching::Inotify, 2000, {root});
    const auto audio = [&] {
        const Wallpaper* w = row(m.wallpapers(), item);
        return w && w->usable() ? int(w->audio) : -1;
    };
    m.rescan();
    if (!listsWith(m, [](const QList<Wallpaper>& l) { return l.size() == 1; }) || audio() != int(AudioStreams::Present)) {
        notVerified("P4.9 (a rescan reuses a Workshop video's sound answer)", "the first scan did not list the video with sound");
        return;
    }
    const QByteArray path = QFile::encodeName(asset);
    struct stat before {};
    const int fd = ::stat(path.constData(), &before) == 0 ? ::open(path.constData(), O_WRONLY | O_CLOEXEC) : -1;
    const QByteArray junk(qsizetype(before.st_size), 'x');
    const timespec times[2] = {{0, UTIME_OMIT}, before.st_mtim};
    const bool rewritten = fd >= 0 && ::write(fd, junk.constData(), size_t(junk.size())) == junk.size() && ::futimens(fd, times) == 0;
    if (fd >= 0) {
        ::close(fd);
    }
    if (!rewritten || !writeItem(root, QStringLiteral("1002"), sceneItem(QStringLiteral("Marker"))).isEmpty()) {
        harnessFailure("probeReuse: the asset could not be rewritten, or the marker item written");
        return;
    }
    // GOAL: P4.9 -- a Workshop video whose asset stamp is unchanged keeps the sound answer it was probed
    //       with on a rescan, as a .wpx video does.
    // WHY-THIS-SHAPE: the asset is no longer media with the same size and time, so only a reused answer
    //       reads Present; the marker item makes the rescan's result a change whose arrival is the barrier.
    // CONTROL: app/library/wallpaper_engine.cpp, 2026-11-11, control_batch, reverted: scanWorkshop()'s
    //       `knownAudio(previous)` made `knownAudio({})` -> red, "P4.9: after a rescan with the asset's stamp
    //       unchanged its audio reads 0". Re-run 2026-11-11 on P4-1D's scanWorkshop(), same line.
    m.rescan();
    const bool rescanned = listsWith(m, [](const QList<Wallpaper>& l) { return l.size() == 2; });
    expect(rescanned && audio() == int(AudioStreams::Present),
           QStringLiteral("P4.9: after a rescan with the asset's stamp unchanged its audio reads %1 (Present is %2; rescanned %3)")
               .arg(audio()).arg(int(AudioStreams::Present)).arg(rescanned));
}

// P4.10
void holdByPath(const QString& base) {
    const QString wpx = base + QStringLiteral("/hold/wallpapers");
    const QString root = base + QStringLiteral("/hold/steam");
    const QString item = steamItemFolder(root) + QStringLiteral("/7007");
    const QString folder = wpx + QStringLiteral("/7007");
    if (!writeFolder(folder, planEntries(kPlanManifest)).isEmpty() || !writeSteamRoot(root, {root}).isEmpty() ||
        !writeItem(root, QStringLiteral("7007"), videoItem(QStringLiteral("Rain"))).isEmpty()) {
        harnessFailure("holdByPath: the fake library and Steam could not be written");
        return;
    }
    auto m = manager(wpx, {root});
    m->rescan();
    if (!listsWith(*m, [](const QList<Wallpaper>& l) { return l.size() == 2; })) {
        expect(false, QStringLiteral("P4.10: the two 7007s never listed: [%1]").arg(rows(m->wallpapers())));
        return;
    }
    const Wallpaper shown = *row(m->wallpapers(), item);
    QFile writer(item + QStringLiteral("/clip [1] it's.flv"));
    QByteArray renamed(kPlanManifest);
    renamed.replace("Ocean Waves", "New Waves");
    if (!writer.open(QIODevice::WriteOnly | QIODevice::Append) || writer.write("x", 1) != 1 || !writer.flush() ||
        !writeBytes(folder + QStringLiteral("/wallpaper.json"), renamed)) {
        harnessFailure("holdByPath: the item could not be held open, or the .wpx folder retitled");
        return;
    }
    // GOAL: P4.10 -- an item being written in a Workshop folder is held back by its full path: a .wpx
    //       folder of the same name changed meanwhile is read anew, while the held item keeps its row.
    // WHY-THIS-SHAPE: both are named 7007, so a hold matched by name holds back the .wpx folder too and
    //       its new title never shows; the held row is compared whole to its old one, so an item that was
    //       never held (its stamp changes with the byte written) cannot pass this vacuously.
    // CONTROL: app/library/library_manager.cpp, 2026-11-11, control_batch, reverted: holdBack()'s two
    //       `held.contains(...filePath)` matched by folder name -> red, "P4.10: while Workshop item 7007 was
    //       being written ...".
    const bool retitled = listsWith(*m, [&](const QList<Wallpaper>& l) { const Wallpaper* w = row(l, folder); return w && w->title == u"New Waves"; });
    const Wallpaper* now = row(m->wallpapers(), item);
    expect(retitled && now && *now == shown,
           QStringLiteral("P4.10: while Workshop item 7007 was being written the rows are [%1]; the item kept its row %2")
               .arg(rows(m->wallpapers())).arg(now && *now == shown));
    writer.close();
    expect(listsWith(*m, [&](const QList<Wallpaper>& l) { const Wallpaper* w = row(l, item); return w && *w != shown; }),
           QStringLiteral("P4.10: the Workshop item was never read again once its file was closed"));
}

// ---- the P4-1C and P4-1D fixes -----------------------------------------------------------------

// Queued on the global pool behind what is queued now: says when it starts, then holds the pool's
// thread until released. With one thread, its start means every task queued before it has run.
class PoolGate {
public:
    PoolGate() {
        std::shared_future<void> open = m_open.get_future().share();
        QThreadPool::globalInstance()->start([open, reached = m_reached] { reached->set_value(); open.wait(); });
    }
    PoolGate(const PoolGate&) = delete;
    PoolGate& operator=(const PoolGate&) = delete;
    ~PoolGate() { release(); }
    bool reached(int ms) { return m_reachedFuture.wait_for(std::chrono::milliseconds(ms)) == std::future_status::ready; }
    void release() {
        if (!m_released) {
            m_released = true;
            m_open.set_value();
        }
    }

private:
    std::promise<void> m_open;
    std::shared_ptr<std::promise<void>> m_reached = std::make_shared<std::promise<void>>();
    std::future<void> m_reachedFuture = m_reached->get_future();
    bool m_released = false;
};

// Lets the follow-up scan every start runs finish and publish, so a phase begins from quiet.
void drainScans() {
    for (int i = 0; i < 2; ++i) {
        QThreadPool::globalInstance()->waitForDone();
        pump(150);
    }
}

// Counts reads of one file by anyone, through the test's own inotify descriptor. Reads, not opens:
// the watchers open a file on the GUI thread to see whether it is being written, and read nothing.
class ReadCounter {
public:
    explicit ReadCounter(const QString& path) : m_fd(::inotify_init1(IN_NONBLOCK | IN_CLOEXEC)) {
        // Opens too, though only reads are counted: the kernel merges a read into an identical one still
        // unread, so two scans' reads with no open between them would count as one.
        m_ok = m_fd >= 0 && ::inotify_add_watch(m_fd, QFile::encodeName(path).constData(), IN_ACCESS | IN_OPEN) >= 0;
    }
    ReadCounter(const ReadCounter&) = delete;
    ReadCounter& operator=(const ReadCounter&) = delete;
    ~ReadCounter() {
        if (m_fd >= 0) ::close(m_fd);
    }
    bool ok() const { return m_ok; }
    int count() {
        alignas(inotify_event) char buffer[4096];
        for (ssize_t n; m_fd >= 0 && (n = ::read(m_fd, buffer, sizeof buffer)) > 0;) {
            for (ssize_t at = 0; at < n;) {
                const auto* event = reinterpret_cast<const inotify_event*>(buffer + at);
                m_count += (event->mask & IN_ACCESS) != 0;
                at += ssize_t(sizeof(inotify_event) + event->len);
            }
        }
        return m_count;
    }

private:
    int m_fd;
    bool m_ok = false;
    int m_count = 0;
};

// The inode of every inotify watch this process holds, from /proc/self/fdinfo.
QSet<quint64> watchedInodes() {
    QSet<quint64> out;
    const QDir fds(QStringLiteral("/proc/self/fd"));
    for (const QString& name : fds.entryList(QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot)) {
        char target[64] = {};
        if (::readlink(QFile::encodeName(fds.filePath(name)).constData(), target, sizeof target - 1) <= 0 ||
            std::string_view(target) != "anon_inode:inotify") {
            continue;
        }
        std::ifstream info("/proc/self/fdinfo/" + name.toStdString());
        for (std::string line; std::getline(info, line);) {
            if (const auto at = line.find(" ino:"); line.starts_with("inotify ") && at != std::string::npos) {
                out.insert(std::stoull(line.substr(at + 5), nullptr, 16));
            }
        }
    }
    return out;
}

quint64 inodeOf(const QString& path) {
    struct stat st {};
    return ::stat(QFile::encodeName(path).constData(), &st) == 0 ? quint64(st.st_ino) : 0;
}

// P4.21, P4.22, P4.23: items of other kinds, previews left empty, a malformed setting.
void otherKinds(const QString& base) {
    const QString root = base + QStringLiteral("/kinds/steam");
    const QString folder = steamItemFolder(root);
    const QByteArray clip = soundVideo();
    const QByteArray preview = pngBytes(16, 9);
    const std::tuple<const char*, QByteArray, bool> items[] = {
        {"3001", R"({"type":"scene","title":"No file","preview":"preview.jpg"})", false},
        {"3002", R"({"type":"Scene","title":"Bad file","file":"../x.json","general":5})", false},
        {"3003", R"({"type":"application","file":"/abs.exe","general":{"properties":{"b":{"type":"bool","value":1}}}})", false},
        {"3004", R"({"type":"video","file":"clip.flv","preview":"","title":"Empty preview"})", true},
        {"3005", R"({"type":"video","file":"clip.flv","preview":null,"title":"Null preview"})", true},
        {"3006", R"({"type":"video","file":"clip.flv","title":"Bad setting","general":{"properties":{"b":{"type":"bool","value":1}}}})", true},
    };
    for (const auto& [id, json, video] : items) {
        Entries entries{{QStringLiteral("project.json"), json}, {QStringLiteral("preview.jpg"), preview}};
        if (video) entries.append({QStringLiteral("clip.flv"), clip});
        if (const QString why = writeItem(root, QString::fromLatin1(id), entries); !why.isEmpty()) {
            harnessFailure("otherKinds: " + why.toStdString());
            return;
        }
    }
    WarningLog log;
    const WorkshopScan scan = scanWorkshop({folder});
    const auto item = [&](const char* id) { return row(scan.items, folder + u'/' + QString::fromLatin1(id)); };
    const auto describe = [](const Wallpaper* w) {
        return w ? QStringLiteral("usable %1, problem \"%2\", preview \"%3\"").arg(w->usable()).arg(w->problem, w->previewSource().toString())
                 : QStringLiteral("not listed");
    };

    // GOAL: P4.22 -- a scene or an unknown kind with a missing or bad `file`, or a malformed `general`, is
    //       listed with its kind's sentence, never a project.json parse error.
    // CONTROL: shared/we_project.cpp, 2026-11-11, control_batch, each alone, reverted: `file` required of every
    //       kind -> red, "P4.22: item 3001 is [usable 0, problem "project.json does not name its file ..."]" (and
    //       3002, 3003); parseProject() refusing on any setting problem -> red, "P4.22: item 3002 is [usable 0,
    //       problem "project.json has a "general" that is not an object." ...]" (and 3003).
    const QString scene = QStringLiteral("Wallpaper Engine scenes are not supported yet.");
    const QString application = QStringLiteral("It is a Wallpaper Engine \"application\" wallpaper, which WallpaperX does not know.");
    for (const auto& [id, want] : {std::pair{"3001", scene}, std::pair{"3002", scene}, std::pair{"3003", application}}) {
        const Wallpaper* w = item(id);
        expect(w && !w->usable() && w->problem == want, QStringLiteral("P4.22: item %1 is [%2], not \"%3\"").arg(QString::fromLatin1(id), describe(w), want));
    }
    // GOAL: P4.21 -- a video whose preview is "" or null is usable with no preview.
    // CONTROL: shared/we_project.cpp, 2026-11-11, control_batch, reverted: `!preview->is_null() &&` deleted
    //       -> red, "P4.21: video 3005 is [usable 0, problem "project.json does not name its preview ..."]".
    for (const char* id : {"3004", "3005"}) {
        const Wallpaper* w = item(id);
        expect(w && w->usable() && w->previewName.isEmpty(), QStringLiteral("P4.21: video %1 is [%2]").arg(QString::fromLatin1(id), describe(w)));
    }
    // GOAL: P4.23 -- a video with a malformed setting is usable, and the setting is logged exactly once,
    //       with its sentence; the unknown kind's malformed setting is not logged.
    // WHY-THIS-SHAPE: the count is of every "ignored in" line in this scan, so a second line for the video
    //       or any line for the application fails alike.
    // CONTROL: app/library/wallpaper_engine.cpp, 2026-11-11, control_batch, each alone, reverted, each red with
    //       "P4.23: the video with a malformed setting is [...]; logged [...]": readWorkshopItem()'s qWarning doubled
    //       -> [usable 1 ...] two lines; deleted -> logged []; every non-video kind's setting problems logged too
    //       -> a line for 3003; shared/we_project.cpp parseProject() refusing on a setting problem -> [usable 0 ...].
    const Wallpaper* bad = item("3006");
    const QStringList logged = log.containing(u"wallpaperx: ignored in");
    expect(bad && bad->usable() && logged.size() == 1 && logged[0].contains(u"/3006: ") && logged[0].contains(u"true or false"),
           QStringLiteral("P4.23: the video with a malformed setting is [%1]; logged [%2]").arg(describe(bad), logged.join(u" | ")));
}

// P4.25
void heldInUnlistedFolder(const QString& base) {
    const QString root = base + QStringLiteral("/heldunlisted/steam");
    const QString second = base + QStringLiteral("/heldunlisted/games");
    const QString rootItem = steamItemFolder(root) + QStringLiteral("/1001");
    const QString item = steamItemFolder(second) + QStringLiteral("/7007");
    if (!writeSteamRoot(root, {root, second}).isEmpty() || !writeItem(root, QStringLiteral("1001"), videoItem(QStringLiteral("Rain"))).isEmpty() ||
        !writeItem(second, QStringLiteral("7007"), videoItem(QStringLiteral("Snow"))).isEmpty() ||
        !writeFolder(steamInstallFolder(second) + QStringLiteral("/assets"), {}).isEmpty()) {
        harnessFailure("heldInUnlistedFolder: the fake Steam could not be written");
        return;
    }
    auto m = manager(base + QStringLiteral("/heldunlisted/wallpapers"), {root});
    m->rescan();
    if (!listsWith(*m, [](const QList<Wallpaper>& l) { return l.size() == 2; }) || m->wallpaperEngineInstall().isEmpty()) {
        expect(false, QStringLiteral("P4.25: the two libraries and the install never listed: [%1]").arg(rows(m->wallpapers())));
        return;
    }
    drainScans();
    // Held as P4.10 holds an item: a byte appended to its asset, the file left open.
    QFile writer(item + QStringLiteral("/clip [1] it's.flv"));
    if (!writer.open(QIODevice::WriteOnly | QIODevice::Append) || writer.write("x", 1) != 1 || !writer.flush() ||
        !writeSteamRoot(root, {root}).isEmpty()) {
        harnessFailure("heldInUnlistedFolder: the item could not be held open, or the list rewritten");
        return;
    }
    // GOAL: P4.25 -- a held-back row is kept only in a folder the scan listed: an item held (a file open for
    //       writing in it) in a library just taken off the list goes in the scan that drops the library.
    // WHY-THIS-SHAPE: graded on the publication that drops the library, seen as the install (only in that
    //       library) going in the same publish(); a wait for the row to go would also pass once the hold
    //       lapses (2 s here). Folders made unreadable cannot reach this rule: the watcher reports no hold
    //       in a folder it cannot read, so all three breaks of the rule stayed green in that shape.
    // CONTROL: app/library/library_manager.cpp, 2026-11-11, control_batch, reverted: publish()'s `held.removeIf(...
    //       !listed.contains(folderOf(path)))` deleted -> red, "P4.25: when the held item's library left the list
    //       (dropped 1) the rows are [Rain[WE],Snow[WE]]".
    m->rescan();
    const bool dropped = waitUntil([&] { return m->wallpaperEngineInstall().isEmpty(); }, kWaitMs);
    expect(dropped && !row(m->wallpapers(), item) && row(m->wallpapers(), rootItem),
           QStringLiteral("P4.25: when the held item's library left the list (dropped %1) the rows are [%2]")
               .arg(dropped).arg(rows(m->wallpapers())));
}

// P4.26
void listWrittenParses(const QString& base) {
    const QString root = base + QStringLiteral("/parses/steam");
    const QString second = base + QStringLiteral("/parses/games");
    const QString rootItem = steamItemFolder(root) + QStringLiteral("/1001");
    const QString secondItem = steamItemFolder(second) + QStringLiteral("/2001");
    if (!writeSteamRoot(root, {root, second}).isEmpty() || !writeItem(root, QStringLiteral("1001"), videoItem(QStringLiteral("Rain"))).isEmpty() ||
        !writeItem(second, QStringLiteral("2001"), videoItem(QStringLiteral("Snow"))).isEmpty()) {
        harnessFailure("listWrittenParses: the fake Steam could not be written");
        return;
    }
    auto m = manager(base + QStringLiteral("/parses/wallpapers"), {root});
    m->rescan();
    if (!listsWith(*m, [](const QList<Wallpaper>& l) { return l.size() == 2; })) {
        expect(false, QStringLiteral("P4.26: the two libraries never listed: [%1]").arg(rows(m->wallpapers())));
        return;
    }
    drainScans();
    // GOAL: P4.26 -- a list that parses but was written while the scan read it is not trusted: that scan's
    //       publication keeps the libraries the list last listed; the next scan trusts the new list.
    // WHY-THIS-SHAPE: the scan is held on the pool until the list (now naming only the root) is written, so
    //       the worker reads the new list and its watcher saw the write; a gate behind the scan stops any later
    //       scan, so only that scan's publication is graded. The library path is made a file in the same
    //       window: its problem shows this scan was the one published (a retitled item cannot show it, since
    //       a watched item just written is held back).
    // CONTROL: app/library/library_manager.cpp, 2026-11-11, control_batch, each alone, reverted: publish()'s
    //       `if (!held.contains(libraryListOf(root)))` made `if (true)` -> red, "P4.26: the scan that read the list
    //       while it was written dropped the library it last listed: [Rain[WE]]"; a list trusted only on its first
    //       read (`&& !m_steamLibraries.contains(root)`) -> red, "P4.26: the next scan never trusted the new list".
    const QString wpx = base + QStringLiteral("/parses/wallpapers");
    QString during;
    {
        PoolHold hold;
        m->rescan();
        if (!writeSteamRoot(root, {root}).isEmpty() || !writeBytes(wpx, "a file")) {
            harnessFailure("listWrittenParses: the list could not be rewritten, or the library path made a file");
            return;
        }
        PoolGate after;
        hold.release();
        if (!after.reached(kWaitMs)) {
            expect(false, QStringLiteral("P4.26: the held scan never finished"));
            return;
        }
        QCoreApplication::sendPostedEvents();
        if (m->problems() != QStringList{wpx + QStringLiteral(" is not a folder.")}) {
            expect(false, QStringLiteral("P4.26: the held scan was not the one published: problems [%1]").arg(m->problems().join(u'|')));
            return;
        }
        if (!row(m->wallpapers(), secondItem)) {
            during = rows(m->wallpapers());
        }
    }
    expect(during.isEmpty(), QStringLiteral("P4.26: the scan that read the list while it was written dropped the library it last listed: [%1]").arg(during));
    if (!QFile::remove(wpx)) {
        harnessFailure("listWrittenParses: the library path could not be put back");
        return;
    }
    m->rescan();
    // Rows and problems together: a scan from before the path was put back may already drop the library.
    const bool trusted = waitUntil([&] {
        return m->state() == LibraryManager::State::Ready && m->problems().isEmpty() && !row(m->wallpapers(), secondItem) && row(m->wallpapers(), rootItem);
    }, kWaitMs);
    expect(trusted, QStringLiteral("P4.26: the next scan never trusted the new list: [%1], problems [%2]").arg(rows(m->wallpapers()), m->problems().join(u'|')));
}

// P4.28
void listOffGuiThread(const QString& base) {
    const QString root = base + QStringLiteral("/offgui/steam");
    if (!writeSteamRoot(root, {root}).isEmpty() || !writeItem(root, QStringLiteral("1001"), videoItem(QStringLiteral("Rain"))).isEmpty()) {
        harnessFailure("listOffGuiThread: the fake Steam could not be written");
        return;
    }
    ReadCounter opens(libraryListOf(root));
    if (!opens.ok()) {
        harnessFailure("listOffGuiThread: the list could not be watched");
        return;
    }
    // GOAL: P4.28 -- no libraryfolders.vdf is read on the GUI thread: while the scan worker is held,
    //       constructing the manager and calling rescan() read no list.
    // WHY-THIS-SHAPE: a FIFO list cannot show a stall, since the reader refuses a file that is not regular;
    //       counting reads (IN_ACCESS) of the real list while the pool is held separates the threads. Opens are
    //       not counted: the list watcher opens it on the GUI thread to test for a writer, reading nothing.
    //       The count after the worker ran must be above zero, so a counter that sees nothing cannot pass.
    // CONTROL: app/library/library_manager.cpp, 2026-11-11, control_batch, reverted: rescan() calling
    //       steamLibrariesOf(root) for each root -> red, "P4.28: the list was read 1 time(s) on the GUI thread
    //       before the scan ran, 2 in all; listed 1".
    std::unique_ptr<LibraryManager> m;
    int onGui = -1;
    {
        PoolHold hold;
        m = manager(base + QStringLiteral("/offgui/wallpapers"), {root});
        m->rescan();
        onGui = opens.count();
    }
    const bool listed = listsWith(*m, [](const QList<Wallpaper>& l) { return l.size() == 1; });
    expect(onGui == 0 && listed && opens.count() > 0,
           QStringLiteral("P4.28: the list was read %1 time(s) on the GUI thread before the scan ran, %2 in all; listed %3")
               .arg(onGui).arg(opens.count()).arg(listed));
}

// P4.29
void shallowWatch(const QString& base) {
    const QString root = base + QStringLiteral("/shallow/steam");
    const QString item = steamItemFolder(root) + QStringLiteral("/1001");
    const QString sub = item + QStringLiteral("/assets");
    if (!writeSteamRoot(root, {root}).isEmpty() || !writeItem(root, QStringLiteral("1001"), videoItem(QStringLiteral("Rain"))).isEmpty() ||
        !writeFolder(sub, {{QStringLiteral("a.txt"), QByteArray("one")}}).isEmpty()) {
        harnessFailure("shallowWatch: the fake Steam could not be written");
        return;
    }
    {
        auto m = manager(base + QStringLiteral("/shallow/wallpapers"), {root});
        m->rescan();
        if (!listsWith(*m, [](const QList<Wallpaper>& l) { return l.size() == 1; })) {
            expect(false, QStringLiteral("P4.29: the item never listed: [%1]").arg(rows(m->wallpapers())));
            return;
        }
        drainScans();
        // GOAL: P4.29 -- an item folder gets a watch and its subfolders get none; a change directly in it
        //       is seen with no rescan asked for.
        // WHY-THIS-SHAPE: the watches are read from the kernel (/proc/self/fdinfo), so how the watcher keeps
        //       them does not matter; the item folder's own watch must be found, so a reader that finds no
        //       watch at all cannot pass the "subfolder not watched" half.
        // CONTROL: app/library/folder_watcher.cpp, 2026-11-11, control_batch, each alone, reverted: followsInto()'s
        //       ShallowFolders `return relative.isEmpty();` made `true` -> red, "P4.29: the item folder is watched 1
        //       and its subfolder 1 (5 watches in all)"; made `false` -> red, "P4.29: a change directly in a
        //       Workshop item folder was never seen" (and "watched 0 and its subfolder 0").
        const QSet<quint64> watched = watchedInodes();
        expect(watched.contains(inodeOf(item)) && !watched.contains(inodeOf(sub)),
               QStringLiteral("P4.29: the item folder is watched %1 and its subfolder %2 (%3 watches in all)")
                   .arg(watched.contains(inodeOf(item))).arg(watched.contains(inodeOf(sub))).arg(watched.size()));
        if (!writeBytes(item + QStringLiteral("/project.json"), projectBytes("video", QStringLiteral("Retitled"), QStringLiteral("clip [1] it's.flv")))) {
            harnessFailure("shallowWatch: project.json could not be rewritten");
            return;
        }
        expect(listsWith(*m, [&](const QList<Wallpaper>& l) { const Wallpaper* w = row(l, item); return w && w->title == u"Retitled"; }),
               QStringLiteral("P4.29: a change directly in a Workshop item folder was never seen: [%1]").arg(rows(m->wallpapers())));
    }
    drainScans();
    LibraryManager m(base + QStringLiteral("/shallow/poll-wallpapers"), kSettleMs, 200, LibraryManager::Watching::Poll, 2000, {root});
    m.rescan();
    if (!listsWith(m, [](const QList<Wallpaper>& l) { return l.size() == 1; })) {
        expect(false, QStringLiteral("P4.29: with polling the item never listed: [%1]").arg(rows(m.wallpapers())));
        return;
    }
    drainScans();
    pump(500);
    ReadCounter reads(item + QStringLiteral("/project.json"));
    if (!reads.ok() || !writeBytes(sub + QStringLiteral("/a.txt"), "two, and longer")) {
        harnessFailure("shallowWatch: the item could not be watched or its subfolder written");
        return;
    }
    // GOAL: P4.29 -- with polling, a write inside an item's subfolder starts no scan; one at its top does.
    // WHY-THIS-SHAPE: a scan is seen as a read of the item's project.json (every scan reads it); the
    //       window is six poll periods, an absence that can only be graded over time, so the write at the top
    //       must then be seen through the same counter, or the counter proves nothing.
    // CONTROL: app/library/folder_watcher.cpp, 2026-11-11, control_batch, reverted: followsInto()'s ShallowFolders
    //       case made `true` -> red, "P4.29: with polling a write inside an item's subfolder brought 1 scan read(s)".
    pump(1200);
    const int afterSub = reads.count();
    if (!writeBytes(item + QStringLiteral("/note.txt"), "x")) {
        harnessFailure("shallowWatch: a file could not be written at the item's top");
        return;
    }
    const bool seen = waitUntil([&] { return reads.count() > 0; }, kWaitMs);
    expect(afterSub == 0 && seen,
           QStringLiteral("P4.29: with polling a write inside an item's subfolder brought %1 scan read(s); a write at its top was seen %2")
               .arg(afterSub).arg(seen));
}

// P4.30
void emptyWithSteamProblem(const QString& base) {
    const QString wpx = base + QStringLiteral("/emptysteam/wallpapers");
    const QString root = base + QStringLiteral("/emptysteam/steam");
    if (!QDir().mkpath(wpx) || !QDir().mkpath(steamAppsFolder(root)) || !writeBytes(libraryListOf(root), "\"libraryfolders\" {")) {
        harnessFailure("emptyWithSteamProblem: the empty library and the cut-off list could not be written");
        return;
    }
    // GOAL: P4.30 -- an empty library whose only problem is a Steam one is Ready, its reason empty.
    // CONTROL: app/library/library_manager.cpp, 2026-11-11, control_batch, reverted: state() Ready only with no
    //       Steam problems either (the rule before P4-1C) -> red, "P4.30: an empty library with a cut-off list is
    //       state 2, reason "Some Wallpaper Engine items may be missing. ..."".
    auto m = manager(wpx, {root});
    m->rescan();
    const bool read = waitUntil([&] { return m->state() != LibraryManager::State::Loading; }, kWaitMs);
    const QStringList problems = m->problems();
    expect(read && m->state() == LibraryManager::State::Ready && m->wallpapers().isEmpty() && m->unavailableReason().isEmpty() &&
               problems.size() == 1 && problems[0].startsWith(u"Some Wallpaper Engine items may be missing. "),
           QStringLiteral("P4.30: an empty library with a cut-off list is state %1, reason \"%2\", problems [%3]")
               .arg(int(m->state())).arg(m->unavailableReason(), problems.join(u'|')));
}

// P4.31
void followUpScan(const QString& base) {
    const QString root = base + QStringLiteral("/followup/steam");
    const QString item = steamItemFolder(root) + QStringLiteral("/1001");
    if (!writeSteamRoot(root, {root}).isEmpty() || !writeItem(root, QStringLiteral("1001"), videoItem(QStringLiteral("Rain"))).isEmpty()) {
        harnessFailure("followUpScan: the fake Steam could not be written");
        return;
    }
    // A quiet period no test outlasts, so no watcher starts a scan.
    LibraryManager m(base + QStringLiteral("/followup/wallpapers"), 600000, 200, LibraryManager::Watching::Inotify, 2000, {root});
    // GOAL: P4.31 -- a Workshop folder read before it had a watcher is read again by a follow-up scan,
    //       with no change notice and no rescan asked for.
    // WHY-THIS-SHAPE: the item is retitled after the first scan read it and before that scan is published,
    //       while nothing watches it, and the watchers' quiet period outlasts the test, so only the
    //       follow-up can show the new title; the old title after the first publication is the barrier.
    // CONTROL: app/library/library_manager.cpp, 2026-11-11, control_batch, reverted: publish()'s `if (unwatched)`
    //       made `if (false && unwatched)` -> red, "P4.31: a Workshop folder read before it was watched was never
    //       read again: [Rain[WE]]".
    QString first;
    {
        PoolHold hold;
        m.rescan();
        PoolGate after;
        hold.release();
        if (!after.reached(kWaitMs)) {
            expect(false, QStringLiteral("P4.31: the first scan never finished"));
            return;
        }
        if (!writeBytes(item + QStringLiteral("/project.json"), projectBytes("video", QStringLiteral("Rain again"), QStringLiteral("clip [1] it's.flv")))) {
            harnessFailure("followUpScan: project.json could not be rewritten");
            return;
        }
        QCoreApplication::sendPostedEvents();
        first = rows(m.wallpapers());
    }
    if (first != u"Rain[WE]") {
        expect(false, QStringLiteral("P4.31: the first scan published [%1], not the item as first read").arg(first));
        return;
    }
    expect(listsWith(m, [&](const QList<Wallpaper>& l) { const Wallpaper* w = row(l, item); return w && w->title == u"Rain again"; }),
           QStringLiteral("P4.31: a Workshop folder read before it was watched was never read again: [%1]").arg(rows(m.wallpapers())));
}

// P4.35
void settingLoggedOnce(const QString& base) {
    const QString root = base + QStringLiteral("/logonce/steam");
    const QString item = steamItemFolder(root) + QStringLiteral("/1001");
    const auto project = [](const char* title) {
        return QByteArray(R"({"type":"video","file":"clip.flv","title":")") + title +
               R"(","general":{"properties":{"b":{"type":"bool","value":1}}}})";
    };
    if (!writeSteamRoot(root, {root}).isEmpty() ||
        !writeItem(root, QStringLiteral("1001"), {{QStringLiteral("project.json"), project("Bad setting")}, {QStringLiteral("clip.flv"), soundVideo()}}).isEmpty()) {
        harnessFailure("settingLoggedOnce: the fake Steam could not be written");
        return;
    }
    WarningLog log;
    ReadCounter reads(item + QStringLiteral("/project.json"));
    if (!reads.ok()) {
        harnessFailure("settingLoggedOnce: project.json's reads could not be watched");
        return;
    }
    const auto logged = [&] { return log.containing(u"wallpaperx: ignored in").size(); };
    const auto titled = [&](const char* title) {
        return [title, &item](const QList<Wallpaper>& l) { const Wallpaper* w = row(l, item); return w && w->usable() && w->title == QLatin1StringView(title); };
    };
    auto m = manager(base + QStringLiteral("/logonce/wallpapers"), {root});
    // GOAL: P4.35 -- a Workshop video's malformed setting is logged once across the two start-up scans and a
    //       later rescan, and once more after its project.json changes.
    // WHY-THIS-SHAPE: every scan is run to its end before counting, so a line logged by a scan still in
    //       flight is not missed; one scan alone would log once even with the per-stamp rule gone.
    // COVERAGE-ASSERT: project.json's reads are counted, so the start-up must show two scans reading it and
    //       the rescan one more, or the "once" proves nothing.
    // CONTROL: app/library/wallpaper_engine.cpp, 2026-11-11, control_batch, each alone, reverted: scanWorkshop()'s
    //       `logged.insert(item.filePath, item.stamp);` deleted -> red, "P4.35: the start-up scans (2 read(s) of
    //       project.json) logged the setting 2 time(s)" and "P4.35: a later rescan ... logged it 3 time(s) in all";
    //       readWorkshopItem()'s `if (lastRead != stamp)` made `if (!lastRead)` -> red, "P4.35: after project.json
    //       changed the setting was logged 1 time(s) in all".
    m->rescan();
    const bool started = listsWith(*m, titled("Bad setting"));
    finishScans();
    const int startReads = reads.count();
    const qsizetype startLogs = logged();
    expect(started && startReads >= 2 && startLogs == 1,
           QStringLiteral("P4.35: the start-up scans (%1 read(s) of project.json) logged the setting %2 time(s); listed %3")
               .arg(startReads).arg(startLogs).arg(started));
    m->rescan();
    finishScans();
    const int laterReads = reads.count();
    const qsizetype laterLogs = logged();
    expect(laterReads > startReads && laterLogs == 1,
           QStringLiteral("P4.35: a later rescan (%1 read(s) of project.json, %2 before it) left it logged %3 time(s) in all")
               .arg(laterReads).arg(startReads).arg(laterLogs));
    if (!writeBytes(item + QStringLiteral("/project.json"), project("Changed setting"))) {
        harnessFailure("settingLoggedOnce: project.json could not be rewritten");
        return;
    }
    const bool changed = listsWith(*m, titled("Changed setting"));
    finishScans();
    expect(changed && logged() == 2,
           QStringLiteral("P4.35: after project.json changed the setting was logged %1 time(s) in all; re-read %2").arg(logged()).arg(changed));
}

// P4.36
void idleScans(const QString& base) {
    if (::geteuid() == 0) {
        notVerified("P4.36", "running as root, which can read a mode-000 folder");
        return;
    }
    struct Case {
        QString name;
        LibraryManager::Watching watching;
        std::unique_ptr<ReadCounter> reads;
        std::unique_ptr<LibraryManager> m;
        int before = 0;
    };
    ModesRestored restore;
    std::vector<Case> cases;
    for (const auto watching : {LibraryManager::Watching::Inotify, LibraryManager::Watching::Poll}) {
        const QString mode = watching == LibraryManager::Watching::Poll ? QStringLiteral("polling") : QStringLiteral("inotify");
        for (const char* name : {"wpx", "workshop", "item", "root", "steamapps", "list", "rescanned"}) {
            const QString top = base + QStringLiteral("/idle/%1-%2").arg(mode, QLatin1StringView(name));
            const QString wpx = top + QStringLiteral("/wallpapers");
            const QString good = top + QStringLiteral("/good-steam");
            const QString bad = top + QStringLiteral("/bad-steam");
            const QString lockedByName = QStringLiteral("wpx:%1|workshop:%2|item:%3|root:%4|steamapps:%5|list:%6|rescanned:%2")
                                             .arg(wpx, steamItemFolder(bad), steamItemFolder(bad) + QStringLiteral("/2001"), bad,
                                                  bad + QStringLiteral("/steamapps"), libraryListOf(bad));
            QString locked;
            for (const QString& pair : lockedByName.split(u'|')) {
                if (pair.startsWith(QLatin1StringView(name) + u':')) locked = pair.section(u':', 1);
            }
            if (!QDir().mkpath(wpx) || !writeItem(good, QStringLiteral("1001"), sceneItem(QStringLiteral("Counter"))).isEmpty() ||
                !writeItem(bad, QStringLiteral("2001"), sceneItem(QStringLiteral("Other"))).isEmpty() || !writeSteamRoot(bad, {bad}).isEmpty() ||
                locked.isEmpty()) {
                harnessFailure("idleScans: the fake Steam roots could not be written");
                return;
            }
            restore.paths.append(locked);
            if (!setMode(locked, 0)) {
                harnessFailure("idleScans: " + locked.toStdString() + " could not be made unreadable");
                return;
            }
            if (QFileInfo(locked).isDir()) {
                if (DIR* d = ::opendir(QFile::encodeName(locked).constData())) {
                    ::closedir(d);
                    notVerified("P4.36", "a mode-000 folder can be listed here");
                    return;
                }
            }
            Case c{mode + u' ' + QLatin1StringView(name), watching,
                   std::make_unique<ReadCounter>(steamItemFolder(good) + QStringLiteral("/1001/project.json")), nullptr};
            if (!c.reads->ok()) {
                harnessFailure("idleScans: the counter item's reads could not be watched");
                return;
            }
            // The app's own timings, so the polls are as many as the app makes.
            c.m = std::make_unique<LibraryManager>(wpx, LibraryManager::kSettleMs, LibraryManager::kPollMs, watching,
                                                   LibraryManager::kHoldMs, QStringList{good, bad});
            cases.push_back(std::move(c));
        }
    }
    for (Case& c : cases) c.m->rescan();
    const bool started = waitUntil([&] {
        return std::ranges::all_of(cases, [](const Case& c) {
            return c.m->state() == LibraryManager::State::Ready && std::ranges::any_of(c.m->wallpapers(), [](const Wallpaper& w) { return w.title == u"Counter"; });
        });
    }, kWaitMs * 2);
    finishScans();
    pump(LibraryManager::kSettleMs * 2);
    finishScans();
    for (Case& c : cases) {
        c.before = c.reads->count();
        expect(started && c.before >= 1, QStringLiteral("P4.36: %1: the start-up never listed the counter item (%2 read(s)), state %3, problems [%4]")
                                             .arg(c.name).arg(c.before).arg(int(c.m->state())).arg(c.m->problems().join(u'|')));
    }
    if (!started) return;
    // GOAL: P4.36 -- with the .wpx folder, a Workshop folder, a Workshop item, a Steam root, its steamapps
    //       folder or its libraryfolders.vdf unreadable, no scan runs in 10 s of idle, with inotify and with
    //       polling; a rescan made in the same window is counted.
    // WHY-THIS-SHAPE: a scan is seen as a read of the counter item's project.json (a scan reads every item),
    //       not as a signal, since a scan that finds nothing new emits nothing. All fourteen run at once,
    //       so the window is 10 s once; five polls fit in it. The "rescanned" case proves the counter counts.
    // CONTROL: 2026-11-11, control_batch, each alone, reverted: app/library/library_manager.cpp publish()'s
    //       `unwatched` also true whenever the scan has a problem -> red, "P4.36: inotify workshop: 8192 scan
    //       read(s) in 10 s of idle" (every case but "item", both modes); `unwatched` also true when a row could
    //       not be opened -> red, "P4.36: inotify item: ..." and "polling item: ..." only;
    //       app/library/folder_watcher.cpp poll() also due whenever the folder cannot be opened -> red,
    //       "P4.36: polling wpx: 4 scan read(s) ..." (and polling workshop, root, steamapps); the test's
    //       mid-window rescan() deleted -> red, "P4.36: inotify rescanned: a rescan in the idle window was not counted".
    pump(5000);
    for (Case& c : cases) {
        if (c.name.endsWith(u"rescanned")) c.m->rescan();
    }
    pump(5000);
    finishScans();
    for (Case& c : cases) {
        const int idle = c.reads->count() - c.before;
        if (c.name.endsWith(u"rescanned")) {
            expect(idle >= 1, QStringLiteral("P4.36: %1: a rescan in the idle window was not counted (%2 read(s))").arg(c.name).arg(idle));
        } else {
            expect(idle == 0, QStringLiteral("P4.36: %1: %2 scan read(s) in 10 s of idle, problems [%3]").arg(c.name).arg(idle).arg(c.m->problems().join(u'|')));
        }
    }
    cases.clear();
}

} // namespace

int main(int argc, char* argv[]) {
    wallpaperx::testing::g_testName = "wallpaper_engine";
    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        notVerified("Wallpaper Engine items", "no temporary folder could be made");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    isolateXdg(scratch.path());
    QCoreApplication app(argc, argv);

    const QString root = scratch.path();
    runPhase("libraryList", libraryList);
    runPhase("projectFile", projectFile);
    runPhase("steamRoots", [&] { steamRoots(root); });
    runPhase("listing", [&] { listing(root); });
    runPhase("readOnly", [&] { readOnly(root); });
    runPhase("writesNothing", [&] { writesNothing(root); });
    runPhase("steamChanges", [&] { steamChanges(root); });
    runPhase("steamArrives", [&] { steamArrives(root); });
    runPhase("sourceFails", [&] { sourceFails(root); });
    runPhase("problemOrder", [&] { problemOrder(root); });
    runPhase("listBeingWritten", [&] { listBeingWritten(root); });
    runPhase("probeReuse", [&] { probeReuse(root); });
    runPhase("holdByPath", [&] { holdByPath(root); });
    runPhase("otherKinds", [&] { otherKinds(root); });
    runPhase("heldInUnlistedFolder", [&] { heldInUnlistedFolder(root); });
    runPhase("listWrittenParses", [&] { listWrittenParses(root); });
    runPhase("listOffGuiThread", [&] { listOffGuiThread(root); });
    runPhase("shallowWatch", [&] { shallowWatch(root); });
    runPhase("emptyWithSteamProblem", [&] { emptyWithSteamProblem(root); });
    runPhase("followUpScan", [&] { followUpScan(root); });
    runPhase("settingLoggedOnce", [&] { settingLoggedOnce(root); });
    runPhase("idleScans", [&] { idleScans(root); });
    return verdict();
}
