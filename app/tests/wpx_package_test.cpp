// SPDX-License-Identifier: GPL-2.0-or-later

// app/library/wpx_package.cpp and app/library/preview_image_provider.cpp, with no bus and no
// event loop: one wallpaper.json, one .wpx archive, one wallpaper folder, one library folder, one
// preview address at a time. Goals S15.31-S15.35, S15.45, S15.47, S15.48, S15.50, S15.51, S15.53,
// S15.55, S15.67, S15.68, S15.221-S15.225 in GOALS.md; the last graded against the daemon's own
// property rules (daemon_rules.cpp).

#include "audio_probe.h"
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

// What the daemon makes of a wallpaper.json's properties as declared: its own propertiesError() on
// every default, and on a "volume"'s min and max, the two ends its slider can reach. Empty: accepted.
std::string daemonVerdict(const OrderedJson& properties) {
    std::vector<std::pair<std::string, DaemonValue>> defaults;
    std::string extremes;
    for (const auto& [key, spec] : properties.items()) {
        const std::string type = spec.value("type", "");
        if (type == "float") {
            defaults.emplace_back(key, spec.at("default").get<double>());
            if (key == "volume") {
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

DaemonValue toDaemon(const QVariant& value) {
    if (value.typeId() == QMetaType::Bool) {
        return value.toBool();
    }
    if (value.typeId() == QMetaType::Double) {
        return value.toDouble();
    }
    return value.toString().toStdString();
}

// What the daemon makes of everything the app can send for a usable wallpaper: the whole set of
// defaults as the app stored them, then that set with each offered choice in its property's place,
// so rules on the whole set (the property count) apply to every probe. Empty: all accepted.
std::string daemonVerdictOnOffered(const QList<WallpaperProperty>& properties) {
    std::vector<std::pair<std::string, DaemonValue>> set;
    for (const WallpaperProperty& property : properties) {
        set.emplace_back(property.key.toStdString(), toDaemon(property.defaultValue));
    }
    if (std::string why = daemonPropertiesError(set); !why.empty()) {
        return why;
    }
    for (std::size_t i = 0; i < set.size(); ++i) {
        const DaemonValue stored = set[i].second;
        for (const double choice : properties[static_cast<qsizetype>(i)].choices()) {
            set[i].second = choice;
            if (std::string why = daemonPropertiesError(set); !why.empty()) {
                return why + " (the choice " + std::to_string(choice) + ")";
            }
        }
        set[i].second = stored;
    }
    return {};
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
    // GOAL: S15.223 -- "speed" is refused, naming its declared range, exactly when no playback speed
    //       (0.5, 1 or 2) lies inside that range; a range holding at least one is usable, however far
    //       it reaches past them; "Speed" is another key and is not restricted.
    // GOAL: S15.224 -- a "speed" that is not a float lists broken. ("mute" and "volume" are judged where
    //       the folder is read, knowing whether they are kept: soundPropertyRules.)
    // WHY-THIS-SHAPE: each refusal is paired with the usable case on the other side of its boundary
    //       (64/65 characters and properties; a range ending at 0.5 or starting at 2, inclusive, beside
    //       ones just short of them, 0.4999 and 2.0001), so neither a
    //       validator that refuses too much nor one that refuses too little passes; each refusal's
    //       sentence is matched, so a refusal for some other reason is red.
    // CONTROL: app/library/wpx_package.cpp, each through control_batch, 2026-10-02 (the first re-run
    //       2026-10-04, inside anchoredPattern): isPropertyKey's
    //       `.arg(kMaxPropertyKeyBytes)` made `.arg(kMaxPropertyKeyBytes + 1)` -> red, "S15.221: a
    //       65-character key was accepted"; `[A-Za-z0-9_.-]` made `[ -~]` -> red, "S15.221: the key
    //       "my key" was accepted"; `> kMaxProperties` made `> kMaxProperties + 1` -> red, "S15.222: 65
    //       properties was accepted"; the not-a-float test made `if (false)` -> red, "S15.224: a bool
    //       speed was refused with "...allows 0 to 0, ...", not "...is not a float..."" (the empty
    //       choices catch it, with the wrong sentence). S15.223, control_batch 2026-10-14: choices()'s
    //       `speed >= minimum && speed <= maximum` made `speed > minimum && speed < maximum` -> red,
    //       "S15.223: speed from 0.1 to 0.5 was refused"; the `offered.isEmpty()` refusal made `false` (and
    //       the snap skipped when nothing is offered), re-run control_batch 2026-10-15 -> red, "S15.223:
    //       speed from 0.6 to 0.9 was accepted". The refusal's speed list is playbackSpeedsText("or")
    //       (shared/playback_speed.h): its `std::format(" {} ", last)` made `", {} "` (control_batch
    //       2026-10-15) -> red, "S15.223: speed from 0.6 to 0.9 was refused with "...0.5, 1, or 2 times
    //       its speed.", not ...", and the 0.4999 and 2.0001 rows.
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
        {"S15.223", "speed from 0.1 to 2", one("speed", spec(R"({"type": "float", "default": 1, "min": 0.1, "max": 2})")), nullptr},
        {"S15.223", "speed from -1 to 3", one("speed", spec(R"({"type": "float", "default": 1, "min": -1, "max": 3})")), nullptr},
        {"S15.223", "speed from 0.1 to 0.5", one("speed", spec(R"({"type": "float", "default": 0.5, "min": 0.1, "max": 0.5})")), nullptr},
        {"S15.223", "speed from 0.1 to 0.4999", one("speed", spec(R"({"type": "float", "default": 0.2, "min": 0.1, "max": 0.4999})")),
         "Property \"speed\" in wallpaper.json allows 0.1 to 0.4999, and WallpaperX plays video at 0.5, 1 or 2 times its speed."},
        {"S15.223", "speed from 0.6 to 0.9", one("speed", spec(R"({"type": "float", "default": 0.75, "min": 0.6, "max": 0.9})")),
         "Property \"speed\" in wallpaper.json allows 0.6 to 0.9, and WallpaperX plays video at 0.5, 1 or 2 times its speed."},
        {"S15.223", "speed from 2 to 5", one("speed", spec(R"({"type": "float", "default": 2, "min": 2, "max": 5})")), nullptr},
        {"S15.223", "speed from 2.0001 to 5", one("speed", spec(R"({"type": "float", "default": 3, "min": 2.0001, "max": 5})")),
         "Property \"speed\" in wallpaper.json allows 2.0001 to 5, and WallpaperX plays video at 0.5, 1 or 2 times its speed."},
        {"S15.223", "\"Speed\" from 0 to 5 at 0.3", one("Speed", spec(R"({"type": "float", "default": 0.3, "min": 0, "max": 5})")), nullptr},
        {"S15.224", "a bool speed", one("speed", flag), "Property \"speed\" in wallpaper.json is not a float, and WallpaperX reads a video's speed as one."},
        {"S15.224", "a colour speed", one("speed", spec(R"({"type": "color", "default": "#ffffff"})")),
         "Property \"speed\" in wallpaper.json is not a float, and WallpaperX reads a video's speed as one."},
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
    // GOAL: S15.225 -- everything the app can send for a usable wallpaper passes the daemon's own
    //       propertiesError() (backend/daemon/wallpaper_properties.cpp, compiled in): every default as
    //       the app stored it, and every choice it offers; and a wallpaper the app lists broken is one
    //       whose declared defaults the daemon refuses too.
    // WHY-THIS-SHAPE: graded against the daemon's code rather than this table, so a rule changed on
    //       either side alone is red here even where the table above was changed to match. The usable
    //       side is judged on the parsed values, not the declared ones: a declared speed default
    //       between the choices is one the app moves, so the daemon refusing the declared 0.75 is right.
    // CONTROL: app/library/wpx_package.cpp isPropertyKey's `.arg(kMaxPropertyKeyBytes + 1)`
    //       (control_batch, 2026-10-04) -> red, "S15.225: a 65-character key: the app lists it usable,
    //       the daemon would refuse it (property name "kkk..." must be 1 to 64 ASCII letters, digits,
    //       '_', '-' or '.')"; each break above gave its own S15.225 line beside its row's. The count
//       (the whole set is sent, so the daemon's 64-property rule applies): `> kMaxProperties` made
//       `> kMaxProperties + 1` (control_batch, 2026-10-15) -> red, "S15.225: 65 properties: the app
//       lists it usable, the daemon would refuse it (at most 64 properties, got 65)". Choices:
    //       both sides read kPlaybackSpeeds from shared/playback_speed.h, so the list cannot differ; what can
    //       is choices() (app/library/wpx_package.cpp) offering a value off it. Its `within.append(speed)`
    //       made `within.append(speed == 2.0 ? 1.5 : speed)` (control_batch 2026-10-15) -> red, "S15.225:
    //       speed from 0.1 to 2: the app lists it usable, the daemon would refuse it (property speed must be
    //       0.5, 1 or 2 (the choice 1.500000))".
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
        const std::string daemon = parsed ? daemonVerdictOnOffered(parsed->properties) : daemonVerdict(row.properties);
        expect(parsed.has_value() == daemon.empty(),
               QStringLiteral("S15.225: %1: the app lists it %2, the daemon would %3")
                   .arg(what, parsed ? QStringLiteral("usable") : QStringLiteral("broken"),
                        daemon.empty() ? QStringLiteral("accept it") : QStringLiteral("refuse it (%1)").arg(QString::fromStdString(daemon))));
    }
    expect(outside == 30 && checked == 22 + outside,
           QStringLiteral("S15.221-S15.225: %1 rows were checked, %2 of them characters outside the rule; expected 52 and 30").arg(checked).arg(outside));
}

// S15.223, S15.225
void speedChoices() {
    struct Row {
        const char* what;
        const char* key;
        const char* spec;
        QList<double> choices;
        double stored; // the default the app keeps; NaN: the wallpaper is refused
    };
    const double refused = std::numeric_limits<double>::quiet_NaN();
    // GOAL: S15.223 -- a "speed" offers exactly the playback speeds inside its declared range, ends
    //       included; a declared default inside the range is stored as the nearest offered speed by
    //       ratio (0.74 -> 1, 1.45 -> 2, 0.8 -> 0.5 when 1 is not offered); a default outside the range
    //       is refused even when it is a playback speed; any other key offers no choices.
    // WHY-THIS-SHAPE: 0.74 and 1.45 are where nearest-by-difference and nearest-by-ratio disagree, so
    //       a snap by difference is red; 0.8 in a range offering only 0.5 is red for a snap to the
    //       nearest of all three rather than of those offered.
    // CONTROL: control_batch 2026-10-15, each alone, reverted. In parseProperty (app/library/wpx_package.cpp)
    //       `nearestPlaybackSpeed(value.toDouble(), among)` made `nearestPlaybackSpeed(value.toDouble(),
    //       kPlaybackSpeeds)` -> red, "S15.223: speed default 0.8 in 0.1 to 0.9 offers [] and stores Property
    //       "speed" in wallpaper.json has a default outside its min and max."; the snap's range test deleted
    //       -> red, "S15.223: speed default 2 in 0.5 to 1 offers [0.5, 1] and stores 1, not refused for its
    //       default". In nearestPlaybackSpeed (shared/playback_speed.h) `std::fabs(std::log(from / to))` made
    //       `std::fabs(from - to)` -> red, "S15.223: speed default 0.74 in 0.1 to 2 offers [0.5, 1, 2] and
    //       stores 0.5", the 0.75 row likewise, and "speed default 1.45 ... stores 1".
    // COVERAGE-ASSERT: the row count.
    const Row rows[] = {
        {"speed default 1 in 0.1 to 2", "speed", R"({"type": "float", "default": 1, "min": 0.1, "max": 2})", {0.5, 1.0, 2.0}, 1.0},
        {"speed default 0.75 in 0.1 to 2", "speed", R"({"type": "float", "default": 0.75, "min": 0.1, "max": 2})", {0.5, 1.0, 2.0}, 1.0},
        {"speed default 0.74 in 0.1 to 2", "speed", R"({"type": "float", "default": 0.74, "min": 0.1, "max": 2})", {0.5, 1.0, 2.0}, 1.0},
        {"speed default 1.45 in 0.1 to 2", "speed", R"({"type": "float", "default": 1.45, "min": 0.1, "max": 2})", {0.5, 1.0, 2.0}, 2.0},
        {"speed default 0.5 in 0.5 to 1", "speed", R"({"type": "float", "default": 0.5, "min": 0.5, "max": 1})", {0.5, 1.0}, 0.5},
        {"speed default 0.8 in 0.1 to 0.9", "speed", R"({"type": "float", "default": 0.8, "min": 0.1, "max": 0.9})", {0.5}, 0.5},
        {"speed default 1.6 in 1.5 to 3", "speed", R"({"type": "float", "default": 1.6, "min": 1.5, "max": 3})", {2.0}, 2.0},
        {"speed default 2 in 0.5 to 1", "speed", R"({"type": "float", "default": 2, "min": 0.5, "max": 1})", {0.5, 1.0}, refused},
        {"zoom default 0.75 in 0.1 to 2", "zoom", R"({"type": "float", "default": 0.75, "min": 0.1, "max": 2})", {}, 0.75},
    };
    const auto list = [](const QList<double>& values) {
        QStringList out;
        for (const double v : values) {
            out.append(QString::number(v));
        }
        return u'[' + out.join(QStringLiteral(", ")) + u']';
    };
    int checked = 0;
    for (const Row& row : rows) {
        ++checked;
        const auto parsed = parseManifest(edited([&](OrderedJson& j) {
            j["properties"] = OrderedJson::object();
            j["properties"][row.key] = OrderedJson::parse(row.spec);
        }));
        const WallpaperProperty* property = parsed && parsed->properties.size() == 1 ? &parsed->properties[0] : nullptr;
        if (std::isnan(row.stored)) {
            expect(!parsed && parsed.error().endsWith(u"has a default outside its min and max."),
                   QStringLiteral("S15.223: %1 offers %2 and stores %3, not refused for its default")
                       .arg(QLatin1String(row.what), property ? list(property->choices()) : QStringLiteral("[]"),
                            property ? property->defaultValue.toString() : (parsed ? QString() : parsed.error())));
            continue;
        }
        expect(property && property->choices() == row.choices && property->defaultValue == QVariant(row.stored),
               QStringLiteral("S15.223: %1 offers %2 and stores %3")
                   .arg(QLatin1String(row.what), property ? list(property->choices()) : QStringLiteral("[]"),
                        property ? property->defaultValue.toString() : (parsed ? QString() : parsed.error())));
        if (parsed) {
            const std::string daemon = daemonVerdictOnOffered(parsed->properties);
            expect(daemon.empty(), QStringLiteral("S15.225: %1: the app offers what the daemon would refuse (%2)")
                                       .arg(QLatin1String(row.what), QString::fromStdString(daemon)));
        }
    }
    expect(checked == 9, QStringLiteral("S15.223: %1 rows were checked, not 9").arg(checked));

    // GOAL: S15.640 -- a declared speed default of 0 or below, inside its range, is stored as 1 when 1 is
    //       offered: what the daemon plays for a speed that is not positive (config_store's "0 snaps to 1").
    //       Where 1 is not offered it is still snapped among those offered: 0.5 for a range up to 0.6.
    // WHY-THIS-SHAPE: a nearest-by-ratio search over a non-positive default takes the log of a negative
    //       ratio, every comparison is false and the first offered speed, 0.5, wins; so 1 is asserted
    //       literally, not as "some offered speed", which 0.5 also is.
    // CONTROL: control_batch 2026-10-15, each alone, reverted. parseProperty's call (app/library/wpx_package.cpp)
    //       made the earlier `min_element(among, ..., fabs(log(declared / s)))` -> red, "S15.640: speed
    //       default 0 in -1 to 3 offers [0.5, 1, 2] and stores 0.5, not 1", the -2 and -0.5 rows likewise.
    //       In nearestPlaybackSpeed (shared/playback_speed.h) the non-positive guard made `from = speed` ->
    //       the same three; a `return 1.0` for a non-positive speed before the snap -> red, "S15.640: speed
    //       default -1 in -2 to 0.6 offers [] and stores Property "speed" ... outside its min and max., not 0.5".
    // COVERAGE-ASSERT: the row count.
    const Row nonPositive[] = {
        {"speed default 0 in -1 to 3", "speed", R"({"type": "float", "default": 0, "min": -1, "max": 3})", {0.5, 1.0, 2.0}, 1.0},
        {"speed default -2 in -5 to 2", "speed", R"({"type": "float", "default": -2, "min": -5, "max": 2})", {0.5, 1.0, 2.0}, 1.0},
        {"speed default -0.5 in -1 to 1", "speed", R"({"type": "float", "default": -0.5, "min": -1, "max": 1})", {0.5, 1.0}, 1.0},
        {"speed default -1 in -2 to 0.6", "speed", R"({"type": "float", "default": -1, "min": -2, "max": 0.6})", {0.5}, 0.5},
    };
    int nonPositiveChecked = 0;
    for (const Row& row : nonPositive) {
        ++nonPositiveChecked;
        const auto parsed = parseManifest(edited([&](OrderedJson& j) {
            j["properties"] = OrderedJson::object();
            j["properties"][row.key] = OrderedJson::parse(row.spec);
        }));
        const WallpaperProperty* property = parsed && parsed->properties.size() == 1 ? &parsed->properties[0] : nullptr;
        expect(property && property->choices() == row.choices && property->defaultValue == QVariant(row.stored),
               QStringLiteral("S15.640: %1 offers %2 and stores %3, not %4")
                   .arg(QLatin1String(row.what), property ? list(property->choices()) : QStringLiteral("[]"),
                        property ? property->defaultValue.toString() : (parsed ? QString() : parsed.error()))
                   .arg(row.stored));
    }
    expect(nonPositiveChecked == 4, QStringLiteral("S15.640: %1 rows were checked, not 4").arg(nonPositiveChecked));
}

// S15.35, S15.53
void acceptValues() {
    WallpaperProperty zoom;
    zoom.key = QStringLiteral("zoom");
    zoom.type = WallpaperProperty::Type::Float;
    zoom.minimum = 0.1;
    zoom.maximum = 3.0;
    WallpaperProperty speed = zoom;
    speed.key = QStringLiteral("speed");
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
        {"a float at exactly min", zoom, 0.1, QVariant(0.1)},
        {"a float at exactly max", zoom, 3.0, QVariant(3.0)},
        {"a float between min and max", zoom, 0.75, QVariant(0.75)},
        {"an int", zoom, 2, QVariant(2.0)},
        {"a float just below min", zoom, std::nextafter(0.1, 0.0), std::nullopt},
        {"a float just above max", zoom, std::nextafter(3.0, 4.0), std::nullopt},
        {"NaN", zoom, std::numeric_limits<double>::quiet_NaN(), std::nullopt},
        {"infinity", zoom, std::numeric_limits<double>::infinity(), std::nullopt},
        {"the string \"1\" for a float", zoom, QStringLiteral("1"), std::nullopt},
        {"a bool for a float", zoom, true, std::nullopt},
        {"speed 0.5", speed, 0.5, QVariant(0.5)},
        {"speed 1", speed, 1.0, QVariant(1.0)},
        {"speed as the int 2", speed, 2, QVariant(2.0)},
        {"speed 0.75", speed, 0.75, std::nullopt},
        {"speed one ulp above 1", speed, std::nextafter(1.0, 2.0), std::nullopt},
        {"speed at its min 0.1", speed, 0.1, std::nullopt},
        {"speed at its max 3", speed, 3.0, std::nullopt},
        {"true for speed", speed, true, std::nullopt},
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
    //       text "#aarrggbb" still refused. A float with choices (speed) takes only one of them,
    //       even in range: not 0.75, not its own min or max, not one ulp off 1.
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
    //       refusal"; restored, green each time. The choice rule, control_batch 2026-10-14: accept()'s
    //       `!offered.contains(number)` made `false` -> red, "S15.35: speed 0.75 was accepted as 0.75".
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
    // The whole pair carries no sound property: a folder's video is given or stripped of them by what
    // the probe hears (S15.473), which an archive is not, so only a manifest without them reads the same.
    OrderedJson soundlessJson = planManifest();
    soundlessJson["properties"].erase("mute");
    const QByteArray soundless = jsonBytes(soundlessJson);
    Entries noPreview = planEntries(plan);
    noPreview.removeAt(1);
    QByteArray huge = plan;
    huge.append(QByteArray(kMaxManifestBytes + 1 - plan.size(), ' '));
    QByteArray atCap = plan;
    atCap.append(QByteArray(kMaxManifestBytes - plan.size(), ' '));
    if (!writeFolder(dir + QStringLiteral("/nopreview"), noPreview).isEmpty() ||
        !writeZip(dir + QStringLiteral("/nopreview.wpx"), noPreview).isEmpty() ||
        !writeFolder(dir + QStringLiteral("/whole"), planEntries(soundless)).isEmpty() ||
        !writeZip(dir + QStringLiteral("/whole.wpx"), planEntries(soundless)).isEmpty() ||
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
    //       as usable=1 ..." and "... the archive missing its preview read as usable=1 ..." (control_batch,
    //       re-run 2026-10-11 with the soundless whole pair); restored. The whole-pair line has no control
    //       of its own.
    const Wallpaper folder = readWallpaperFolder(dir + QStringLiteral("/nopreview"));
    const Wallpaper archive = readArchive(dir + QStringLiteral("/nopreview.wpx"));
    const Wallpaper wholeFolder = readWallpaperFolder(dir + QStringLiteral("/whole"));
    const Wallpaper wholeArchive = readArchive(dir + QStringLiteral("/whole.wpx"));
    expect(isBrokenOnly(folder) && folder.problem == u"The folder has no preview.png, which wallpaper.json names.",
           QStringLiteral("S15.67: the folder missing its preview read as usable=%1 problem \"%2\"").arg(folder.usable()).arg(folder.problem));
    expect(isBrokenOnly(archive) && archive.problem == u"The archive has no preview.png, which wallpaper.json names.",
           QStringLiteral("S15.67: the archive missing its preview read as usable=%1 problem \"%2\"").arg(archive.usable()).arg(archive.problem));
    expect(wholeFolder.usable() && wholeArchive.usable() && wholeFolder.properties == wholeArchive.properties &&
               wholeFolder.properties.size() == 2 && wholeFolder.title == wholeArchive.title,
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

QString nameOf(AudioStreams audio) {
    switch (audio) {
    case AudioStreams::Unknown:
        return QStringLiteral("Unknown");
    case AudioStreams::Absent:
        return QStringLiteral("Absent");
    case AudioStreams::Present:
        return QStringLiteral("Present");
    }
    return QStringLiteral("(not an AudioStreams)");
}

// S15.472, S15.487
void audioProbe(const QString& root) {
    const QString dir = root + QStringLiteral("/probe");
    struct Case {
        const char* what;
        QString file;
        QByteArray bytes;
        AudioStreams audio;
    };
    const QByteArray listing("ffconcat version 1.0\nfile listed.wav\n");
    const Case cases[] = {
        {"a WAV file (audio declared in its header)", QStringLiteral("wav"), wavAudio(), AudioStreams::Present},
        {"an FLV video whose audio follows its video", QStringLiteral("flv-sound"), flvVideo(true), AudioStreams::Present},
        {"the same FLV video without the audio", QStringLiteral("flv-silent"), flvVideo(false), AudioStreams::Absent},
        {"a Y4M video", QStringLiteral("y4m"), y4mVideo(), AudioStreams::Absent},
        {"a file that is not media", QStringLiteral("junk"), QByteArrayLiteral("not really a video"), AudioStreams::Unknown},
        {"an ffconcat script listing a WAV file beside it", QStringLiteral("listing"), listing, AudioStreams::Unknown},
    };
    const QString cwd = QDir::currentPath();
    if (!QDir().mkpath(dir) || !writeBytes(dir + QStringLiteral("/listed.wav"), wavAudio())) {
        harnessFailure("S15.472: the probe's folder could not be made");
        return;
    }
    // FFmpeg is given no file name, so a name a container lists is looked up in the working directory.
    if (::chdir(QFile::encodeName(dir).constData()) != 0) {
        harnessFailure("S15.487: the working directory could not be moved to the listing's folder");
        return;
    }
    const auto back = qScopeGuard([&cwd] { static_cast<void>(::chdir(QFile::encodeName(cwd).constData())); });
    // GOAL: S15.472 -- the app hears a video's sound by an audio stream it can decode: a WAV file's
    //       header-declared stream and an FLV video's audio found only by reading its packets are Present;
    //       the same FLV without the audio tag and a Y4M video are Absent; a file FFmpeg cannot open as media
    //       is Unknown, not Absent. The probe reads from the file's start wherever the descriptor stands.
    // GOAL: S15.487 -- the probe opens no file a container names: an ffconcat script listing a real WAV
    //       file in the working directory is Unknown, never Present.
    // WHY-THIS-SHAPE: each descriptor is left at the file's end before the probe, so a probe that read
    //       from the offset it was given would see nothing; the FLV pair differs only by the audio tag, and
    //       is large enough for FFmpeg to recognise without it. The listed file is a WAV the working
    //       directory holds, so a probe that followed the listing would find its audio.
    // CONTROL: via control_batch.sh, --phase '^audioProbe$', 2026-10-11, each alone, reverted, in
    //       app/library/audio_probe.cpp: the AVFMTCTX_NOHEADER test made `if (true)` -> "FAIL: S15.472: an FLV
    //       video whose audio follows its video probed as Absent, not Present", alone; the `lseek(fd, 0,
    //       SEEK_SET)` deleted -> the WAV, both FLVs and the Y4M "probed as Unknown"; the audio-type test
    //       dropped from holdsDecodableAudio -> the Y4M and the silent FLV "probed as Present"; the packet
    //       loop's EOF answered Unknown -> the silent FLV "probed as Unknown, not Absent", alone; a refused
    //       open answered Absent -> the non-media file and the listing "probed as Absent, not Unknown";
    //       readOnlyThrough() replaced by bare custom I/O -> "FAIL: S15.487: an ffconcat script listing a WAV
    //       file beside it probed as Present, not Unknown", alone.
    int probed = 0;
    for (const Case& c : cases) {
        const QString path = dir + QLatin1Char('/') + c.file;
        if (!writeBytes(path, c.bytes)) {
            harnessFailure("S15.472: a probe fixture could not be written");
            return;
        }
        const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_CLOEXEC);
        if (fd < 0 || ::lseek(fd, 0, SEEK_END) < 0) {
            harnessFailure("S15.472: a probe fixture could not be opened");
            if (fd >= 0) {
                ::close(fd);
            }
            return;
        }
        const AudioStreams heard = probeAudioStreams(fd);
        ::close(fd);
        ++probed;
        expect(heard == c.audio, QStringLiteral("%1: %2 probed as %3, not %4")
                                     .arg(c.file == u"listing" ? QStringLiteral("S15.487") : QStringLiteral("S15.472"),
                                          QLatin1StringView(c.what), nameOf(heard), nameOf(c.audio)));
    }
    expect(probed == 6, QStringLiteral("S15.472: %1 probe cases ran, not 6").arg(probed));
}

// S15.473
void soundProperties(const QString& root) {
    const QString dir = root + QStringLiteral("/sound");
    const auto write = [&](const char* name, const QByteArray& manifest, const QByteArray& asset) {
        return writeFolder(dir + QLatin1Char('/') + QLatin1StringView(name), planEntriesWith(manifest, asset)).isEmpty();
    };
    OrderedJson undeclared = planManifest();
    undeclared["properties"].erase("mute");
    OrderedJson declared = planManifest();
    declared["properties"]["volume"] = OrderedJson::parse(R"({"type": "float", "default": 0.25, "min": 0, "max": 0.5, "label": "Loudness"})");
    OrderedJson still = declared;
    still["type"] = "static";
    still["file"] = "assets/video.mp4";
    OrderedJson full = planManifest();
    full["properties"] = OrderedJson::object();
    for (int i = 0; i < int(kMaxProperties); ++i) {
        full["properties"]["k" + std::to_string(i)] = OrderedJson::parse(R"({"type": "bool", "default": true})");
    }
    if (!write("added", jsonBytes(undeclared), soundVideo()) || !write("declared", jsonBytes(declared), soundVideo()) ||
        !write("silent", jsonBytes(declared), silentVideo()) || !write("still", jsonBytes(still), soundVideo()) ||
        !write("full", jsonBytes(full), soundVideo())) {
        harnessFailure("S15.473: the wallpaper folders could not be written");
        return;
    }
    const auto at = [&](const char* name) { return readWallpaperFolder(dir + QLatin1Char('/') + QLatin1StringView(name)); };
    const auto find = [](const Wallpaper& w, const char* key) -> const WallpaperProperty* {
        const auto it = std::ranges::find(w.properties, QLatin1StringView(key), &WallpaperProperty::key);
        return it == w.properties.end() ? nullptr : &*it;
    };

    // GOAL: S15.473 -- a video folder whose video has sound offers `mute` (bool, default false) and
    //       `volume` (float 0 to 1, default 1) after its declared properties when wallpaper.json declares
    //       neither; declared ones keep their own default, range and label, and nothing is added twice.
    //       A silent video, and a still, list neither, a declared one included, and keep every other
    //       property. With wallpaper.json at the 64-property cap nothing is added.
    // WHY-THIS-SHAPE: the same declared manifest read over a video with sound, a silent video and as a
    //       still, so only the probe and the type decide; the cap case has no sound key of its own.
    // CONTROL: via control_batch.sh, --phase '^soundProperties$', 2026-10-11, each alone, reverted, in
    //       app/library/wpx_package.cpp: audioOf() answering Absent in place of the probe -> "FAIL: S15.473: a
    //       video with sound declaring neither reads speed,tint" and the declaring-both line; settleSoundProperties'
    //       `== AudioStreams::Absent` strip made `if (false)` -> "... a silent video reads speed,mute,tint,volume
    //       and a still speed,mute,tint,volume"; audioOf()'s not-a-video answer deleted, so a still is probed ->
    //       "... a silent video reads speed,tint and a still speed,mute,tint,volume"; the cap test dropped from
    //       `declared` -> "... at the property cap reads 66 properties"; mute added whether declared or not ->
    //       "... declaring both reads speed,mute,tint,volume,mute" and the cap line (65).
    const Wallpaper added = at("added");
    const WallpaperProperty* mute = find(added, "mute");
    const WallpaperProperty* volume = find(added, "volume");
    expect(added.usable() && keysOf(added.properties) == u"speed,tint,mute,volume" && mute != nullptr &&
               mute->type == WallpaperProperty::Type::Bool && mute->defaultValue == QVariant(false) && volume != nullptr &&
               volume->type == WallpaperProperty::Type::Float && volume->defaultValue == QVariant(1.0) && volume->minimum == 0.0 &&
               volume->maximum == 1.0,
           QStringLiteral("S15.473: a video with sound declaring neither reads %1 (usable %2)").arg(keysOf(added.properties)).arg(added.usable()));
    const Wallpaper kept = at("declared");
    const WallpaperProperty* keptMute = find(kept, "mute");
    const WallpaperProperty* keptVolume = find(kept, "volume");
    expect(kept.usable() && keysOf(kept.properties) == u"speed,mute,tint,volume" && keptMute != nullptr &&
               keptMute->defaultValue == QVariant(true) && keptVolume != nullptr && keptVolume->maximum == 0.5 &&
               keptVolume->defaultValue == QVariant(0.25) && keptVolume->label == u"Loudness",
           QStringLiteral("S15.473: a video with sound declaring both reads %1 (usable %2)").arg(keysOf(kept.properties)).arg(kept.usable()));
    const Wallpaper silent = at("silent");
    const Wallpaper stillRead = at("still");
    expect(silent.usable() && keysOf(silent.properties) == u"speed,tint" && stillRead.usable() &&
               keysOf(stillRead.properties) == u"speed,tint",
           QStringLiteral("S15.473: declaring both, a silent video reads %1 and a still %2").arg(keysOf(silent.properties), keysOf(stillRead.properties)));
    const Wallpaper fullRead = at("full");
    expect(fullRead.usable() && fullRead.properties.size() == kMaxProperties && find(fullRead, "mute") == nullptr,
           QStringLiteral("S15.473: a video with sound at the property cap reads %1 properties (usable %2)")
               .arg(fullRead.properties.size()).arg(fullRead.usable()));
}

// S15.224, S15.225, S15.474, S15.488, S15.489
void soundPropertyRules(const QString& root) {
    const QString dir = root + QStringLiteral("/soundrules");
    const auto spec = [](const char* text) { return OrderedJson::parse(text); };
    const OrderedJson loudVolume = spec(R"({"type": "float", "default": 50, "min": 0, "max": 100})");
    const OrderedJson floatMute = spec(R"({"type": "float", "default": 0.5, "min": 0.1, "max": 1})");
    const auto manifest = [](const char* type, const OrderedJson& properties) {
        OrderedJson json = planManifest();
        json["type"] = type;
        json["properties"] = properties;
        return jsonBytes(json);
    };
    const auto at = [&](const QString& name) { return readWallpaperFolder(dir + QLatin1Char('/') + name); };
    const auto write = [&](const QString& name, const QByteArray& json, const QByteArray& asset) {
        return writeFolder(dir + QLatin1Char('/') + name, planEntriesWith(json, asset)).isEmpty();
    };
    struct Row {
        const char* goal;
        const char* what;
        OrderedJson properties;
        const char* reason; // nullptr: usable
    };
    const char* notBool = "Property \"mute\" in wallpaper.json is not a bool, and WallpaperX reads \"mute\" as one.";
    const Row rows[] = {
        {"S15.224", "a float mute", OrderedJson{{"mute", floatMute}}, notBool},
        {"S15.224", "a colour mute", OrderedJson{{"mute", spec(R"({"type": "color", "default": "#000000"})")}}, notBool},
        {"S15.224", "a bool mute", OrderedJson{{"mute", spec(R"({"type": "bool", "default": true})")}}, nullptr},
        {"S15.474", "volume from 0 to 0.5", OrderedJson{{"volume", spec(R"({"type": "float", "default": 0.25, "min": 0, "max": 0.5})")}}, nullptr},
        {"S15.474", "volume from 0 to 1", OrderedJson{{"volume", spec(R"({"type": "float", "default": 1, "min": 0, "max": 1})")}}, nullptr},
        {"S15.474", "volume to 1.5", OrderedJson{{"volume", spec(R"({"type": "float", "default": 1, "min": 0, "max": 1.5})")}},
         "Property \"volume\" in wallpaper.json allows 0 to 1.5, and WallpaperX plays a video's volume from 0 to 1."},
        {"S15.474", "volume from -0.1", OrderedJson{{"volume", spec(R"({"type": "float", "default": 0.5, "min": -0.1, "max": 1})")}},
         "Property \"volume\" in wallpaper.json allows -0.1 to 1, and WallpaperX plays a video's volume from 0 to 1."},
        {"S15.474", "a bool volume", OrderedJson{{"volume", spec(R"({"type": "bool", "default": true})")}},
         "Property \"volume\" in wallpaper.json is not a float, and WallpaperX reads a video's volume as one."},
        {"S15.474", "volume from 0 to 100", OrderedJson{{"volume", loudVolume}},
         "Property \"volume\" in wallpaper.json allows 0 to 100, and WallpaperX plays a video's volume from 0 to 1."},
    };
    // The same broken pair declared by a still, a shader, a video with sound and a video the probe cannot read.
    OrderedJson broken = planManifest()["properties"];
    broken["mute"] = floatMute;
    broken["volume"] = loudVolume;
    OrderedJson loudOnly = planManifest()["properties"];
    loudOnly["volume"] = loudVolume;
    int index = 0;
    bool written = true;
    for (const Row& row : rows) {
        written = write(QStringLiteral("row%1").arg(index++), manifest("video", row.properties), soundVideo()) && written;
    }
    written = write(QStringLiteral("still"), manifest("static", broken), soundVideo()) &&
              write(QStringLiteral("shader"), manifest("shader", broken), soundVideo()) &&
              write(QStringLiteral("unknown"), QByteArray(kPlanManifest), QByteArrayLiteral("not really a video")) &&
              write(QStringLiteral("unknownloud"), manifest("video", loudOnly), QByteArrayLiteral("not really a video")) && written;
    if (!written) {
        harnessFailure("S15.474: the wallpaper folders could not be written");
        return;
    }

    // GOAL: S15.224 and S15.474 at the folder read -- a video with sound declaring a "mute" that is not a
    //       bool, or a "volume" that is not a float or whose range leaves 0 to 1 (0 to 100 included), lists
    //       broken with that property's sentence; a bool mute and volumes within 0 to 1 are usable.
    // GOAL: S15.225 for these -- the app lists each usable exactly when the daemon would take its properties.
    // WHY-THIS-SHAPE: each row is a whole folder over a video with sound, read as the library reads it,
    //       since the manifest check no longer judges these keys (S15.488's parseManifest line below).
    // CONTROL: via control_batch.sh, --phase '^soundPropertyRules$', 2026-10-11, settleSoundProperties'
    //       `return problem;` made a no-op (app/library/wpx_package.cpp) -> every refusal row "was listed
    //       usable", e.g. "FAIL: S15.474: a video with sound declaring volume from 0 to 100 was listed usable",
    //       each with its S15.225 line ("the daemon would refuse it (property volume must be a number from 0 to
    //       1)"), and S15.489's 0-to-100 line. Reverted.
    // COVERAGE-ASSERT: the row count.
    int checked = 0;
    index = 0;
    for (const Row& row : rows) {
        const Wallpaper read = at(QStringLiteral("row%1").arg(index++));
        const QString what = QString::fromLatin1(row.what);
        ++checked;
        if (row.reason == nullptr) {
            expect(read.usable(), QStringLiteral("%1: a video with sound declaring %2 was listed broken: %3").arg(QLatin1StringView(row.goal), what, read.problem));
        } else {
            expect(!read.usable() && read.problem.endsWith(QString::fromUtf8(row.reason)),
                   QStringLiteral("%1: a video with sound declaring %2 was listed %3 \"%4\", not broken \"%5\"")
                       .arg(QLatin1StringView(row.goal), what, read.usable() ? QStringLiteral("usable") : QStringLiteral("broken"), read.problem,
                            QString::fromUtf8(row.reason)));
        }
        const std::string daemon = daemonVerdict(row.properties);
        expect(read.usable() == daemon.empty(),
               QStringLiteral("S15.225: a video with sound declaring %1: the app lists it %2, the daemon would %3")
                   .arg(what, read.usable() ? QStringLiteral("usable") : QStringLiteral("broken"),
                        daemon.empty() ? QStringLiteral("accept it") : QStringLiteral("refuse it (%1)").arg(QString::fromStdString(daemon))));
    }
    expect(checked == 9, QStringLiteral("S15.474: %1 sound rows were checked, not 9").arg(checked));

    // GOAL: S15.488 -- a still or a shader declaring a float "mute" and a "volume" of 0 to 100 is usable
    //       and keeps neither, its other properties kept in order; parseManifest accepts that manifest.
    // WHY-THIS-SHAPE: both keys broken at once, so a rule applied before the keys are taken away shows on
    //       either; the asset is the video with sound, so only the type decides.
    // CONTROL: via control_batch.sh, --phase '^soundPropertyRules$', 2026-10-11, settleSoundProperties' rule
    //       loop copied above the Absent strip (app/library/wpx_package.cpp) -> "FAIL: S15.488: declaring a float
    //       mute and a volume of 0 to 100, a still reads  (Property "mute" ... is not a bool ...) and a shader
    //       ...", alone. Reverted.
    const Wallpaper still = at(QStringLiteral("still"));
    const Wallpaper shader = at(QStringLiteral("shader"));
    const auto parsed = parseManifest(manifest("video", broken));
    expect(still.usable() && keysOf(still.properties) == u"speed,tint" && shader.usable() && keysOf(shader.properties) == u"speed,tint" &&
               parsed.has_value(),
           QStringLiteral("S15.488: declaring a float mute and a volume of 0 to 100, a still reads %1 (%2) and a shader %3 (%4); "
                          "the manifest alone was %5")
               .arg(keysOf(still.properties), still.problem, keysOf(shader.properties), shader.problem,
                    parsed ? QStringLiteral("accepted") : QStringLiteral("refused: ") + parsed.error()));

    // GOAL: S15.489 -- a video whose asset FFmpeg cannot open keeps a declared `mute` as declared and is
    //       given no `volume` of its own; a declared volume of 0 to 100 there is still refused.
    // WHY-THIS-SHAPE: the asset is the same non-media file in both, so the probe answers Unknown for both
    //       (S15.472's non-media case); `audio` is read as well, so a pass for some other reason shows.
    // CONTROL: via control_batch.sh, --phase '^soundPropertyRules$', 2026-10-11, each alone, reverted, in
    //       settleSoundProperties (app/library/wpx_package.cpp): the strip taken for any answer but Present ->
    //       "FAIL: S15.489: a video the probe cannot read, declaring mute, reads speed,tint (usable 1, audio
    //       Unknown)" and the 0-to-100 line ("was listed usable"); the `== AudioStreams::Unknown` return
    //       deleted -> "... declaring mute, reads speed,mute,tint,volume ...", alone.
    const Wallpaper unknown = at(QStringLiteral("unknown"));
    const Wallpaper unknownLoud = at(QStringLiteral("unknownloud"));
    const auto keptMute = std::ranges::find(unknown.properties, QStringLiteral("mute"), &WallpaperProperty::key);
    expect(unknown.usable() && unknown.audio == AudioStreams::Unknown && keysOf(unknown.properties) == u"speed,mute,tint" &&
               keptMute != unknown.properties.end() && keptMute->defaultValue == QVariant(true),
           QStringLiteral("S15.489: a video the probe cannot read, declaring mute, reads %1 (usable %2, audio %3)")
               .arg(keysOf(unknown.properties)).arg(unknown.usable()).arg(nameOf(unknown.audio)));
    expect(!unknownLoud.usable() &&
               unknownLoud.problem.endsWith(u"Property \"volume\" in wallpaper.json allows 0 to 100, and WallpaperX plays a video's volume from 0 to 1."),
           QStringLiteral("S15.489: a video the probe cannot read, declaring a volume of 0 to 100, was listed %1 \"%2\"")
               .arg(unknownLoud.usable() ? QStringLiteral("usable") : QStringLiteral("broken"), unknownLoud.problem));
}

// S15.490
void probeReuse(const QString& root) {
    const QString dir = root + QStringLiteral("/reuse");
    const QString assets = dir + QStringLiteral("/ocean/assets");
    const QString asset = assets + QStringLiteral("/video.mp4");
    if (!writeFolder(dir + QStringLiteral("/ocean"), planEntriesWith(QByteArray(kPlanManifest), soundVideo())).isEmpty()) {
        harnessFailure("S15.490: the library could not be written");
        return;
    }
    const QByteArray assetPath = QFile::encodeName(asset);
    struct stat original {};
    if (::stat(assetPath.constData(), &original) != 0) {
        harnessFailure("S15.490: the asset could not be read");
        return;
    }
    const auto audioIn = [](const std::expected<QList<Wallpaper>, QString>& scan) {
        return scan && scan->size() == 1 && scan->front().usable() ? nameOf(scan->front().audio) : QStringLiteral("(no usable row)");
    };

    // GOAL: S15.490 -- a scan opens a video folder's asset once, the probe reading the descriptor the
    //       folder read opened.
    // WHY-THIS-SHAPE: counted by the binary's own open/openat/openat2 replacements (FileOpens), on the
    //       assets folder alone, so the manifest and preview reads are not in the count.
    // CONTROL: via control_batch.sh, --phase '^probeReuse$', 2026-10-11, readFolderAt
    //       (app/library/wpx_package.cpp) probing a second descriptor it opens with openat -> "FAIL: S15.490: the
    //       first scan read audio Present and opened the asset 2 time(s), not Present and once". Reverted.
    std::expected<QList<Wallpaper>, QString> first;
    int opened = -1;
    {
        FileOpens opens(assets);
        if (!opens.ok()) {
            harnessFailure("S15.490: the open counter was already counting another folder");
            return;
        }
        static_cast<void>(opens.take());
        first = scanLibrary(dir);
        opened = opens.take();
    }
    expect(audioIn(first) == u"Present" && opened == 1,
           QStringLiteral("S15.490: the first scan read audio %1 and opened the asset %2 time(s), not Present and once").arg(audioIn(first)).arg(opened));
    if (audioIn(first) != u"Present") {
        return;
    }

    // Non-media bytes of the same size written in place, and the modification time put back: the asset's
    // stamp (device, inode, size, time) is as the first scan read it.
    const int fd = ::open(assetPath.constData(), O_WRONLY | O_CLOEXEC);
    const QByteArray junk(qsizetype(original.st_size), 'x');
    const timespec times[2] = {{0, UTIME_OMIT}, original.st_mtim};
    struct stat after {};
    const bool rewritten = fd >= 0 && ::write(fd, junk.constData(), size_t(junk.size())) == junk.size() && ::futimens(fd, times) == 0 &&
                           ::fstat(fd, &after) == 0;
    if (fd >= 0) {
        ::close(fd);
    }
    if (!rewritten || after.st_ino != original.st_ino || after.st_size != original.st_size || after.st_mtim.tv_sec != original.st_mtim.tv_sec ||
        after.st_mtim.tv_nsec != original.st_mtim.tv_nsec) {
        harnessFailure("S15.490: the asset could not be rewritten with its stamp kept");
        return;
    }

    // GOAL: S15.490 -- a rescan given an earlier scan reuses its probe answer for an asset whose stamp is
    //       unchanged, and probes again once the modification time moves.
    // WHY-THIS-SHAPE: the asset is no longer media, so a scan with no earlier one answers Unknown; Present
    //       can then only come from the earlier answer. The touched case is read against the same earlier
    //       scan, so only the stamp differs.
    // CONTROL: via control_batch.sh, --phase '^probeReuse$', 2026-10-11, each alone, reverted, audioOf()'s stamp
    //       comparison (app/library/wpx_package.cpp) made `false` -> "FAIL: S15.490: with its stamp unchanged, a
    //       rescan given the first scan read Unknown and one without it Unknown"; made `true` -> "FAIL: S15.490:
    //       with the modification time moved, a rescan given the first scan read Present, not Unknown".
    const auto reused = scanLibrary(dir, *first);
    const auto fresh = scanLibrary(dir);
    expect(audioIn(reused) == u"Present" && audioIn(fresh) == u"Unknown",
           QStringLiteral("S15.490: with its stamp unchanged, a rescan given the first scan read %1 and one without it %2, not Present and Unknown")
               .arg(audioIn(reused), audioIn(fresh)));
    const timespec later[2] = {{0, UTIME_OMIT}, {original.st_mtim.tv_sec + 1, original.st_mtim.tv_nsec}};
    if (::utimensat(AT_FDCWD, assetPath.constData(), later, 0) != 0) {
        harnessFailure("S15.490: the asset's modification time could not be moved");
        return;
    }
    const auto touched = scanLibrary(dir, *first);
    expect(audioIn(touched) == u"Unknown",
           QStringLiteral("S15.490: with the modification time moved, a rescan given the first scan read %1, not Unknown").arg(audioIn(touched)));
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
    runPhase("speedChoices", speedChoices);
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
    runPhase("audioProbe", [&] { audioProbe(root); });
    runPhase("soundProperties", [&] { soundProperties(root); });
    runPhase("soundPropertyRules", [&] { soundPropertyRules(root); });
    runPhase("probeReuse", [&] { probeReuse(root); });
    return verdict();
}
