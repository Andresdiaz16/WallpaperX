// SPDX-License-Identifier: GPL-2.0-or-later

// app/library/wpx_package.cpp and app/library/preview_image_provider.cpp, with no bus and no
// event loop: one wallpaper.json, one .wpx archive, one wallpaper folder, one library folder, one
// preview address at a time. Goals S15.31-S15.35, S15.45, S15.47, S15.48, S15.50, S15.51, S15.53,
// S15.55, S15.67, S15.68, S15.221-S15.225 in GOALS.md; the last graded against the daemon's own
// property rules (daemon_rules.cpp).

#include "daemon_rules.h"
#include "openat2_shim.h"
#include "preview_image_provider.h"
#include "wpx_fixtures.h"
#include "wpx_package.h"

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QImageReader>
#include <QRandomGenerator>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QUrl>

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <future>
#include <limits>
#include <thread>

#include <csignal>

#include <fcntl.h>
#include <linux/openat2.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace wallpaperx;
using namespace wallpaperx::apptest;
using wallpaperx::testing::check;
using wallpaperx::testing::harnessFailure;
using wallpaperx::testing::notVerified;

namespace {

constexpr int kFifoTimeoutMs = 3000;

QString keysOf(const QList<WallpaperProperty>& properties) {
    QStringList keys;
    for (const WallpaperProperty& p : properties) {
        keys.append(p.key);
    }
    return keys.join(u',');
}

QByteArray edited(const std::function<void(OrderedJson&)>& change) {
    OrderedJson json = planManifest();
    change(json);
    return jsonBytes(json);
}

// runBounded (wpx_fixtures.h) catches this file's throws too.
// CONTROL: app/library/wpx_package.cpp, parseManifest's `Json::parse_error` and `Json::exception`
//       catch clauses deleted, so badjson's manifest throws out of the scan -> red, "FAIL: S15.34:
//       scanning a library holding a FIFO threw: [json.exception.parse_error.101] ...", beside
//       runPhase's "an exception escaped malformedManifests ..."; ctest graded it Failed, not aborted.

bool isBrokenOnly(const Wallpaper& w) {
    return !w.usable() && !w.problem.isEmpty() && w.type.isEmpty() && w.title.isEmpty() && w.author.isEmpty() &&
           w.tags.isEmpty() && w.assetName.isEmpty() && w.previewName.isEmpty() && w.properties.isEmpty() &&
           w.previewSource().isEmpty() && w.assetPath.isEmpty();
}

// readWallpaper on the archive at `path`, opened as the importer opens one. A fixture that cannot
// be opened is a harness fault, and reads as no wallpaper.
Wallpaper readArchive(const QString& path) {
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        harnessFailure("S15.33: " + path.toStdString() + " could not be opened: " + std::strerror(errno));
        return {};
    }
    Wallpaper read = readWallpaper(fd, path);
    ::close(fd);
    return read;
}

bool makeSymlink(const QString& target, const QString& link) {
    return ::symlink(QFile::encodeName(target).constData(), QFile::encodeName(link).constData()) == 0;
}

// Rewrites `path` in place with the same bytes and sets its modification time `plusNs` later
// before closing it. Whether the inode and size stayed and the time moved.
bool rewriteLater(const QString& path, qint64 plusNs) {
    const auto bytes = readBytes(path);
    struct stat before {};
    if (!bytes || ::stat(QFile::encodeName(path).constData(), &before) != 0) {
        return false;
    }
    const int fd = ::open(QFile::encodeName(path).constData(), O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    const qint64 when = qint64(before.st_mtim.tv_sec) * 1'000'000'000 + before.st_mtim.tv_nsec + plusNs;
    const timespec times[2] = {{0, UTIME_OMIT}, {when / 1'000'000'000, when % 1'000'000'000}};
    struct stat after {};
    const bool done = ::write(fd, bytes->constData(), size_t(bytes->size())) == bytes->size() && ::futimens(fd, times) == 0 &&
                      ::fstat(fd, &after) == 0;
    return ::close(fd) == 0 && done && after.st_ino == before.st_ino && after.st_size == before.st_size &&
           (after.st_mtim.tv_sec != before.st_mtim.tv_sec || after.st_mtim.tv_nsec != before.st_mtim.tv_nsec);
}

// S15.31
void manifestOrderAndFields() {
    const auto plan = parseManifest(QByteArray(kPlanManifest));
    if (!plan) {
        expect(false, QStringLiteral("S15.31: PLAN.md's example manifest was refused: %1").arg(plan.error()));
        return;
    }
    // GOAL: S15.31 -- properties come out in the order wallpaper.json lists them, never sorted.
    // WHY-THIS-SHAPE: PLAN's keys are speed, mute, tint (alphabetical would be mute, speed, tint) and
    //       the second manifest's are zoom, glow, alpha, so a sort in either direction goes red;
    //       a manifest already in alphabetical order would pass a sorting implementation.
    // CONTROL: app/library/wpx_package.cpp, `using Json = nlohmann::ordered_json;` made
    //       `nlohmann::json` (a sorted map) -> red, "S15.31: properties came out as mute,speed,tint";
    //       restored, green.
    expect(keysOf(plan->properties) == QStringLiteral("speed,mute,tint"),
           QStringLiteral("S15.31: properties came out as %1, not speed,mute,tint").arg(keysOf(plan->properties)));
    const auto reversed = parseManifest(edited([](OrderedJson& j) {
        j["properties"] = OrderedJson::parse(R"({"zoom": {"type": "float", "default": 1, "min": 0, "max": 2},
                                                "glow": {"type": "bool", "default": false},
                                                "alpha": {"type": "color", "default": "#000000"}})");
    }));
    expect(reversed && keysOf(reversed->properties) == QStringLiteral("zoom,glow,alpha"),
           QStringLiteral("S15.31: properties came out as %1, not zoom,glow,alpha")
               .arg(reversed ? keysOf(reversed->properties) : reversed.error()));

    expect(plan->type == u"video" && plan->title == u"Ocean Waves" && plan->author == u"someone" &&
               plan->tags == QStringList{QStringLiteral("nature"), QStringLiteral("relaxing")} &&
               plan->assetName == u"assets/video.mp4" && plan->previewName == u"preview.png",
           QStringLiteral("S15.31: PLAN's manifest read as type %1, title %2, author %3, tags %4, file %5, preview %6")
               .arg(plan->type, plan->title, plan->author, plan->tags.join(u','), plan->assetName, plan->previewName));
    if (plan->properties.size() == 3) {
        const WallpaperProperty& speed = plan->properties[0];
        const WallpaperProperty& mute = plan->properties[1];
        const WallpaperProperty& tint = plan->properties[2];
        expect(speed.type == WallpaperProperty::Type::Float && speed.minimum == 0.1 && speed.maximum == 2.0 &&
                   speed.defaultValue == QVariant(1.0) && speed.label == u"Speed",
               QStringLiteral("S15.31: speed read as %1 [%2, %3] default %4 label %5")
                   .arg(speed.typeName())
                   .arg(speed.minimum)
                   .arg(speed.maximum)
                   .arg(speed.defaultValue.toString(), speed.label));
        expect(mute.type == WallpaperProperty::Type::Bool && mute.defaultValue == QVariant(true),
               QStringLiteral("S15.31: mute read as %1 default %2").arg(mute.typeName(), mute.defaultValue.toString()));
        // GOAL: S15.31 -- a colour default is stored lower-case, so "#FFFFFF" and "#ffffff" are one value.
        // CONTROL: app/library/wpx_package.cpp, accept()'s `QVariant(text.toLower())` made
        //       `QVariant(text)` -> red, "S15.31: tint read as color default #FFFFFF"; restored, green.
        expect(tint.type == WallpaperProperty::Type::Color && tint.defaultValue == QVariant(QStringLiteral("#ffffff")),
               QStringLiteral("S15.31: tint read as %1 default %2").arg(tint.typeName(), tint.defaultValue.toString()));
    }

    // GOAL: S15.31 -- schema_version 1 written as a JSON integer is read like "1".
    // CONTROL: app/library/wpx_package.cpp, the `|| (version->is_number_integer() && ...)` clause
    //       deleted -> red, "S15.31: schema_version 1 as an integer was refused"; restored, green.
    const auto integer = parseManifest(edited([](OrderedJson& j) { j["schema_version"] = 1; }));
    expect(integer.has_value(), QStringLiteral("S15.31: schema_version 1 as an integer was refused: %1")
                                    .arg(integer ? QString() : integer.error()));
}

// S15.32, S15.48
void malformedManifests() {
    struct Case {
        const char* what;
        QByteArray json;
        const char* reason;
    };
    QByteArray badUtf8(kPlanManifest);
    badUtf8.replace("Ocean Waves", "Ocean \xff Waves");
    QByteArray overflow(kPlanManifest);
    overflow.replace("\"default\": 1.0", "\"default\": 1e999");
    const auto property = [](const char* key, const char* spec) {
        return edited([key, spec](OrderedJson& j) { j["properties"][key] = OrderedJson::parse(spec); });
    };
    const Case cases[] = {
        {"a JSON array", "[1, 2]", "is not a JSON object"},
        {"invalid JSON", "{ nope", "is not valid JSON"},
        {"invalid UTF-8 in a string", badUtf8, "is not valid JSON"},
        {"a number too large for a double", overflow, "wallpaper.json holds a number too large to read."},
        {"no schema_version", edited([](OrderedJson& j) { j.erase("schema_version"); }), "has no schema_version"},
        {"schema_version \"2\"", edited([](OrderedJson& j) { j["schema_version"] = "2"; }), "schema version \"2\""},
        {"schema_version 2.0", edited([](OrderedJson& j) { j["schema_version"] = 2.0; }), "schema version 2.0"},
        {"type gif", edited([](OrderedJson& j) { j["type"] = "gif"; }), "needs a type of static, video or shader"},
        {"type Video", edited([](OrderedJson& j) { j["type"] = "Video"; }), "needs a type of static, video or shader"},
        {"no type", edited([](OrderedJson& j) { j.erase("type"); }), "needs a type of static, video or shader"},
        {"empty title", edited([](OrderedJson& j) { j["title"] = ""; }), "has no title"},
        {"no title", edited([](OrderedJson& j) { j.erase("title"); }), "has no title"},
        {"numeric title", edited([](OrderedJson& j) { j["title"] = 5; }), "has no title"},
        {"numeric author", edited([](OrderedJson& j) { j["author"] = 5; }), "author that is not text"},
        {"no preview", edited([](OrderedJson& j) { j.erase("preview"); }), "has no \"preview\""},
        {"no file", edited([](OrderedJson& j) { j.erase("file"); }), "has no \"file\""},
        {"absolute file", edited([](OrderedJson& j) { j["file"] = "/etc/passwd"; }), "\"file\" is not a plain path"},
        {"file with ..", edited([](OrderedJson& j) { j["file"] = "assets/../../evil"; }), "\"file\" is not a plain path"},
        {"file with .", edited([](OrderedJson& j) { j["file"] = "./assets/video.mp4"; }), "\"file\" is not a plain path"},
        {"file with an empty part", edited([](OrderedJson& j) { j["file"] = "assets//video.mp4"; }), "\"file\" is not a plain path"},
        {"file with a backslash", edited([](OrderedJson& j) { j["file"] = "assets\\video.mp4"; }), "\"file\" is not a plain path"},
        {"absolute preview", edited([](OrderedJson& j) { j["preview"] = "/preview.png"; }), "\"preview\" is not a plain path"},
        {"a tag that is a number", edited([](OrderedJson& j) { j["tags"] = OrderedJson::parse(R"(["a", 1])"); }), "tags are not a list of text"},
        {"tags as one string", edited([](OrderedJson& j) { j["tags"] = "nature"; }), "tags are not a list of text"},
        {"properties as a list", edited([](OrderedJson& j) { j["properties"] = OrderedJson::array(); }), "properties are not an object"},
        {"a property that is a number", property("speed", "1"), "Property \"speed\" in wallpaper.json is not an object"},
        {"a property with an empty name", property("", R"({"type": "bool", "default": true})"),
         "a property whose name is not 1 to 64 letters, digits, \"_\", \"-\" or \".\"."},
        {"a numeric label", property("speed", R"({"type": "float", "default": 1, "min": 0, "max": 2, "label": 5})"), "label that is not text"},
        {"an unknown property type", property("speed", R"({"type": "int", "default": 1})"), "has type \"int\""},
        {"a property with no type", property("speed", R"({"default": 1})"), "Property \"speed\" in wallpaper.json has no type"},
        {"a property with no default", property("speed", R"({"type": "float", "min": 0, "max": 2})"), "has no default"},
        {"a float with no max", property("speed", R"({"type": "float", "default": 1, "min": 0})"), "needs a numeric min and max"},
        {"a float with min == max", property("speed", R"({"type": "float", "default": 1, "min": 1, "max": 1})"), "min that is not below its max"},
        {"a float with min > max", property("speed", R"({"type": "float", "default": 1, "min": 3, "max": 1})"), "min that is not below its max"},
        {"a float default above max", property("speed", R"({"type": "float", "default": 9, "min": 0, "max": 2})"), "default outside its min and max"},
        {"a bool default that is a number", property("mute", R"({"type": "bool", "default": 1})"), "default that is not a bool"},
        {"a colour default #abc", property("tint", R"({"type": "color", "default": "#abc"})"), "default that is not a color"},
        {"a colour default #aarrggbb", property("tint", R"({"type": "color", "default": "#ffffffff"})"), "default that is not a color"},
        {"a colour default red", property("tint", R"({"type": "color", "default": "red"})"), "default that is not a color"},
    };
    if (!overflow.contains("1e999")) {
        harnessFailure("S15.48: the 1e999 manifest could not be made from PLAN's example");
        return;
    }
    // GOAL: S15.32, S15.48 -- every malformed manifest is refused, and the sentence names what is
    //       wrong with it, not some other field; a number too large for a double is one of them.
    // WHY-THIS-SHAPE: each case asserts its own reason substring, so a validator that refuses for
    //       the wrong reason (the file check firing on a missing title, say) goes red; asserting only
    //       "was refused" would pass a parser that rejects everything.
    // CONTROL: app/library/wpx_package.cpp, one branch per run, each red on its own row and
    //       restored: `!doc.is_object()` -> "a JSON array was refused with ...has no schema_version";
    //       `!versionOne` -> "schema_version "2" was accepted"; the type list -> "type gif was
    //       accepted"; the title test -> "empty title was accepted"; each of isPlainRelativePath's
    //       four terms -> "absolute file", "file with .", "file with ..", "file with a backslash
    //       was accepted"; tags' is_array and all_of terms -> "tags as one string", "a tag that is a
    //       number was accepted"; properties' is_object -> "properties as a list was accepted";
    //       parseProperty's is_object, type-refusal, no-default, min<max, range and bool tests ->
    //       their rows; inside isPropertyKey's anchoredPattern, `{1,` made `{0,` -> "a property with
    //       an empty name was accepted" (control_batch, 2026-10-04); inside colorPattern()'s,
    //       `{6}` made `{3,8}` -> "a colour default #abc was accepted";
    //       parseManifest's `catch (const Json::out_of_range&)` clause deleted -> "a number too
    //       large for a double was refused with "wallpaper.json cannot be read: ..."". Rows with
    //       no break of their own (invalid JSON or UTF-8, no schema_version, no type, numeric
    //       author, no preview/file, numeric label, a property with no type or max): removing
    //       those tests makes nlohmann throw or read past a missing key rather than accept.
    // COVERAGE-ASSERT: the case count is checked, so dropping rows fails.
    int checked = 0;
    for (const Case& c : cases) {
        const auto parsed = parseManifest(c.json);
        ++checked;
        if (parsed) {
            expect(false, QStringLiteral("S15.32: %1 was accepted").arg(QLatin1String(c.what)));
        } else {
            expect(parsed.error().contains(QLatin1String(c.reason)),
                   QStringLiteral("S15.32: %1 was refused with \"%2\", which does not say \"%3\"")
                       .arg(QLatin1String(c.what), parsed.error(), QLatin1String(c.reason)));
        }
    }
    expect(checked == 39, QStringLiteral("S15.32: %1 malformed manifests were checked, not 39").arg(checked));
}

// What the daemon makes of a wallpaper.json's properties: its own propertiesError() on every default,
// and for a float "speed" on its min and max too, the two ends a slider can reach. Empty: accepted.
std::string daemonVerdict(const OrderedJson& properties) {
    std::vector<std::pair<std::string, DaemonValue>> defaults;
    std::string extremes;
    for (const auto& [key, spec] : properties.items()) {
        const std::string type = spec.value("type", "");
        if (type == "float") {
            defaults.emplace_back(key, spec.at("default").get<double>());
            if (key == "speed") {
                for (const char* end : {"min", "max"}) {
                    if (const std::string why = daemonPropertiesError({{key, spec.at(end).get<double>()}}); !why.empty()) {
                        extremes = why;
                    }
                }
            }
        } else if (type == "bool") {
            defaults.emplace_back(key, spec.at("default").get<bool>());
        } else {
            defaults.emplace_back(key, spec.at("default").get<std::string>());
        }
    }
    const std::string why = daemonPropertiesError(defaults);
    return why.empty() ? extremes : why;
}

// S15.221-S15.225
void daemonPropertyRules() {
    const auto spec = [](const char* text) { return OrderedJson::parse(text); };
    const OrderedJson flag = spec(R"({"type": "bool", "default": true})");
    const auto one = [&](const std::string& key, const OrderedJson& s) {
        OrderedJson p = OrderedJson::object();
        p[key] = s;
        return p;
    };
    const auto many = [&](int n) {
        OrderedJson p = OrderedJson::object();
        for (int i = 0; i < n; ++i) {
            p["k" + std::to_string(i)] = flag;
        }
        return p;
    };
    const char* badKey = "a property whose name is not 1 to 64 letters, digits, \"_\", \"-\" or \".\".";
    struct Row {
        const char* goal;
        std::string what;
        OrderedJson properties;
        const char* reason; // nullptr: usable
    };
    // GOAL: S15.221 -- a property key outside [A-Za-z0-9_.-]{1,64} lists the wallpaper broken with
    //       that reason; a key of 64 allowed characters, and keys using every allowed character, are
    //       usable.
    // GOAL: S15.222 -- at most 64 properties: 64 are usable, 65 list it broken saying so.
    // GOAL: S15.223 -- "speed" declaring a range that starts at or below 0 or ends above 2 is
    //       refused, naming the range, not narrowed to fit; (0.1, 2] and the smallest positive
    //       minimum are usable; "Speed" is another key and is not checked.
    // GOAL: S15.224 -- a "speed" that is not a float, and a "mute" that is not a bool, list broken.
    // WHY-THIS-SHAPE: each refusal is paired with the usable case on the other side of its boundary
    //       (64/65 characters and properties, min 0/smallest positive, max 2/2.0001), so neither a
    //       validator that refuses too much nor one that refuses too little passes; each refusal's
    //       sentence is matched, so a refusal for some other reason is red.
    // CONTROL: app/library/wpx_package.cpp, each through control_batch, 2026-10-02 (the first re-run
    //       2026-10-04, inside anchoredPattern): isPropertyKey's
    //       `.arg(kMaxPropertyKeyBytes)` made `.arg(kMaxPropertyKeyBytes + 1)` -> red, "S15.221: a
    //       65-character key was accepted"; `[A-Za-z0-9_.-]` made `[ -~]` -> red, "S15.221: the key
    //       "my key" was accepted"; `> kMaxProperties` made `> kMaxProperties + 1` -> red, "S15.222: 65
    //       properties was accepted"; `!(property.minimum > 0.0)` made `property.minimum < 0.0` -> red,
    //       "S15.223: speed from 0 was accepted"; `> kMaxPlaybackSpeed` made `> 3` -> red, "S15.223: speed to
    //       2.0001 was accepted"; the not-a-float test made `if (false)` -> red, "S15.224: a bool speed
    //       was refused with "...allows 0 to 0, ...", not "...is not a float..."" (the range check
    //       catches it, with the wrong sentence); the mute test made `false` -> red, "S15.224: a float
    //       mute was accepted".
    // GOAL: S15.255 -- a key with a trailing newline, also after 64 allowed characters, and a colour
    //       default with one, are refused, as the daemon refuses them.
    // WHY-THIS-SHAPE: a pattern anchored with `^...$` matches before a final "\n"; graded against
    //       the daemon (S15.225 below) as well as by sentence.
    // CONTROL: app/library/wpx_package.cpp, isPropertyKey's pattern written
    //       `QStringLiteral("^[A-Za-z0-9_.-]{1,%1}$")` without anchoredPattern (control_batch,
    //       2026-10-04) -> red, "S15.255: a key ending in a newline was accepted", the 64-character
    //       row and their S15.225 lines; colorPattern()'s written `QStringLiteral("^#[0-9a-fA-F]{6}$")`
    //       -> red, "S15.255: a colour default ending in a newline was accepted" and its S15.225 line.
    std::vector<Row> rows{
        {"S15.221", "a 64-character key", one(std::string(64, 'k'), flag), nullptr},
        {"S15.221", "a 65-character key", one(std::string(65, 'k'), flag), badKey},
        {"S15.221", "the key \"my key\"", one("my key", flag), badKey},
        {"S15.221", "the key \"a/b\"", one("a/b", flag), badKey},
        {"S15.221", "a non-ASCII key", one("\xc3\xa9", flag), badKey},
        {"S15.221", "letters as a key", one("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz", flag), nullptr},
        {"S15.221", "digits and _.- as a key", one("0123456789_.-", flag), nullptr},
        {"S15.222", "64 properties", many(64), nullptr},
        {"S15.222", "65 properties", many(65), "wallpaper.json has 65 properties; WallpaperX takes at most 64."},
        {"S15.223", "speed in (0.1, 2]", one("speed", spec(R"({"type": "float", "default": 1, "min": 0.1, "max": 2})")), nullptr},
        {"S15.223", "speed from the smallest positive double", one("speed", spec(R"({"type": "float", "default": 1, "min": 5e-324, "max": 2})")), nullptr},
        {"S15.223", "speed from 0", one("speed", spec(R"({"type": "float", "default": 1, "min": 0, "max": 2})")),
         "Property \"speed\" in wallpaper.json allows 0 to 2, and WallpaperX plays video above 0 and at most 2 times its speed."},
        {"S15.223", "speed from -1", one("speed", spec(R"({"type": "float", "default": 1, "min": -1, "max": 2})")),
         "Property \"speed\" in wallpaper.json allows -1 to 2, and WallpaperX plays video above 0 and at most 2 times its speed."},
        {"S15.223", "speed to 2.0001", one("speed", spec(R"({"type": "float", "default": 1, "min": 0.5, "max": 2.0001})")),
         "Property \"speed\" in wallpaper.json allows 0.5 to 2.0001, and WallpaperX plays video above 0 and at most 2 times its speed."},
        {"S15.223", "\"Speed\" from 0 to 5", one("Speed", spec(R"({"type": "float", "default": 1, "min": 0, "max": 5})")), nullptr},
        {"S15.224", "a bool speed", one("speed", flag), "Property \"speed\" in wallpaper.json is not a float, and WallpaperX reads a video's speed as one."},
        {"S15.224", "a colour speed", one("speed", spec(R"({"type": "color", "default": "#ffffff"})")),
         "Property \"speed\" in wallpaper.json is not a float, and WallpaperX reads a video's speed as one."},
        {"S15.224", "a float mute", one("mute", spec(R"({"type": "float", "default": 0.5, "min": 0.1, "max": 1})")),
         "Property \"mute\" in wallpaper.json is not a bool, and WallpaperX reads \"mute\" as one."},
        {"S15.224", "a colour mute", one("mute", spec(R"({"type": "color", "default": "#000000"})")),
         "Property \"mute\" in wallpaper.json is not a bool, and WallpaperX reads \"mute\" as one."},
        {"S15.224", "a bool mute", one("mute", flag), nullptr},
        {"S15.255", "a key ending in a newline", one("glow\n", flag), badKey},
        {"S15.255", "a 64-character key and a newline", one(std::string(64, 'k') + "\n", flag), badKey},
        {"S15.255", "a colour default ending in a newline", one("tint", OrderedJson{{"type", "color"}, {"default", "#aabbcc\n"}}),
         "Property \"tint\" in wallpaper.json has a default that is not a color."},
    };
    // Every printable ASCII character the rule leaves out, as a one-character key.
    int outside = 0;
    for (char c = 0x20; c < 0x7f; ++c) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '.' && c != '-') {
            rows.push_back({"S15.221", std::string("the key \"") + c + "\"", one(std::string(1, c), flag), badKey});
            ++outside;
        }
    }
    // GOAL: S15.225 -- the app lists a wallpaper usable exactly when the daemon would take its
    //       properties: every default, and a speed's min and max, pass the daemon's own
    //       propertiesError() (backend/daemon/wallpaper_properties.cpp, compiled in).
    // WHY-THIS-SHAPE: graded against the daemon's code rather than this table, so a rule changed on
    //       either side alone is red here even where the table above was changed to match.
    // CONTROL: app/library/wpx_package.cpp isPropertyKey's `.arg(kMaxPropertyKeyBytes + 1)`
    //       (control_batch, 2026-10-04) -> red, "S15.225: a 65-character key: the app lists it usable,
    //       the daemon would refuse it (property name "kkk..." must be 1 to 64 ASCII letters, digits,
    //       '_', '-' or '.')"; each break above gave its own S15.225 line beside its row's.
    // COVERAGE-ASSERT: the row count, the 30 printable ASCII characters outside the rule among them.
    int checked = 0;
    for (const Row& row : rows) {
        const auto parsed = parseManifest(edited([&](OrderedJson& j) { j["properties"] = row.properties; }));
        const QString what = QString::fromStdString(row.what);
        ++checked;
        if (row.reason == nullptr) {
            expect(parsed.has_value(), QStringLiteral("%1: %2 was refused: %3").arg(QLatin1StringView(row.goal), what, parsed ? QString() : parsed.error()));
        } else if (parsed) {
            expect(false, QStringLiteral("%1: %2 was accepted").arg(QLatin1StringView(row.goal), what));
        } else {
            expect(parsed.error().endsWith(QString::fromUtf8(row.reason)),
                   QStringLiteral("%1: %2 was refused with \"%3\", not \"%4\"").arg(QLatin1StringView(row.goal), what, parsed.error(), QString::fromUtf8(row.reason)));
        }
        const std::string daemon = daemonVerdict(row.properties);
        expect(parsed.has_value() == daemon.empty(),
               QStringLiteral("S15.225: %1: the app lists it %2, the daemon would %3")
                   .arg(what, parsed ? QStringLiteral("usable") : QStringLiteral("broken"),
                        daemon.empty() ? QStringLiteral("accept it") : QStringLiteral("refuse it (%1)").arg(QString::fromStdString(daemon))));
    }
    expect(outside == 30 && checked == 23 + outside,
           QStringLiteral("S15.221-S15.225: %1 rows were checked, %2 of them characters outside the rule; expected 53 and 30").arg(checked).arg(outside));
}

// S15.35, S15.53
void acceptValues() {
    WallpaperProperty speed;
    speed.key = QStringLiteral("speed");
    speed.type = WallpaperProperty::Type::Float;
    speed.minimum = 0.1;
    speed.maximum = 3.0;
    WallpaperProperty mute;
    mute.type = WallpaperProperty::Type::Bool;
    WallpaperProperty tint;
    tint.type = WallpaperProperty::Type::Color;

    struct Case {
        const char* what;
        const WallpaperProperty& property;
        QVariant value;
        std::optional<QVariant> want;
    };
    const Case cases[] = {
        {"a float at exactly min", speed, 0.1, QVariant(0.1)},
        {"a float at exactly max", speed, 3.0, QVariant(3.0)},
        {"an int", speed, 2, QVariant(2.0)},
        {"a float just below min", speed, std::nextafter(0.1, 0.0), std::nullopt},
        {"a float just above max", speed, std::nextafter(3.0, 4.0), std::nullopt},
        {"NaN", speed, std::numeric_limits<double>::quiet_NaN(), std::nullopt},
        {"infinity", speed, std::numeric_limits<double>::infinity(), std::nullopt},
        {"the string \"1\" for a float", speed, QStringLiteral("1"), std::nullopt},
        {"a bool for a float", speed, true, std::nullopt},
        {"false for a bool", mute, false, QVariant(false)},
        {"the int 1 for a bool", mute, 1, std::nullopt},
        {"the string \"true\" for a bool", mute, QStringLiteral("true"), std::nullopt},
        {"#AbCdEf for a colour", tint, QStringLiteral("#AbCdEf"), QVariant(QStringLiteral("#abcdef"))},
        {"a QColor", tint, QColor(255, 0, 16), QVariant(QStringLiteral("#ff0010"))},
        {"a QColor with alpha 128", tint, QColor(255, 0, 0, 128), QVariant(QStringLiteral("#ff0000"))},
        {"an invalid QColor", tint, QColor(), std::nullopt},
        {"#abc for a colour", tint, QStringLiteral("#abc"), std::nullopt},
        {"#aarrggbb for a colour", tint, QStringLiteral("#80ff0000"), std::nullopt},
        {"red for a colour", tint, QStringLiteral("red"), std::nullopt},
        {"a number for a colour", tint, 16777215, std::nullopt},
    };
    // GOAL: S15.35, S15.53 -- accept() takes exactly what a property can hold: a float in
    //       [min, max] inclusive (an int widened to double), a real bool, a "#rrggbb" or a valid
    //       QColor stored as lower-case "#rrggbb", a QColor's alpha dropped; nothing else, and the
    //       text "#aarrggbb" still refused.
    // WHY-THIS-SHAPE: the boundaries are probed with nextafter, one ulp outside, so a check using
    //       a tolerance or `<=` on the wrong side goes red; the stored value is compared including its
    //       type, so returning the int unconverted also goes red.
    // CONTROL: app/library/wpx_package.cpp, `number < minimum || number > maximum` made
    //       `number <= minimum || number > maximum` -> red, "a float at exactly min"; the
    //       `!std::isfinite(number) ||` term deleted -> red, "NaN was accepted as nan" (infinity
    //       is still refused by the range test); `case QMetaType::QString:` added to the float cases -> red,
    //       "the string "1" for a float"; `return QVariant(number);` made `return value;` -> red,
    //       "an int gave 2 (int)"; `if (value.typeId() == QMetaType::QColor) {` made `if (false) {`
    //       -> red, "a QColor gave a refusal"; the QColor path's `color.isValid() ?` made
    //       `color.isValid() && color.alpha() == 255 ?` -> red, "a QColor with alpha 128 gave a
    //       refusal"; restored, green each time.
    for (const Case& c : cases) {
        const auto got = c.property.accept(c.value);
        if (!c.want) {
            expect(!got, QStringLiteral("S15.35: %1 was accepted as %2").arg(QLatin1String(c.what), got ? got->toString() : QString()));
        } else {
            expect(got && *got == *c.want && got->typeId() == c.want->typeId(),
                   QStringLiteral("S15.35: %1 gave %2 (%3), not %4")
                       .arg(QLatin1String(c.what), got ? got->toString() : QStringLiteral("a refusal"),
                            got ? QString::fromLatin1(got->typeName()) : QString(), c.want->toString()));
        }
    }

    // GOAL: S15.255 -- an edited colour with a trailing newline is refused, and the daemon refuses
    //       it too, so the app never sends what the daemon would turn down at Apply.
    // CONTROL: app/library/wpx_package.cpp, colorPattern()'s written `QStringLiteral("^#[0-9a-fA-F]{6}$")`
    //       without anchoredPattern (control_batch, 2026-10-04) -> red, "S15.255: an edited colour
    //       "#aabbcc\n" was taken as "#aabbcc\n"; the daemon says "property tint is not a colour"".
    const QString newline = QStringLiteral("#aabbcc\n");
    const auto edited = tint.accept(newline);
    const std::string daemon = daemonPropertiesError({{"tint", newline.toStdString()}});
    expect(!edited && !daemon.empty(),
           QStringLiteral("S15.255: an edited colour \"#aabbcc\\n\" was taken as \"%1\"; the daemon says \"%2\"")
               .arg(edited ? edited->toString() : QStringLiteral("(refused)"), QString::fromStdString(daemon)));
}

struct ArchiveCase {
    QString file;
    const char* reason;
};

// S15.33, S15.55
void badArchives(const QString& root) {
    const QString dir = root + QStringLiteral("/archives");
    if (!QDir().mkpath(dir)) {
        harnessFailure("S15.33: could not make " + dir.toStdString());
        return;
    }
    const auto at = [&dir](const char* name) { return dir + QLatin1Char('/') + QLatin1String(name); };
    const QByteArray plan(kPlanManifest);
    Entries noManifest = planEntries(plan);
    noManifest.removeFirst();
    Entries noAsset = planEntries(plan);
    noAsset.removeLast();
    Entries noPreview = planEntries(plan);
    noPreview.removeAt(1);
    QByteArray huge = plan;
    huge.append(QByteArray(kMaxManifestBytes + 1 - plan.size(), ' '));
    QByteArray atCap = plan;
    atCap.append(QByteArray(kMaxManifestBytes - plan.size(), ' '));
    Entries bigPreview = planEntries(plan);
    bigPreview[1].second = QByteArray(kMaxPreviewBytes + 1, '\0');

    // Every fixture is built before the one check of the faults, so none is graded unbuilt.
    QString setupFault;
    const auto make = [&](const char* name, const Entries& entries) {
        if (const QString why = writeZip(at(name), entries); !why.isEmpty()) {
            setupFault = why;
        }
    };
    make("good.wpx", planEntries(plan));
    make("nomanifest.wpx", noManifest);
    make("noasset.wpx", noAsset);
    make("nopreview.wpx", noPreview);
    make("huge.wpx", planEntries(huge));
    make("atcap.wpx", planEntries(atCap));
    make("future.wpx", planEntries(edited([](OrderedJson& j) { j["schema_version"] = "2"; })));
    if (!writeBytes(at("notzip.wpx"), "hello")) {
        setupFault = QStringLiteral("notzip.wpx could not be written");
    }
    if (const QString why = writeFolder(at("bigpreview"), bigPreview); !why.isEmpty()) {
        setupFault = why;
    }
    if (const QString why = writeFolder(at("percent"), {{QStringLiteral("shot%1.png"), QByteArray((1 << 20) + 1, '\0')}}); !why.isEmpty()) {
        setupFault = why;
    }
    const QString fifo = at("fifo.wpx");
    if (::mkfifo(QFile::encodeName(fifo).constData(), 0644) != 0) {
        setupFault = QStringLiteral("mkfifo failed: ") + QString::fromLocal8Bit(std::strerror(errno));
    }
    if (!setupFault.isEmpty()) {
        harnessFailure("S15.33: a fixture could not be built: " + setupFault.toStdString());
        return;
    }

    const Wallpaper good = readArchive(at("good.wpx"));
    expect(good.usable() && good.filePath == at("good.wpx") && good.fileName == u"good.wpx" && good.title == u"Ocean Waves" &&
               !good.previewSource().isEmpty(),
           QStringLiteral("S15.33: a good archive read as usable=%1 title \"%2\" problem \"%3\"").arg(good.usable()).arg(good.title, good.problem));
    // GOAL: S15.33 -- a wallpaper.json of exactly 1 MiB is read; one byte more is refused before it is parsed.
    // WHY-THIS-SHAPE: both manifests are PLAN's example padded with trailing spaces, so they are
    //       valid JSON: without the cap the over-size one would be read as usable, which is what goes red.
    // CONTROL: app/library/wpx_package.cpp, `st.size > static_cast<zip_uint64_t>(maxBytes)` made
    //       `false && ...` -> red, "S15.33: huge.wpx read as usable=1 title "Ocean Waves""; restored, green.
    const Wallpaper atCapRead = readArchive(at("atcap.wpx"));
    expect(atCapRead.usable(), QStringLiteral("S15.33: a wallpaper.json of exactly 1 MiB was refused: %1").arg(atCapRead.problem));

    const ArchiveCase cases[] = {
        {at("notzip.wpx"), "It is not a ZIP archive, so not a WallpaperX package."},
        {at("nomanifest.wpx"), "The archive has no wallpaper.json."},
        {at("huge.wpx"), "wallpaper.json is larger than 1 MiB."},
        {at("noasset.wpx"), "The archive has no assets/video.mp4, which wallpaper.json names."},
        {at("nopreview.wpx"), "The archive has no preview.png, which wallpaper.json names."},
        {at("future.wpx"), "wallpaper.json is for schema version \"2\""},
    };
    // GOAL: S15.33 -- each archive that cannot be used comes back broken with its own reason, its
    //       filePath and fileName set, and every manifest field empty, so nothing half-read can be
    //       shown or applied.
    // WHY-THIS-SHAPE: the manifest-field emptiness is checked on every broken result because the
    //       asset check runs after parsing, when those fields are already filled.
    // CONTROL: app/library/wpx_package.cpp, ArchiveContents::find's `if (!locate(m_zip, name)) {`
    //       made `if (false) {` -> red, "noasset.wpx read as usable=1"; readFrom's asset
    //       `return broken(std::move(file), asset.error());` made to return the parsed wallpaper
    //       with `problem` set -> red, "noasset.wpx read as usable=0 title "Ocean Waves""; the
    //       ZIP_ER_NOZIP branch's condition made `false` -> red, "notzip.wpx ... It cannot be read
    //       as a ZIP archive"; restored, green each time.
    for (const ArchiveCase& c : cases) {
        const Wallpaper w = readArchive(c.file);
        expect(isBrokenOnly(w) && w.problem.contains(QLatin1String(c.reason)) && w.filePath == c.file &&
                   w.fileName == QFileInfo(c.file).fileName(),
               QStringLiteral("S15.33: %1 read as usable=%2 title \"%3\" problem \"%4\"; wanted broken, \"%5\", nothing else set")
                   .arg(QFileInfo(c.file).fileName())
                   .arg(w.usable())
                   .arg(w.title, w.problem, QLatin1String(c.reason)));
    }

    // GOAL: S15.33 -- a preview file over 32 MiB in a wallpaper folder is refused by its size, before
    //       it is read.
    // CONTROL: app/library/wpx_package.cpp, readToEnd's `if (size > maxBytes) {` made `if (false) {`
    //       -> red, "S15.33: a 32 MiB + 1 preview was read: 33554433 bytes" (and the two checks below
    //       it that rely on the same cap, S15.55's and S15.67's); restored.
    const auto preview = readWallpaperEntry(at("bigpreview"), QStringLiteral("preview.png"), kMaxPreviewBytes);
    expect(!preview && preview.error() == u"preview.png is larger than 32 MiB.",
           QStringLiteral("S15.33: a 32 MiB + 1 preview was read: %1").arg(preview ? QStringLiteral("%1 bytes").arg(preview->size()) : preview.error()));

    // GOAL: S15.55 -- a file name holding "%1" is quoted as it is in a refusal, not taken as a
    //       place for the size to go.
    // CONTROL: app/library/wpx_package.cpp, FolderContents::read's `.arg(name, QString::number(maxBytes >> 20))`
    //       made `.arg(name).arg(QString::number(maxBytes >> 20))` -> red, "S15.55: ... "shot1.png
    //       is larger than %2 MiB.""; restored.
    const auto percent = readWallpaperEntry(at("percent"), QStringLiteral("shot%1.png"), 1 << 20);
    expect(!percent && percent.error() == u"shot%1.png is larger than 1 MiB.",
           QStringLiteral("S15.55: a file named %1 over the cap was refused with \"%2\"")
               .arg(QStringLiteral("shot%1.png"), percent ? QStringLiteral("nothing") : percent.error()));

    // GOAL: S15.33 -- a descriptor that is not a regular file (a FIFO put in a .wpx's place) is
    //       refused as "not a regular file", not handed to the ZIP reader.
    // WHY-THIS-SHAPE: opened here with O_NONBLOCK, so the open cannot hang; whether the importer's
    //       own open of such a path blocks is S15.76's (importer_test.cpp).
    // CONTROL: app/library/wpx_package.cpp, openArchive's `|| !S_ISREG(st.st_mode)` deleted -> red,
    //       "S15.33: a FIFO read as usable=0 problem "It cannot be read as a ZIP archive: Operation
    //       not supported.""; restored.
    const Wallpaper fifoRead = readArchive(fifo);
    expect(isBrokenOnly(fifoRead) && fifoRead.problem == u"It is not a regular file.",
           QStringLiteral("S15.33: a FIFO read as usable=%1 problem \"%2\"").arg(fifoRead.usable()).arg(fifoRead.problem));
}

// S15.34
void scanFolder(const QString& root) {
    const QString lib = root + QStringLiteral("/library");
    const QString elsewhere = root + QStringLiteral("/elsewhere");
    const auto at = [&lib](const QString& name) { return lib + QLatin1Char('/') + name; };
    const QByteArray plan(kPlanManifest);
    Entries noManifest = planEntries(plan);
    noManifest.removeFirst();
    Entries noAsset = planEntries(plan);
    noAsset.removeLast();

    QString fault;
    const auto make = [&fault](const QString& dir, const Entries& entries) {
        if (const QString why = writeFolder(dir, entries); !why.isEmpty()) {
            fault = why;
        }
    };
    make(at(QStringLiteral("ocean")), planEntries(plan));
    // Lower case, so by title alone it would sort after every broken folder's name in any locale;
    // only the usable-first rule puts it ahead of them.
    make(at(QStringLiteral("still")), planEntries(edited([](OrderedJson& j) { j["title"] = "zebra still"; })));
    make(elsewhere + QStringLiteral("/linked"), planEntries(edited([](OrderedJson& j) { j["title"] = "Linked"; })));
    make(at(QStringLiteral(".hidden")), planEntries(plan));
    make(at(QStringLiteral(".import-abc123")), planEntries(plan));
    make(at(QStringLiteral("nomanifest")), noManifest);
    make(at(QStringLiteral("badjson")), planEntries("{ nope"));
    make(at(QStringLiteral("escape")), planEntries(edited([](OrderedJson& j) { j["file"] = "../../evil"; })));
    make(at(QStringLiteral("future")), planEntries(edited([](OrderedJson& j) { j["schema_version"] = "2"; })));
    make(at(QStringLiteral("outofrange")), planEntries(edited([](OrderedJson& j) { j["properties"]["speed"]["default"] = 9; })));
    make(at(QStringLiteral("missingasset")), noAsset);
    make(at(QStringLiteral("assetdir")), noAsset);
    make(at(QStringLiteral("fifojson")), noManifest);
    if (!QDir().mkpath(at(QStringLiteral("assetdir/assets/video.mp4"))) ||
        ::mkfifo(QFile::encodeName(at(QStringLiteral("fifojson/wallpaper.json"))).constData(), 0644) != 0 ||
        !writeBytes(at(QStringLiteral("readme.txt")), "hello") || !writeZip(at(QStringLiteral("package.wpx")), planEntries(plan)).isEmpty() ||
        ::mkfifo(QFile::encodeName(at(QStringLiteral("fifo"))).constData(), 0644) != 0 ||
        !makeSymlink(QStringLiteral("../elsewhere/linked"), at(QStringLiteral("linked"))) ||
        !makeSymlink(QStringLiteral("readme.txt"), at(QStringLiteral("filelink"))) ||
        !makeSymlink(QStringLiteral("nothing-here"), at(QStringLiteral("dangling")))) {
        fault = QStringLiteral("a plain file, archive, FIFO, symlink or folder could not be made");
    }
    const bool asRoot = ::geteuid() == 0;
    if (!asRoot) {
        make(at(QStringLiteral("locked")), planEntries(plan));
        if (!setMode(at(QStringLiteral("locked")), 0)) {
            fault = QStringLiteral("locked could not be made mode 000");
        }
    }
    if (!fault.isEmpty()) {
        harnessFailure("S15.34: the library folder could not be built: " + fault.toStdString());
        static_cast<void>(setMode(at(QStringLiteral("locked")), 0755));
        return;
    }

    using Scan = std::expected<QList<Wallpaper>, QString>;
    // GOAL: S15.34 -- a wallpaper folder whose wallpaper.json is a FIFO never blocks a scan; it
    //       is listed broken, "wallpaper.json is not a regular file.".
    // WHY-THIS-SHAPE: bounded as in S15.33. A FIFO directly in the library is not a folder, so a
    //       scan never opens it; one named by wallpaper.json inside a folder is opened, and only
    //       the non-blocking open stands in front of the hang.
    // CONTROL: app/library/wpx_package.cpp, FolderContents::open's `O_RDONLY | O_NONBLOCK | O_NOCTTY`
    //       made `O_RDONLY | O_NOCTTY` -> red, "S15.34: scanning a library holding a FIFO blocked for
    //       3000 ms"; restored.
    const QString fifo = at(QStringLiteral("fifojson/wallpaper.json"));
    const auto scanned = runBounded<Scan>([&lib] { return scanLibrary(lib); }, fifo, QStringLiteral("S15.34: scanning a library holding a FIFO"), kFifoTimeoutMs);
    const Wallpaper linkRead = readWallpaperFolder(at(QStringLiteral("linked")));
    if (!asRoot && !setMode(at(QStringLiteral("locked")), 0755)) {
        harnessFailure("S15.34: locked could not be made readable again");
    }
    if (!scanned) {
        return;
    }
    if (!*scanned) {
        expect(false, QStringLiteral("S15.34: the library folder could not be scanned: %1").arg(scanned->error()));
        return;
    }
    const QList<Wallpaper>& list = **scanned;

    // GOAL: S15.34 -- every wallpaper folder in the library is listed, usable ones first by title
    //       and broken ones after by folder name, each broken one with its own reason; a link to a
    //       folder is listed broken; hidden folders (the importer's unfinished ones among them),
    //       plain files, .wpx archives, FIFOs and links to anything but a folder are not listed.
    // WHY-THIS-SHAPE: the whole listing is compared in order, so a missing, extra or reordered
    //       entry is named. One usable title is lower case: with capitalised titles only, the C
    //       locale sorts them all before the broken names anyway and the usable-first rule goes
    //       untested.
    // CONTROL: app/library/wpx_package.cpp, listedBefore()'s `if (a.usable() != b.usable())` block
    //       deleted -> red, "listed [Ocean Waves, assetdir(broken), ... zebra still]"; scanLibrary's
    //       `if (name.startsWith('.')) {` made `if (false) {` -> red, "listed [Ocean Waves, Ocean
    //       Waves, Ocean Waves, zebra still, .(broken), ..(broken), ...]" (.hidden and
    //       .import-abc123 by their titles); `if (S_ISDIR(st.st_mode)) {` made `if (true) {` -> red,
    //       "fifo is broken with "It is not a folder."" and the same for package.wpx and readme.txt;
    //       the link branch's `wallpapers.append(broken(...))` deleted -> red, "linked" missing; restored.
    QStringList want{QStringLiteral("Ocean Waves"), QStringLiteral("zebra still"), QStringLiteral("assetdir(broken)"),
                     QStringLiteral("badjson(broken)"), QStringLiteral("escape(broken)"), QStringLiteral("fifojson(broken)"),
                     QStringLiteral("future(broken)"), QStringLiteral("linked(broken)")};
    if (!asRoot) {
        want.append(QStringLiteral("locked(broken)"));
    }
    want += QStringList{QStringLiteral("missingasset(broken)"), QStringLiteral("nomanifest(broken)"), QStringLiteral("outofrange(broken)")};
    QStringList got;
    for (const Wallpaper& w : list) {
        got.append(w.usable() ? w.title : w.fileName + QStringLiteral("(broken)"));
    }
    expect(got == want, QStringLiteral("S15.34: the library listed [%1], not [%2]").arg(got.join(u", "), want.join(u", ")));

    // GOAL: S15.34 -- each broken folder is broken for its own reason, with nothing read from its
    //       manifest kept.
    // CONTROL: app/library/wpx_package.cpp, FolderContents::open's `|| !S_ISREG(st.st_mode)` deleted
    //       -> red, "assetdir is usable" and "fifojson is broken with "wallpaper.json is not valid
    //       JSON ...""; restored.
    // COVERAGE-ASSERT: the number of broken rows whose reason was compared.
    const QMap<QString, QString> reasons{
        {QStringLiteral("assetdir"), QStringLiteral("assets/video.mp4 is not a regular file.")},
        {QStringLiteral("badjson"), QStringLiteral("wallpaper.json is not valid JSON")},
        {QStringLiteral("escape"), QStringLiteral("wallpaper.json's \"file\" is not a plain path inside the wallpaper.")},
        {QStringLiteral("fifojson"), QStringLiteral("wallpaper.json is not a regular file.")},
        {QStringLiteral("future"), QStringLiteral("schema version \"2\"")},
        {QStringLiteral("linked"), QStringLiteral("It is a link to a folder. A wallpaper has to be a folder inside the library itself.")},
        {QStringLiteral("locked"), QStringLiteral("It cannot be opened: Permission denied.")},
        {QStringLiteral("missingasset"), QStringLiteral("The folder has no assets/video.mp4, which wallpaper.json names.")},
        {QStringLiteral("nomanifest"), QStringLiteral("The folder has no wallpaper.json.")},
        {QStringLiteral("outofrange"), QStringLiteral("default outside its min and max")},
    };
    int reasonsChecked = 0;
    for (const Wallpaper& w : list) {
        if (!w.usable()) {
            ++reasonsChecked;
            expect(isBrokenOnly(w) && w.problem.contains(reasons.value(w.fileName, QStringLiteral("\x01"))),
                   QStringLiteral("S15.34: %1 is broken with \"%2\", not \"%3\"").arg(w.fileName, w.problem, reasons.value(w.fileName)));
        } else if (w.fileName == u"assetdir" || w.fileName == u"fifojson") {
            expect(false, QStringLiteral("S15.34: %1 is usable").arg(w.fileName));
        }
    }
    expect(reasonsChecked >= 9, QStringLiteral("S15.34: %1 broken folder(s) had their reason checked, not 9 or more").arg(reasonsChecked));

    // GOAL: S15.34 -- a usable folder's row names the folder, and its assetPath is the folder, a
    //       slash and the manifest's file: the path the daemon is to open.
    // CONTROL: app/library/wpx_package.cpp, readFolderAt's `read.assetPath = read.filePath + u'/' + read.assetName;`
    //       deleted -> red, "S15.34: ocean read as path … name ocean asset path """; restored.
    const auto ocean = std::ranges::find(list, QStringLiteral("ocean"), &Wallpaper::fileName);
    expect(ocean != list.end() && ocean->filePath == at(QStringLiteral("ocean")) && ocean->title == u"Ocean Waves" &&
               ocean->assetPath == at(QStringLiteral("ocean/assets/video.mp4")),
           QStringLiteral("S15.34: ocean read as path %1 name %2 asset path \"%3\"")
               .arg(ocean != list.end() ? ocean->filePath : QString(), ocean != list.end() ? ocean->fileName : QString(),
                    ocean != list.end() ? ocean->assetPath : QString()));

    // GOAL: S15.34 -- a wallpaper folder that is a link is refused by the folder reader itself,
    //       not only by the scan, so nothing reads through it into another folder.
    // CONTROL: app/library/wpx_package.cpp, readFolderAt's `O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC`
    //       made `O_RDONLY | O_DIRECTORY | O_CLOEXEC` -> red, "S15.34: the linked folder read
    //       directly as usable=1 "Linked""; restored.
    expect(isBrokenOnly(linkRead) && linkRead.problem == u"It is a link, not a folder.",
           QStringLiteral("S15.34: the linked folder read directly as usable=%1 \"%2\" problem \"%3\"")
               .arg(linkRead.usable()).arg(linkRead.title, linkRead.problem));

    // GOAL: S15.34 -- a folder that does not exist is an empty library, not an error; a path that
    //       is a file, or a folder that cannot be listed, is an error that says which.
    // CONTROL: app/library/wpx_package.cpp, scanLibrary's `if (error == ENOENT) {` made `if (false) {`
    //       -> red, "a missing folder gave an error"; `if (error == ENOTDIR) {` made `if (false) {` ->
    //       red, "a path that is a file gave ... cannot be read: Not a directory"; the opendir failure
    //       made to return an empty list -> red, "a folder that cannot be listed gave a listing"; restored.
    const auto missing = scanLibrary(root + QStringLiteral("/no/such/folder"));
    expect(missing && missing->isEmpty(), QStringLiteral("S15.34: a missing folder gave %1")
                                              .arg(missing ? QStringLiteral("%1 entries").arg(missing->size()) : QStringLiteral("an error: ") + missing.error()));
    const auto file = scanLibrary(at(QStringLiteral("readme.txt")));
    expect(!file && file.error() == at(QStringLiteral("readme.txt")) + QStringLiteral(" is not a folder."),
           QStringLiteral("S15.34: a path that is a file gave %1").arg(file ? QStringLiteral("a listing") : file.error()));
    if (asRoot) {
        notVerified("S15.34 (a folder that cannot be listed)", "running as root, which can list a mode-000 folder");
    } else {
        const QString closed = root + QStringLiteral("/closed");
        if (!QDir().mkpath(closed) || !setMode(closed, 0)) {
            harnessFailure("S15.34: the mode-000 folder could not be made");
            return;
        }
        const auto denied = scanLibrary(closed);
        if (!setMode(closed, 0755)) {
            harnessFailure("S15.34: the mode-000 folder could not be made readable again");
        }
        expect(!denied && denied.error() == closed + QStringLiteral(" cannot be read: Permission denied."),
               QStringLiteral("S15.34: a folder that cannot be listed gave %1").arg(denied ? QStringLiteral("a listing") : denied.error()));

        // GOAL: S15.34 -- an entry that is listed but cannot be looked at (a library readable but
        //       not searchable) is a broken row giving the kernel's own reason, not one some later
        //       call left behind in errno.
        // CONTROL: app/library/wpx_package.cpp, scanLibrary's per-entry `const int error = errno;`
        //       given `::close(-1);` just before it -> red, "S15.34: an entry that cannot be looked
        //       at read as [a: It cannot be read: Bad file descriptor.]"; restored.
        const QString unsearchable = root + QStringLiteral("/unsearchable");
        if (!writeFolder(unsearchable + QStringLiteral("/a"), planEntries(plan)).isEmpty() || !setMode(unsearchable, 0444)) {
            harnessFailure("S15.34: the library that cannot be searched could not be made");
            static_cast<void>(setMode(unsearchable, 0755));
            return;
        }
        const auto rows = scanLibrary(unsearchable);
        if (!setMode(unsearchable, 0755)) {
            harnessFailure("S15.34: the library that cannot be searched could not be made searchable again");
        }
        QStringList read;
        for (const Wallpaper& w : rows.value_or(QList<Wallpaper>())) {
            read.append(w.fileName + QStringLiteral(": ") + w.problem);
        }
        expect(rows && read == QStringList{QStringLiteral("a: It cannot be read: Permission denied.")},
               QStringLiteral("S15.34: an entry that cannot be looked at read as [%1]%2").arg(read.join(u" | "), rows ? QString() : QStringLiteral(", ") + rows.error()));
    }
}

// S15.50
void folderNamesThatAreNotUtf8(const QString& root) {
    const QString dir = root + QStringLiteral("/names");
    const QString staging = root + QStringLiteral("/names-staging");
    const QByteArray dirBytes = QFile::encodeName(dir);
    const RawNamesRemover removeRawNames(dir, {"caf\xe8", "caf\xe9"});
    bool built = QDir().mkpath(dir);
    for (const QByteArray& name : {QByteArray("caf\xe8"), QByteArray("caf\xe9"), QByteArray("caf\xc3\xa9")}) {
        const QString staged = staging + QStringLiteral("/w");
        built = built && writeFolder(staged, planEntries(QByteArray(kPlanManifest))).isEmpty() &&
                ::rename(QFile::encodeName(staged).constData(), (dirBytes + '/' + name).constData()) == 0;
    }
    if (!built) {
        harnessFailure("S15.50: the folders with odd names could not be built");
        return;
    }
    const auto scan = scanLibrary(dir);
    if (!scan) {
        expect(false, QStringLiteral("S15.50: the folder of odd names could not be scanned: %1").arg(scan.error()));
        return;
    }
    const auto rowFor = [&](const QByteArray& name) -> const Wallpaper* {
        const auto it = std::ranges::find_if(*scan, [&](const Wallpaper& w) { return encodeFileName(w.filePath) == dirBytes + '/' + name; });
        return it == scan->end() ? nullptr : &*it;
    };
    const QString notUtf8 = QStringLiteral("Its path is not valid UTF-8, so it cannot be set as a wallpaper. Rename the folder.");
    const Wallpaper* e8 = rowFor("caf\xe8");
    const Wallpaper* e9 = rowFor("caf\xe9");
    const Wallpaper* valid = rowFor("caf\xc3\xa9");
    QStringList listed;
    for (const Wallpaper& w : *scan) {
        listed.append(QString::fromLatin1(encodeFileName(w.fileName).toPercentEncoding()) + (w.usable() ? QString() : QStringLiteral("(broken: ") + w.problem + u')'));
    }
    // GOAL: S15.50 -- a wallpaper folder whose name is not UTF-8 is listed, broken, with the reason;
    //       two such names differing in one byte are two rows with distinct paths, each converting
    //       back to its own folder's bytes; a valid UTF-8 name is read normally.
    // WHY-THIS-SHAPE: each row is looked up by converting its path back to bytes, so a row whose
    //       text no longer names its folder is not found; comparing texts would pass two names both
    //       decoded to U+FFFD.
    // CONTROL: app/library/wpx_package.cpp, readFolderAt's `if (!QStringView(path).isValidUtf16()) {`
    //       made `if (false) {` -> red, "S15.50: the folder of odd names listed [caf%C3%A9, caf%E8,
    //       caf%E9]", all three usable; decodeFileName's `QChar(char16_t(0xDC00 + value))` made
    //       `QChar(char16_t(0xFFFD))` (what QFile::decodeName does) -> red, both rows named
    //       "caf%EF%BF%BD" and read as usable; restored, green each time.
    expect(scan->size() == 3 && e8 && e9 && valid && e8 != e9 && e8->filePath != e9->filePath && e8->problem == notUtf8 &&
               e9->problem == notUtf8 && !QStringView(e8->filePath).isValidUtf16() && valid->usable() &&
               valid->fileName == QStringLiteral("café"),
           QStringLiteral("S15.50: the folder of odd names listed [%1]").arg(listed.join(u", ")));

    // GOAL: S15.50 -- a path's text converts back to exactly the bytes it came from, for any
    //       bytes: valid UTF-8 as the same text QString::fromUtf8 gives, anything else as text that
    //       is not valid UTF-16, so no other name can be taken for it.
    // WHY-THIS-SHAPE: fixed edge cases (a leading byte-order mark, which a UTF-8 decoder drops by
    //       default; overlong and surrogate encodings; a lone continuation byte) plus random bytes.
    // CONTROL: app/library/wpx_package.cpp, decodeFileName's `| QStringDecoder::Flag::ConvertInitialBom`
    //       deleted -> red, "S15.50: ef bb bf 62 6f 6d 2e 77 70 78 came back as 62 6f 6d 2e 77 70 78";
    //       encodeFileName's `bytes.append(static_cast<char>(unit & 0xFF));` made `bytes.append('?');`
    //       -> red, "S15.50: 63 61 66 e9 2e 77 70 78 came back as 3f 3f 3f 3f 3f 3f 3f 3f"; restored.
    // COVERAGE-ASSERT: the number of byte strings compared, 11 fixed and 300 random. It guards this
    //       check against an edit that narrows its samples; no production change can move it.
    //       Controlled test-side: the random loop's bound made 0 -> red, "S15.50: 11 byte strings
    //       were compared, not 311"; restored.
    QList<QByteArray> samples{"plain.wpx", "caf\xc3\xa9.wpx", "caf\xe9.wpx", "\xef\xbb\xbf" "bom.wpx", "\xff\xfe", "a\x80z",
                              "\xc0\xaf", "\xed\xa0\x80", "\xf0\x9f\x98\x80.wpx", "\xc3", "\xe2\x82"};
    QRandomGenerator random(15050);
    for (int i = 0; i < 300; ++i) {
        QByteArray bytes(1 + int(random.bounded(24)), Qt::Uninitialized);
        for (char& b : bytes) {
            b = char(1 + random.bounded(255));
        }
        samples.append(bytes);
    }
    int compared = 0;
    for (const QByteArray& bytes : samples) {
        ++compared;
        const QString text = decodeFileName(bytes);
        const QByteArray back = encodeFileName(text);
        const bool utf8 = QString::fromUtf8(bytes).toUtf8() == bytes && !bytes.startsWith("\xef\xbb\xbf");
        const bool ok = back == bytes && (utf8 ? text == QString::fromUtf8(bytes) && text.isValidUtf16() : true) &&
                        (!utf8 && !bytes.startsWith("\xef\xbb\xbf") ? !text.isValidUtf16() : true);
        expect(ok, QStringLiteral("S15.50: %1 came back as %2 (valid UTF-16: %3)")
                       .arg(QString::fromLatin1(bytes.toHex(' ')), QString::fromLatin1(back.toHex(' '))).arg(text.isValidUtf16()));
    }
    expect(compared == 311, QStringLiteral("S15.50: %1 byte strings were compared, not 311").arg(compared));
}

// S15.67
void sharedValidator(const QString& root) {
    const QString dir = root + QStringLiteral("/shared");
    const QByteArray plan(kPlanManifest);
    Entries noPreview = planEntries(plan);
    noPreview.removeAt(1);
    QByteArray huge = plan;
    huge.append(QByteArray(kMaxManifestBytes + 1 - plan.size(), ' '));
    QByteArray atCap = plan;
    atCap.append(QByteArray(kMaxManifestBytes - plan.size(), ' '));
    if (!writeFolder(dir + QStringLiteral("/nopreview"), noPreview).isEmpty() ||
        !writeZip(dir + QStringLiteral("/nopreview.wpx"), noPreview).isEmpty() ||
        !writeFolder(dir + QStringLiteral("/whole"), planEntries(plan)).isEmpty() ||
        !writeZip(dir + QStringLiteral("/whole.wpx"), planEntries(plan)).isEmpty() ||
        !writeFolder(dir + QStringLiteral("/huge"), planEntries(huge)).isEmpty() ||
        !writeFolder(dir + QStringLiteral("/atcap"), planEntries(atCap)).isEmpty()) {
        harnessFailure("S15.67: the folders and archives could not be built");
        return;
    }
    // GOAL: S15.67 -- one validator serves a folder and an archive: the same contents missing the
    //       preview wallpaper.json names are refused in both, each saying so, while the same
    //       contents whole are usable in both.
    // WHY-THIS-SHAPE: the control is made in the one place both readers share (readFrom), so both
    //       rows going red together is what shows the rule is shared rather than written twice;
    //       the whole pair proves the refusal is about the preview and nothing else.
    // CONTROL: app/library/wpx_package.cpp, readFrom's `contents.find(parsed->previewName)` made
    //       `contents.find(parsed->assetName)` -> red, "S15.67: the folder missing its preview read
    //       as usable=1 ..." and "... the archive missing its preview read as usable=1 ..."; restored.
    const Wallpaper folder = readWallpaperFolder(dir + QStringLiteral("/nopreview"));
    const Wallpaper archive = readArchive(dir + QStringLiteral("/nopreview.wpx"));
    const Wallpaper wholeFolder = readWallpaperFolder(dir + QStringLiteral("/whole"));
    const Wallpaper wholeArchive = readArchive(dir + QStringLiteral("/whole.wpx"));
    expect(isBrokenOnly(folder) && folder.problem == u"The folder has no preview.png, which wallpaper.json names.",
           QStringLiteral("S15.67: the folder missing its preview read as usable=%1 problem \"%2\"").arg(folder.usable()).arg(folder.problem));
    expect(isBrokenOnly(archive) && archive.problem == u"The archive has no preview.png, which wallpaper.json names.",
           QStringLiteral("S15.67: the archive missing its preview read as usable=%1 problem \"%2\"").arg(archive.usable()).arg(archive.problem));
    expect(wholeFolder.usable() && wholeArchive.usable() && wholeFolder.properties == wholeArchive.properties &&
               wholeFolder.title == wholeArchive.title,
           QStringLiteral("S15.67: the same contents whole read as folder usable=%1 (%2), archive usable=%3 (%4)")
               .arg(wholeFolder.usable()).arg(wholeFolder.problem).arg(wholeArchive.usable()).arg(wholeArchive.problem));

    // GOAL: S15.67 -- a folder's wallpaper.json is held to the archive's 1 MiB cap: exactly 1 MiB is
    //       read, one byte more is refused before it is parsed.
    // WHY-THIS-SHAPE: both manifests are PLAN's example padded with spaces, so without the cap the
    //       larger one would read as usable.
    // CONTROL: app/library/wpx_package.cpp, readToEnd's `if (size > maxBytes) {` made `if (false) {`
    //       -> red, "S15.67: a folder's 1 MiB + 1 wallpaper.json read as usable=1 ..." (its read
    //       loop alone refuses only a file that grows past the cap while read); restored.
    const Wallpaper hugeRead = readWallpaperFolder(dir + QStringLiteral("/huge"));
    const Wallpaper atCapRead = readWallpaperFolder(dir + QStringLiteral("/atcap"));
    expect(isBrokenOnly(hugeRead) && hugeRead.problem == u"wallpaper.json is larger than 1 MiB." && atCapRead.usable(),
           QStringLiteral("S15.67: a folder's 1 MiB + 1 wallpaper.json read as usable=%1 problem \"%2\"; exactly 1 MiB read as usable=%3 \"%4\"")
               .arg(hugeRead.usable()).arg(hugeRead.problem).arg(atCapRead.usable()).arg(atCapRead.problem));
}

// S15.68
void containment(const QString& root) {
    const QString dir = root + QStringLiteral("/contain");
    const QString outsideRoot = root + QStringLiteral("/outside.png");
    const QString outsideFifo = root + QStringLiteral("/outside.fifo");
    const QByteArray plan(kPlanManifest);
    Entries noAsset = planEntries(plan);
    noAsset.removeLast();
    Entries noPreview = planEntries(plan);
    noPreview.removeAt(1);
    const auto at = [&dir](const char* name) { return dir + QLatin1Char('/') + QLatin1String(name); };
    // rel's link climbs to contain/outside.mp4: out of the wallpaper, still inside the library.
    const bool built =
        writeBytes(outsideRoot, pngBytes(32, 18)) &&
        writeFolder(dir, {{QStringLiteral("outside.mp4"), "outside the wallpaper"},
                          {QStringLiteral("outside-assets/video.mp4"), "outside the wallpaper"}}).isEmpty() &&
        writeFolder(at("abs"), noAsset).isEmpty() && QDir().mkpath(at("abs/assets")) &&
        makeSymlink(outsideRoot, at("abs/assets/video.mp4")) && writeFolder(at("rel"), noAsset).isEmpty() &&
        QDir().mkpath(at("rel/assets")) && makeSymlink(QStringLiteral("../../outside.mp4"), at("rel/assets/video.mp4")) &&
        writeFolder(at("dirlink"), noAsset).isEmpty() && makeSymlink(QStringLiteral("../outside-assets"), at("dirlink/assets")) &&
        writeFolder(at("inside"), noAsset).isEmpty() && writeBytes(at("inside/real.mp4"), "inside the wallpaper") &&
        QDir().mkpath(at("inside/assets")) && makeSymlink(QStringLiteral("../real.mp4"), at("inside/assets/video.mp4")) &&
        writeFolder(at("abspreview"), noPreview).isEmpty() && makeSymlink(outsideRoot, at("abspreview/preview.png")) &&
        writeFolder(at("absinside"), noAsset).isEmpty() && writeBytes(at("absinside/real.mp4"), "inside the wallpaper") &&
        QDir().mkpath(at("absinside/assets")) && makeSymlink(at("absinside/real.mp4"), at("absinside/assets/video.mp4")) &&
        writeFolder(at("outin"), noAsset).isEmpty() && writeBytes(at("outin/real.mp4"), "inside the wallpaper") &&
        QDir().mkpath(at("outin/assets")) && makeSymlink(QStringLiteral("../../outin/real.mp4"), at("outin/assets/video.mp4")) &&
        ::mkfifo(QFile::encodeName(outsideFifo).constData(), 0644) == 0 &&
        writeFolder(at("fifolink"), noAsset).isEmpty() && QDir().mkpath(at("fifolink/assets")) &&
        makeSymlink(outsideFifo, at("fifolink/assets/video.mp4"));
    if (!built) {
        harnessFailure("S15.68: the folders with links could not be built");
        return;
    }
    // Opening the FIFO for reading raises IN_OPEN on it; finding it with O_PATH does not.
    const int fifoOpens = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fifoOpens < 0 || ::inotify_add_watch(fifoOpens, QFile::encodeName(outsideFifo).constData(), IN_OPEN) < 0) {
        harnessFailure("S15.68: the outside FIFO could not be watched for opens");
        if (fifoOpens >= 0) {
            ::close(fifoOpens);
        }
        return;
    }
    const auto takeFifoOpens = [fifoOpens] {
        int opens = 0;
        alignas(inotify_event) char buffer[4096];
        for (ssize_t got; (got = ::read(fifoOpens, buffer, sizeof buffer)) > 0;) {
            for (const char* p = buffer; p < buffer + got; p += sizeof(inotify_event) + reinterpret_cast<const inotify_event*>(p)->len) {
                opens += (reinterpret_cast<const inotify_event*>(p)->mask & IN_OPEN) != 0 ? 1 : 0;
            }
        }
        return opens;
    };
    // Whether this kernel has openat2 at all, asked directly; without it the first pass below is
    // the fallback too, and only the second pass means anything.
    open_how asked{};
    asked.flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
    const long probe = ::syscall(SYS_openat2, AT_FDCWD, QFile::encodeName(dir).constData(), &asked, sizeof asked);
    const bool kernelHasOpenat2 = probe >= 0 || errno != ENOSYS;
    if (probe >= 0) {
        ::close(int(probe));
    }
    if (!kernelHasOpenat2) {
        notVerified("S15.68 (with openat2)", "this kernel has no openat2, so only the fallbacks can be exercised");
    }
    static_cast<void>(takeOpenat2Calls());
    static_cast<void>(takeFifoOpens());

    const QString leads = QStringLiteral("assets/video.mp4 leads outside the wallpaper's folder.");
    for (const int refused : {0, ENOSYS, EPERM}) {
        if (refused == 0 && !kernelHasOpenat2) {
            continue;
        }
        const QString how = refused == 0 ? QStringLiteral("with openat2")
                                         : QStringLiteral("openat2 refused with %1").arg(QLatin1String(refused == ENOSYS ? "ENOSYS" : "EPERM"));
        // Off again at the end of each pass and when one throws, so no later phase runs refused.
        setOpenat2Refused(refused);
        const auto unrefuse = qScopeGuard([] { setOpenat2Refused(0); });
        // GOAL: S15.68 -- a file wallpaper.json names is usable exactly when, its links followed, it
        //       lies inside the wallpaper's own folder: an asset linked to an absolute path outside,
        //       by a relative path that climbs out, through a linked folder, or to a FIFO outside is
        //       refused, "leads outside the wallpaper's folder", and that FIFO is never opened; one
        //       linked inside -- relatively, absolutely, or by a ".." out and back in -- is usable,
        //       its assetPath the name wallpaper.json gives. The same with openat2, and when it is
        //       refused with ENOSYS (an old kernel) or EPERM (a sandbox).
        // WHY-THIS-SHAPE: the relative link climbs out of the wallpaper but not the library, so a
        //       check bounded by the library would pass it. openat2_shim.cpp refuses openat2 in this
        //       process, which drives the fallback that finds the file with O_PATH and asks /proc
        //       where it is; openat2's own RESOLVE_BENEATH refuses the absolute and out-and-in links
        //       that land inside, so those go to the fallback in every pass. Whether the FIFO was
        //       opened is read from inotify's IN_OPEN on it, which a blocked writer would only show
        //       by timing.
        // CONTROL: app/library/wpx_package.cpp, openBeneath's `RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS`
        //       made `RESOLVE_NO_MAGICLINKS` -> red, "S15.68 (with openat2): abs read as usable=1", the
        //       same for rel, dirlink and abspreview, fifolink read as "not a regular file" and "the
        //       FIFO outside was opened 1 time(s)", the other passes green; the fallback's
        //       `if (folder.isEmpty() || !file.startsWith(folder == "/" ? folder : folder + '/')) {`
        //       made `if (folder.isEmpty()) {` -> red, those same lines in all three passes;
        //       `errno != EXDEV && ` dropped from `if (fd >= 0 || (errno != EXDEV && ...))` -> red,
        //       "S15.68 (with openat2): absinside read as usable=0 ... leads outside" and outin only;
        //       `&& errno != EPERM` dropped -> red, every row of the EPERM pass only, e.g. "S15.68
        //       (openat2 refused with EPERM): inside read as usable=0 problem "wallpaper.json cannot be
        //       opened: Operation not permitted.""; the whole condition made `if (true) {` -> red, every
        //       row of both refused passes, and absinside and outin with openat2; the fallback's
        //       `O_PATH | O_CLOEXEC` made `O_RDONLY | O_NONBLOCK | O_CLOEXEC` -> red, "S15.68 (...): the
        //       FIFO outside was opened 1 time(s)" in every pass and nothing else; restored.
        // COVERAGE-ASSERT: openat2 was asked for every read of each pass, so the refused passes
        //       went the other way; and an open of the FIFO by this test is seen, so the IN_OPEN
        //       watch is live (test-side control: the watch made on IN_ACCESS -> red, "this test's own
        //       open of the FIFO was seen 0 time(s)", every "opened" line still green).
        const auto expectRefused = [&](const char* name, const QString& reason) {
            const Wallpaper w = readWallpaperFolder(at(name));
            expect(isBrokenOnly(w) && w.problem == reason,
                   QStringLiteral("S15.68 (%1): %2 read as usable=%3 problem \"%4\", not \"%5\"")
                       .arg(how, QLatin1String(name)).arg(w.usable()).arg(w.problem, reason));
        };
        expectRefused("abs", leads);
        expectRefused("rel", leads);
        expectRefused("dirlink", leads);
        expectRefused("abspreview", QStringLiteral("preview.png leads outside the wallpaper's folder."));
        expectRefused("fifolink", leads);
        const int opened = takeFifoOpens();
        expect(opened == 0, QStringLiteral("S15.68 (%1): the FIFO outside was opened %2 time(s)").arg(how).arg(opened));
        for (const char* name : {"inside", "absinside", "outin"}) {
            const Wallpaper w = readWallpaperFolder(at(name));
            expect(w.usable() && w.assetPath == at(name) + QStringLiteral("/assets/video.mp4"),
                   QStringLiteral("S15.68 (%1): %2 read as usable=%3 problem \"%4\" asset path \"%5\"")
                       .arg(how, QLatin1String(name)).arg(w.usable()).arg(w.problem, w.assetPath));
        }

        // GOAL: S15.68 -- the preview provider reads a folder's preview through the same check: a
        //       preview linked outside the folder is not read, and gives no image.
        // CONTROL: the RESOLVE_BENEATH and startsWith mutations above -> red, "S15.68 (with
        //       openat2): reading abspreview's preview.png gave 111 byte(s), and the provider an
        //       image", and for the startsWith one the same in the refused passes; restored.
        const auto entry = readWallpaperEntry(at("abspreview"), QStringLiteral("preview.png"), kMaxPreviewBytes);
        PreviewImageProvider provider;
        QSize size;
        const QImage image = provider.requestImage(previewImageId(at("abspreview"), QStringLiteral("preview.png"), FileStamp{1, 2, 3, 4}), &size, QSize());
        expect(!entry && entry.error() == u"preview.png leads outside the wallpaper's folder." && image.isNull(),
               QStringLiteral("S15.68 (%1): reading abspreview's preview.png gave %2, and the provider %3")
                   .arg(how, entry ? QStringLiteral("%1 byte(s)").arg(entry->size()) : entry.error(),
                        image.isNull() ? QStringLiteral("no image") : QStringLiteral("an image")));
        const int asked = takeOpenat2Calls();
        expect(asked >= 10, QStringLiteral("S15.68 (%1): openat2 was asked %2 time(s) for ten reads").arg(how).arg(asked));
    }
    const int own = ::open(QFile::encodeName(outsideFifo).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (own >= 0) {
        ::close(own);
    }
    const int seen = takeFifoOpens();
    ::close(fifoOpens);
    expect(own >= 0 && seen == 1, QStringLiteral("S15.68: this test's own open of the FIFO was seen %1 time(s); the watch for opens is not live").arg(seen));
}

// S15.51
void turnedPreview(const QString& root) {
    // A 400x300 JPEG whose EXIF orientation (6) says to show it turned 90 degrees clockwise.
    QImage image(400, 300, QImage::Format_RGB32);
    image.fill(qRgb(200, 40, 40));
    QByteArray jpeg;
    {
        QBuffer buffer(&jpeg);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "JPEG");
    }
    const QByteArray tiff = QByteArray::fromHex("49492a0008000000" "0100" "120103000100000006000000" "00000000");
    const QByteArray payload = QByteArray("Exif\0\0", 6) + tiff;
    QByteArray app1 = QByteArray::fromHex("ffe1");
    app1.append(char((payload.size() + 2) >> 8)).append(char((payload.size() + 2) & 0xff)).append(payload);
    if (jpeg.startsWith("\xff\xd8")) {
        jpeg.insert(2, app1);
    }
    QBuffer probe(&jpeg);
    probe.open(QIODevice::ReadOnly);
    QImageReader stored(&probe);
    const QString folder = root + QStringLiteral("/turned");
    if (!jpeg.startsWith("\xff\xd8") || stored.size() != QSize(400, 300) || (stored.transformation() & QImageIOHandler::TransformationRotate90) == 0 ||
        !writeFolder(folder, {{QStringLiteral("turned.jpg"), jpeg}}).isEmpty()) {
        harnessFailure("S15.51: a JPEG turned by its EXIF orientation could not be built");
        return;
    }
    PreviewImageProvider provider;
    const QString id = previewImageId(folder, QStringLiteral("turned.jpg"), FileStamp{1, 2, 3, 4});
    // GOAL: S15.51 -- a preview turned 90 degrees by its EXIF orientation is reported at its shown
    //       size (300x400, not the stored 400x300) and decoded covering the requested box in that
    //       orientation, never enlarged.
    // WHY-THIS-SHAPE: a 160x90 box: covering the shown 300x400 gives 160x213; working in the stored
    //       orientation instead gives 160x120, which leaves the tile short, and a box larger than
    //       the image must give the whole image as shown, 300x400. The preview is in a wallpaper
    //       folder, as every preview the app shows is.
    // CONTROL: app/library/preview_image_provider.cpp, `turned ? stored.transposed() : stored`
    //       made `stored` -> red, "S15.51: a 160x90 request gave 160x120, reported 400x300";
    //       `reader.setScaledSize(turned ? target.transposed() : target);` made
    //       `reader.setScaledSize(target);` -> red, "... gave 213x160"; restored.
    QSize reported;
    const QImage cover = provider.requestImage(id, &reported, QSize(160, 90));
    QSize bigReported;
    const QImage big = provider.requestImage(id, &bigReported, QSize(4000, 4000));
    expect(cover.size() == QSize(160, 213) && reported == QSize(300, 400) && big.size() == QSize(300, 400),
           QStringLiteral("S15.51: a 160x90 request gave %1x%2, reported %3x%4; a 4000x4000 request gave %5x%6")
               .arg(cover.width()).arg(cover.height()).arg(reported.width()).arg(reported.height()).arg(big.width()).arg(big.height()));
}

// S15.45
void previewAddresses(const QString& root) {
    const QString odd = root + QStringLiteral("/odd dir %41 ?q #frag ü/wall paper%2F?#");
    FileStamp stamp{1, 2, 3, 4};
    // GOAL: S15.45 -- a preview address carries the wallpaper folder's path and the preview's name
    //       through a URL and back unchanged, as QML's image loader splits it, for paths holding
    //       '/', '%', '?', '#', spaces and non-ASCII characters.
    // WHY-THIS-SHAPE: the id is taken from the URL the way QQuickPixmapCache does,
    //       `toString(RemoveScheme | RemoveAuthority).mid(1)`, not from previewImageId() directly:
    //       the URL is where percent-decoding and '?'/'#' splitting would damage a raw path.
    // CONTROL: app/library/wpx_package.cpp, previewImageId's `QString::fromLatin1(folderPath.toUtf8().toBase64(kIdEncoding))`
    //       made the raw `folderPath` -> red, "S15.45: ... came back as nothing"; restored.
    for (const auto& [path, entry] : {std::pair{odd, QStringLiteral("previews/p ü#?%.png")},
                                      std::pair{QStringLiteral("/tmp/plain"), QStringLiteral("preview.png")}}) {
        Wallpaper w;
        w.filePath = path;
        w.fileName = QFileInfo(path).fileName();
        w.previewName = entry;
        w.previewStamp = stamp;
        const QUrl url = w.previewSource();
        const auto back = parsePreviewImageId(url.toString(QUrl::RemoveScheme | QUrl::RemoveAuthority).mid(1));
        expect(url.scheme() == u"image" && url.host() == QLatin1String(kPreviewProviderId) && back && back->folderPath == path &&
                   back->entryName == entry,
               QStringLiteral("S15.45: %1 in %2 came back as %3").arg(entry, path, back ? back->entryName + QStringLiteral(" in ") + back->folderPath : QStringLiteral("nothing")));
    }
    // GOAL: S15.45 -- an id that was not made by previewImageId gives nothing, never a path.
    // CONTROL: app/library/wpx_package.cpp, parsePreviewImageId's `if (parts.size() != 3)` made
    //       `if (parts.size() < 3)` -> red, "the id "1-2-3-4/Zm9v/Zm9v/Zm9v" gave foo in foo"; restored.
    for (const QString& garbage : {QString(), QStringLiteral("a/b"), QStringLiteral("1-2-3-4/!!!/Zm9v"), QStringLiteral("1-2-3-4//Zm9v"),
                                   QStringLiteral("1-2-3-4/Zm9v/Zm9v/Zm9v"), QStringLiteral("1-2-3-4/Zm9v")}) {
        const auto got = parsePreviewImageId(garbage);
        expect(!got, QStringLiteral("S15.45: the id \"%1\" gave %2 in %3").arg(garbage, got ? got->entryName : QString(), got ? got->folderPath : QString()));
    }

    // GOAL: S15.45 -- a wallpaper whose preview is rewritten gets a different preview address, so
    //       QML does not show a cached image of the old one; an unchanged one keeps its address; a
    //       broken one has none.
    // WHY-THIS-SHAPE: only preview.png is rewritten, in place with the same bytes, its time set two
    //       seconds on before the close and checked to have moved, so the address can change only
    //       through the preview's own stamp (not wallpaper.json's, which is untouched), and not by
    //       the clock's resolution.
    // CONTROL: app/library/wpx_package.cpp, previewImageId's `QString::number(stamp.size),
    //       QString::number(stamp.modifiedNs)` made `QStringLiteral("0"), QStringLiteral("0")` -> red,
    //       "the rewritten preview kept its address"; its `QString::number(stamp.device)` made a
    //       per-call counter -> red, "an unchanged wallpaper's address changed between reads";
    //       previewSource()'s `previewImageId(filePath, previewName, previewStamp)` made `..., stamp)`
    //       -> red, "the rewritten preview kept its address"; `if (!usable()) {` made `if (false) {`
    //       -> red, "a broken wallpaper has a preview address"; restored, green each time.
    const QString path = root + QStringLiteral("/changing");
    if (!writeFolder(path, planEntries(QByteArray(kPlanManifest))).isEmpty()) {
        harnessFailure("S15.45: the folder changing could not be written");
        return;
    }
    const QUrl first = readWallpaperFolder(path).previewSource();
    const QUrl again = readWallpaperFolder(path).previewSource();
    expect(!first.isEmpty() && first == again, QStringLiteral("S15.45: an unchanged wallpaper's address changed between reads"));
    if (!rewriteLater(path + QStringLiteral("/preview.png"), 2'000'000'000)) {
        harnessFailure("S15.45: preview.png could not be rewritten in place with the same size and a later time");
        return;
    }
    const QUrl rewritten = readWallpaperFolder(path).previewSource();
    expect(!rewritten.isEmpty() && rewritten != first,
           QStringLiteral("S15.45: the rewritten preview kept its address %1 (now \"%2\")").arg(first.toString(), rewritten.toString()));
    Wallpaper broken;
    broken.filePath = path;
    broken.previewName = QStringLiteral("preview.png");
    broken.problem = QStringLiteral("x");
    expect(broken.previewSource().isEmpty(), QStringLiteral("S15.45: a broken wallpaper has a preview address"));

    // GOAL: S15.45 -- the provider decodes a folder's preview scaled down to cover the requested box
    //       and never enlarges it; it reports the full size, and an address it cannot serve gives a
    //       null image.
    // WHY-THIS-SHAPE: a square 100x100 box against a 16:9 image separates "cover" (178x100) from
    //       "fit" (100x56); a 4000x4000 request checks the no-enlarge rule.
    // CONTROL: app/library/preview_image_provider.cpp, `Qt::KeepAspectRatioByExpanding` made
    //       `Qt::KeepAspectRatio` -> red, "a 100x100 request gave 100x56"; `if (target.width() <
    //       full.width())` made `if (true)` -> red, "a 4000x4000 request gave 7111x4000"; the
    //       width-only box's height made `box.width()` -> red, "a width-only 320 request gave 568x320";
    //       a failed read made to return a 1x1 image -> red, "an address the provider cannot serve
    //       gave an image"; restored, green each time.
    PreviewImageProvider provider;
    const QString id = readWallpaperFolder(path).previewSource().toString(QUrl::RemoveScheme | QUrl::RemoveAuthority).mid(1);
    const auto request = [&](QSize box) {
        QSize size;
        const QImage image = provider.requestImage(id, &size, box);
        return std::pair{image.size(), size};
    };
    const auto [cover, fullSize] = request(QSize(100, 100));
    // QSize::scaled truncates, so the covering width is 177 or 178 depending on Qt's rounding.
    expect(cover.height() == 100 && cover.width() >= 177 && cover.width() <= 178 && fullSize == QSize(1280, 720),
           QStringLiteral("S15.45: a 100x100 request gave %1x%2 (full size reported %3x%4), not about 178x100 of 1280x720")
               .arg(cover.width()).arg(cover.height()).arg(fullSize.width()).arg(fullSize.height()));
    const auto [big, bigFull] = request(QSize(4000, 4000));
    expect(big == QSize(1280, 720), QStringLiteral("S15.45: a 4000x4000 request gave %1x%2, not the full 1280x720").arg(big.width()).arg(big.height()));
    const auto [width, widthFull] = request(QSize(320, 0));
    expect(width == QSize(320, 180), QStringLiteral("S15.45: a width-only 320 request gave %1x%2").arg(width.width()).arg(width.height()));
    QSize none;
    expect(provider.requestImage(QStringLiteral("garbage"), &none, QSize()).isNull() &&
               provider.requestImage(previewImageId(root + QStringLiteral("/absent"), QStringLiteral("preview.png"), stamp), &none, QSize()).isNull(),
           QStringLiteral("S15.45: an address the provider cannot serve gave an image"));
}

// S15.47
void defaultDirectory(const QString& root) {
    // GOAL: S15.47 -- the library folder is $XDG_DATA_HOME/wallpaperx, and the wallpaper folders
    //       live in its wallpapers/, so a test or a user with a moved data folder never reads
    //       ~/.local/share.
    // CONTROL: app/library/wpx_package.cpp, `GenericDataLocation` made `HomeLocation` -> red,
    //       "defaultLibraryDirectory() is /home/.../wallpaperx"; wallpapersDirectory's
    //       `QStringLiteral("/wallpapers")` made `QStringLiteral("/library")` -> red,
    //       "wallpapersDirectory() of it is …/library"; restored, green.
    const QString want = root + QStringLiteral("/XDG_DATA_HOME/wallpaperx");
    expect(defaultLibraryDirectory() == want && wallpapersDirectory(defaultLibraryDirectory()) == want + QStringLiteral("/wallpapers"),
           QStringLiteral("S15.47: defaultLibraryDirectory() is %1 and wallpapersDirectory() of it is %2, not %3 and its wallpapers/")
               .arg(defaultLibraryDirectory(), wallpapersDirectory(defaultLibraryDirectory()), want));
}

// S15.354
void leaseDoesNotBlock(const QString& root) {
    // GOAL: S15.354 -- opening a file that another process holds a write lease on returns at once
    //       (refused, EWOULDBLOCK) rather than waiting for the lease to be broken: these opens run on the
    //       app's GUI thread, and a lease holder may keep it for up to the kernel's 45 s.
    // WHY-THIS-SHAPE: a child process takes the lease and holds it until told to let go or 8 s pass,
    //       ignoring the break signal; an open that waits for the lease therefore returns only when the
    //       child ends it, far past the 2 s bound, and one that does not returns at once. A host that
    //       will not give the child a lease is reported not verified.
    // CONTROL: `| O_NONBLOCK` deleted from the reopen in openRegularFile (app/library/wpx_package.cpp)
    //       -> "FAIL: S15.354: opening a file under another process's write lease returned after 8000 ms,
    //       opened". Via control_batch.sh, --phase '^leaseDoesNotBlock$', 2026-10-06. Reverted.
    const QString path = root + QStringLiteral("/leased.wpx");
    int ready[2] = {-1, -1};
    int release[2] = {-1, -1};
    if (!writeBytes(path, "x") || ::pipe2(ready, O_CLOEXEC) != 0 || ::pipe2(release, O_CLOEXEC) != 0) {
        expect(false, QStringLiteral("S15.354: the leased file or the pipes could not be made"));
        return;
    }
    const QByteArray encoded = QFile::encodeName(path);
    const pid_t holder = ::fork();
    if (holder == 0) {
        ::signal(SIGIO, SIG_IGN);
        const int fd = ::open(encoded.constData(), O_RDWR);
        const char leased = fd >= 0 && ::fcntl(fd, F_SETLEASE, F_WRLCK) == 0 ? '1' : '0';
        static_cast<void>(::write(ready[1], &leased, 1));
        pollfd until{release[0], POLLIN, 0};
        static_cast<void>(::poll(&until, 1, 8000));
        ::_exit(0);
    }
    ::close(ready[1]);
    ::close(release[0]);
    // The holder may have given up already (an open that waited for it), so its end of the pipe can be
    // gone: EPIPE, not SIGPIPE, or the run would die before saying what it saw.
    const auto reap = [&] {
        const auto previous = ::signal(SIGPIPE, SIG_IGN);
        static_cast<void>(::write(release[1], "x", 1));
        ::signal(SIGPIPE, previous);
        ::close(release[1]);
        ::close(ready[0]);
        int status = 0;
        if (holder > 0) ::waitpid(holder, &status, 0);
    };
    char leased = '0';
    pollfd answer{ready[0], POLLIN, 0};
    if (holder < 0 || ::poll(&answer, 1, 5000) != 1 || ::read(ready[0], &leased, 1) != 1 || leased != '1') {
        reap();
        notVerified("S15.354", "this host would not give a child process a write lease on a scratch file");
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    const auto opened = openRegularFile(AT_FDCWD, encoded.constData(), O_RDONLY);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    const int error = opened ? 0 : opened.error();
    reap();
    expect(ms < 2000 && !opened && error == EWOULDBLOCK,
           QStringLiteral("S15.354: opening a file under another process's write lease returned after %1 ms, %2")
               .arg(ms)
               .arg(opened ? QStringLiteral("opened") : QString::fromLocal8Bit(std::strerror(error))));
}

} // namespace

int main(int argc, char* argv[]) {
    wallpaperx::testing::g_testName = "wpx_package";
    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        notVerified("S15.31-S15.35, S15.45, S15.47, S15.48, S15.50, S15.51, S15.53, S15.55, S15.67, S15.68", "no temporary folder could be made");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    isolateXdg(scratch.path());
    QCoreApplication app(argc, argv);

    const QString root = scratch.path();
    runPhase("manifestOrderAndFields", manifestOrderAndFields);
    runPhase("malformedManifests", malformedManifests);
    runPhase("daemonPropertyRules", daemonPropertyRules);
    runPhase("acceptValues", acceptValues);
    runPhase("badArchives", [&] { badArchives(root); });
    runPhase("scanFolder", [&] { scanFolder(root); });
    runPhase("folderNamesThatAreNotUtf8", [&] { folderNamesThatAreNotUtf8(root); });
    runPhase("sharedValidator", [&] { sharedValidator(root); });
    runPhase("containment", [&] { containment(root); });
    runPhase("turnedPreview", [&] { turnedPreview(root); });
    runPhase("previewAddresses", [&] { previewAddresses(root); });
    runPhase("defaultDirectory", [&] { defaultDirectory(root); });
    runPhase("leaseDoesNotBlock", [&] { leaseDoesNotBlock(root); });
    return verdict();
}
