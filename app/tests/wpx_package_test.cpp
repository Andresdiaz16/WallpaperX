// SPDX-License-Identifier: GPL-2.0-or-later

// app/library/wpx_package.cpp and app/library/preview_image_provider.cpp, with no bus and no
// event loop: one wallpaper.json, one .wpx archive, one folder, one preview address at a time.
// Goals S15.31-S15.35, S15.45, S15.47, S15.48, S15.50, S15.51, S15.53, S15.55, S15.58 in GOALS.md.

#include "preview_image_provider.h"
#include "wpx_fixtures.h"
#include "wpx_package.h"

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QImageReader>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QUrl>

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <future>
#include <limits>
#include <thread>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace wallpaperx;
using namespace wallpaperx::apptest;
using wallpaperx::testing::check;
using wallpaperx::testing::harnessFailure;
using wallpaperx::testing::notVerified;

namespace {

constexpr int kFifoTimeoutMs = 3000;

void expect(bool cond, const QString& msg) {
    check(cond, msg.toStdString().c_str());
}

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

// Runs `work`, described by `what`, on its own thread and waits at most kFifoTimeoutMs. Nothing is
// returned, with a failed check, when it threw or blocked. On a timeout the FIFO at `fifo` is
// opened for writing, which releases a reader blocked opening it. A worker still blocked after
// that cannot be joined, so the run reports it and exits at once rather than hang in the thread's
// destructor.
// CONTROL: by hand, test-side: the S15.33 FIFO work made `std::this_thread::sleep_for(20s)` ->
//       "FAIL: S15.33: readWallpaper on a FIFO blocked for 3000 ms, and opening the FIFO's write
//       end did not release it", exit 1 after about 5 s instead of 20; restored.
// CONTROL: app/library/wpx_package.cpp, parseManifest's `Json::parse_error` and `Json::exception`
//       catch clauses deleted, so badjson.wpx throws out of the scan -> red, "FAIL: S15.34: scanning
//       a folder holding a FIFO threw: [json.exception.parse_error.101] ...", beside runPhase's
//       "an exception escaped malformedManifests ..."; ctest graded it Failed, not aborted; restored.
template <typename Result>
std::optional<Result> runBounded(const std::function<Result()>& work, const QString& fifo, const QString& what) {
    std::promise<Result> promise;
    std::future<Result> result = promise.get_future();
    std::jthread worker([&] {
        try {
            promise.set_value(work());
        } catch (...) {
            promise.set_exception(std::current_exception());
        }
    });
    const QString blocked = QStringLiteral("%1 blocked for %2 ms").arg(what).arg(kFifoTimeoutMs);
    if (result.wait_for(std::chrono::milliseconds(kFifoTimeoutMs)) == std::future_status::ready) {
        try {
            return result.get();
        } catch (const std::exception& e) {
            expect(false, QStringLiteral("%1 threw: %2").arg(what, QString::fromLocal8Bit(e.what())));
        } catch (...) {
            expect(false, QStringLiteral("%1 threw something that is not a std::exception").arg(what));
        }
        return std::nullopt;
    }
    for (int attempt = 0; attempt < 200 && result.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready; ++attempt) {
        const int fd = ::open(QFile::encodeName(fifo).constData(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) {
            ::close(fd);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (result.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        expect(false, blocked + QStringLiteral(", and opening the FIFO's write end did not release it"));
        static_cast<void>(wallpaperx::testing::reportCheckFailures());
        std::fflush(stderr);
        std::_Exit(1);
    }
    expect(false, blocked);
    return std::nullopt;
}

bool isBrokenOnly(const Wallpaper& w) {
    return !w.usable() && !w.problem.isEmpty() && w.type.isEmpty() && w.title.isEmpty() && w.author.isEmpty() &&
           w.tags.isEmpty() && w.assetName.isEmpty() && w.previewName.isEmpty() && w.properties.isEmpty() &&
           w.previewSource().isEmpty();
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
        expect(speed.type == WallpaperProperty::Type::Float && speed.minimum == 0.1 && speed.maximum == 3.0 &&
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
        {"a property with an empty name", property("", R"({"type": "bool", "default": true})"), "a property with an empty name"},
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
    //       parseProperty's empty-key, is_object, type-refusal, no-default, min<max, range and
    //       bool tests -> their rows; the colour pattern `{3,8}` -> "#abc was accepted";
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
    const bool asRoot = ::geteuid() == 0;

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
    make("bigpreview.wpx", bigPreview);
    make("percent.wpx", {{QStringLiteral("shot%1.png"), QByteArray((1 << 20) + 1, '\0')}});
    if (!writeBytes(at("notzip.wpx"), "hello")) {
        setupFault = QStringLiteral("notzip.wpx could not be written");
    }
    if (!asRoot) {
        make("locked.wpx", planEntries(plan));
        if (!setMode(at("locked.wpx"), 0)) {
            setupFault = QStringLiteral("locked.wpx could not be made mode 000");
        }
    }
    if (!setupFault.isEmpty()) {
        harnessFailure("S15.33: a fixture archive could not be built: " + setupFault.toStdString());
        static_cast<void>(setMode(at("locked.wpx"), 0644));
        return;
    }

    const Wallpaper good = readWallpaper(at("good.wpx"));
    expect(good.usable() && good.filePath == at("good.wpx") && good.fileName == u"good.wpx" && good.title == u"Ocean Waves" &&
               !good.previewSource().isEmpty(),
           QStringLiteral("S15.33: a good archive read as usable=%1 title \"%2\" problem \"%3\"").arg(good.usable()).arg(good.title, good.problem));
    // GOAL: S15.33 -- a wallpaper.json of exactly 1 MiB is read; one byte more is refused before it is parsed.
    // WHY-THIS-SHAPE: both manifests are PLAN's example padded with trailing spaces, so they are
    //       valid JSON: without the cap the over-size one would be read as usable, which is what goes red.
    // CONTROL: app/library/wpx_package.cpp, `st.size > static_cast<zip_uint64_t>(maxBytes)` made
    //       `false && ...` -> red, "S15.33: huge.wpx read as usable=1 title "Ocean Waves""; restored, green.
    const Wallpaper atCapRead = readWallpaper(at("atcap.wpx"));
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
    // CONTROL: app/library/wpx_package.cpp, the `for (const QString& name : {parsed->previewName, ...})`
    //       loop's `return broken(...)` made `continue` -> red, "noasset.wpx read as usable=1"; the
    //       same `return broken(...)` made to return the parsed wallpaper with `problem` set -> red,
    //       "noasset.wpx read as usable=0 title "Ocean Waves""; the ZIP_ER_NOZIP branch's condition
    //       made `false` -> red, "notzip.wpx ... It cannot be read as a ZIP archive"; the open
    //       failure's `.arg(systemError(errno))` dropped -> red, "a mode-000 archive read as ...
    //       "It cannot be opened.""; restored, green each time.
    for (const ArchiveCase& c : cases) {
        const Wallpaper w = readWallpaper(c.file);
        expect(isBrokenOnly(w) && w.problem.contains(QLatin1String(c.reason)) && w.filePath == c.file &&
                   w.fileName == QFileInfo(c.file).fileName(),
               QStringLiteral("S15.33: %1 read as usable=%2 title \"%3\" problem \"%4\"; wanted broken, \"%5\", nothing else set")
                   .arg(QFileInfo(c.file).fileName())
                   .arg(w.usable())
                   .arg(w.title, w.problem, QLatin1String(c.reason)));
    }

    // An unreadable file: root reads through mode 000, so this cannot be set up as root.
    if (asRoot) {
        notVerified("S15.33 (an unreadable archive)", "running as root, which can read a mode-000 file");
    } else {
        const Wallpaper w = readWallpaper(at("locked.wpx"));
        const bool restored = setMode(at("locked.wpx"), 0644);
        expect(isBrokenOnly(w) && w.problem == u"It cannot be opened: Permission denied." && w.fileName == u"locked.wpx",
               QStringLiteral("S15.33: a mode-000 archive read as usable=%1 problem \"%2\"").arg(w.usable()).arg(w.problem));
        if (!restored) {
            harnessFailure("S15.33: locked.wpx could not be made readable again");
        }
    }

    // GOAL: S15.33 -- a preview entry over 32 MiB uncompressed is refused by its size record.
    // CONTROL: shares the size-cap mutation above (`false &&`) -> red, "a 32 MiB + 1 preview was read"; restored.
    const auto preview = readPackageEntry(at("bigpreview.wpx"), QStringLiteral("preview.png"), kMaxPreviewBytes);
    expect(!preview && preview.error() == u"preview.png is larger than 32 MiB.",
           QStringLiteral("S15.33: a 32 MiB + 1 preview was read: %1").arg(preview ? QStringLiteral("%1 bytes").arg(preview->size()) : preview.error()));

    // GOAL: S15.55 -- an entry name holding "%1" is quoted as it is in a refusal, not taken as a
    //       place for the size to go.
    // CONTROL: app/library/wpx_package.cpp, readEntry's `.arg(name, QString::number(maxBytes >> 20))`
    //       made `.arg(name).arg(QString::number(maxBytes >> 20))` -> red, "S15.55: ... "shot1.png
    //       is larger than %2 MiB.""; restored.
    const auto percent = readPackageEntry(at("percent.wpx"), QStringLiteral("shot%1.png"), 1 << 20);
    expect(!percent && percent.error() == u"shot%1.png is larger than 1 MiB.",
           QStringLiteral("S15.55: an entry named %1 over the cap was refused with \"%2\"")
               .arg(QStringLiteral("shot%1.png"), percent ? QStringLiteral("nothing") : percent.error()));

    // GOAL: S15.33 -- a FIFO handed to readWallpaper directly (one swapped in after the folder was
    //       listed) is refused as "not a regular file" without blocking the reading thread.
    // WHY-THIS-SHAPE: run on its own thread with a deadline (runBounded), so a hang is reported as
    //       a failure, never as a stuck ctest entry and never as a pass.
    // CONTROL: app/library/wpx_package.cpp, openArchive's `O_RDONLY | O_CLOEXEC | O_NONBLOCK` made
    //       `O_RDONLY | O_CLOEXEC` -> red, "S15.33: readWallpaper on a FIFO blocked for 3000 ms";
    //       restored, green.
    const QString fifo = at("fifo.wpx");
    if (::mkfifo(QFile::encodeName(fifo).constData(), 0644) != 0) {
        harnessFailure("S15.33: mkfifo failed: " + std::string(std::strerror(errno)));
        return;
    }
    const auto read = runBounded<Wallpaper>([&fifo] { return readWallpaper(fifo); }, fifo, QStringLiteral("S15.33: readWallpaper on a FIFO"));
    if (read) {
        expect(isBrokenOnly(*read) && read->problem == u"It is not a regular file.",
               QStringLiteral("S15.33: a FIFO read as usable=%1 problem \"%2\"").arg(read->usable()).arg(read->problem));
    }
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
    if (!QDir().mkpath(lib) || !QDir().mkpath(elsewhere)) {
        fault = QStringLiteral("the library folders could not be made");
    }
    const auto make = [&fault](const QString& path, const Entries& entries) {
        if (const QString why = writeZip(path, entries); !why.isEmpty()) {
            fault = why;
        }
    };
    make(at(QStringLiteral("ocean.wpx")), planEntries(plan));
    // Lower case, so by title alone it would sort after every broken file's name in any locale;
    // only the usable-first rule puts it ahead of them.
    make(at(QStringLiteral("still.wpx")), planEntries(edited([](OrderedJson& j) { j["title"] = "zebra still"; })));
    make(elsewhere + QStringLiteral("/linked.wpx"), planEntries(edited([](OrderedJson& j) { j["title"] = "Linked"; })));
    make(at(QStringLiteral(".hidden.wpx")), planEntries(plan));
    make(at(QStringLiteral("nomanifest.wpx")), noManifest);
    make(at(QStringLiteral("badjson.wpx")), planEntries("{ nope"));
    make(at(QStringLiteral("escape.wpx")), planEntries(edited([](OrderedJson& j) { j["file"] = "../../evil"; })));
    make(at(QStringLiteral("future.wpx")), planEntries(edited([](OrderedJson& j) { j["schema_version"] = "2"; })));
    make(at(QStringLiteral("outofrange.wpx")), planEntries(edited([](OrderedJson& j) { j["properties"]["speed"]["default"] = 9; })));
    make(at(QStringLiteral("missingasset.wpx")), noAsset);
    make(at(QStringLiteral("readme.txt")), planEntries(plan));
    if (!writeBytes(at(QStringLiteral("notzip.wpx")), "hello") || !QDir().mkpath(at(QStringLiteral("adir.wpx"))) ||
        ::mkfifo(QFile::encodeName(at(QStringLiteral("fifo.wpx"))).constData(), 0644) != 0 ||
        !QFile::link(QStringLiteral("../elsewhere/linked.wpx"), at(QStringLiteral("link.wpx")))) {
        fault = QStringLiteral("a plain file, folder, FIFO or symlink could not be made");
    }
    const bool asRoot = ::geteuid() == 0;
    if (!asRoot) {
        make(at(QStringLiteral("locked.wpx")), planEntries(plan));
        if (!setMode(at(QStringLiteral("locked.wpx")), 0)) {
            fault = QStringLiteral("locked.wpx could not be made mode 000");
        }
    }
    if (!fault.isEmpty()) {
        harnessFailure("S15.34: the library folder could not be built: " + fault.toStdString());
        static_cast<void>(setMode(at(QStringLiteral("locked.wpx")), 0644));
        return;
    }

    using Scan = std::expected<QList<Wallpaper>, QString>;
    // GOAL: S15.34 -- a FIFO named *.wpx in the folder never blocks a scan.
    // WHY-THIS-SHAPE: bounded as in S15.33. Three defences stand in front of the hang -- the scan
    //       skips what stat() says is not a regular file, it skips an empty file (a FIFO's size is
    //       0), and the open is O_NONBLOCK -- so the mutation has to remove all three to reach it;
    //       removing fewer is caught by the listing check below or by S15.33.
    // CONTROL: app/library/wpx_package.cpp, scanLibrary's `if (!S_ISREG(st.st_mode)) { continue; }`
    //       and its empty-file `continue` block deleted and openArchive's `| O_NONBLOCK` deleted, in
    //       one patch -> red, "S15.34: scanning a folder holding a FIFO blocked for 3000 ms"; restored.
    //       The same break without the empty-file block left this check green (the FIFO is skipped
    //       as empty); only S15.33 and the listing below went red.
    const QString fifo = at(QStringLiteral("fifo.wpx"));
    const auto scanned = runBounded<Scan>([&lib] { return scanLibrary(lib); }, fifo, QStringLiteral("S15.34: scanning a folder holding a FIFO"));
    if (!asRoot && !setMode(at(QStringLiteral("locked.wpx")), 0644)) {
        harnessFailure("S15.34: locked.wpx could not be made readable again");
    }
    if (!scanned) {
        return;
    }
    if (!*scanned) {
        expect(false, QStringLiteral("S15.34: the library folder could not be scanned: %1").arg(scanned->error()));
        return;
    }
    const QList<Wallpaper>& list = **scanned;

    // GOAL: S15.34 -- the folder's usable wallpapers are all listed as usable, and every broken
    //       file is listed with its reason; one bad file stops nothing else from loading.
    // WHY-THIS-SHAPE: the whole listing is compared in order, so a missing, extra or reordered
    //       entry is named; usable ones first by title, then broken ones by file name. One usable
    //       title is lower case: with capitalised titles only, the C locale sorts them all before
    //       the broken names anyway and the usable-first rule goes untested.
    // CONTROL: app/library/wpx_package.cpp, listedBefore()'s `if (a.usable() != b.usable())` block deleted
    //       -> red, "listed [link.wpx, ocean.wpx, badjson.wpx(broken), ... still.wpx]";
    //       isPackageFileName's `!name.startsWith('.') &&` deleted -> red, ".hidden.wpx" listed;
    //       scanLibrary's `if (!S_ISREG(st.st_mode)) { continue; }` deleted -> red, "adir.wpx(broken)"
    //       listed (the FIFO is still dropped as an empty file); its first fstatat's flags `0` made
    //       `AT_SYMLINK_NOFOLLOW` -> red, "link.wpx" missing; the scan made to return the first
    //       broken file's problem as its error -> red, "the library folder could not be scanned";
    //       restored, green each time.
    QStringList want{QStringLiteral("link.wpx"), QStringLiteral("ocean.wpx"), QStringLiteral("still.wpx"),
                     QStringLiteral("badjson.wpx"), QStringLiteral("escape.wpx"), QStringLiteral("future.wpx")};
    if (!asRoot) {
        want.append(QStringLiteral("locked.wpx"));
    }
    want += QStringList{QStringLiteral("missingasset.wpx"), QStringLiteral("nomanifest.wpx"), QStringLiteral("notzip.wpx"),
                        QStringLiteral("outofrange.wpx")};
    QStringList got;
    for (const Wallpaper& w : list) {
        got.append(w.fileName + (w.usable() ? QString() : QStringLiteral("(broken)")));
    }
    QStringList wantMarked;
    for (const QString& name : want) {
        const bool usable = name == u"still.wpx" || name == u"link.wpx" || name == u"ocean.wpx";
        wantMarked.append(name + (usable ? QString() : QStringLiteral("(broken)")));
    }
    expect(got == wantMarked, QStringLiteral("S15.34: the folder listed [%1], not [%2]").arg(got.join(u", "), wantMarked.join(u", ")));

    const QMap<QString, QString> reasons{
        {QStringLiteral("badjson.wpx"), QStringLiteral("wallpaper.json is not valid JSON")},
        {QStringLiteral("escape.wpx"), QStringLiteral("\"file\" is not a plain path")},
        {QStringLiteral("future.wpx"), QStringLiteral("schema version \"2\"")},
        {QStringLiteral("locked.wpx"), QStringLiteral("Permission denied")},
        {QStringLiteral("missingasset.wpx"), QStringLiteral("has no assets/video.mp4")},
        {QStringLiteral("nomanifest.wpx"), QStringLiteral("has no wallpaper.json")},
        {QStringLiteral("notzip.wpx"), QStringLiteral("not a ZIP archive")},
        {QStringLiteral("outofrange.wpx"), QStringLiteral("default outside its min and max")},
    };
    for (const Wallpaper& w : list) {
        if (!w.usable()) {
            expect(w.problem.contains(reasons.value(w.fileName, QStringLiteral("\x01"))),
                   QStringLiteral("S15.34: %1 is broken with \"%2\", not \"%3\"").arg(w.fileName, w.problem, reasons.value(w.fileName)));
        }
    }
    const auto link = std::ranges::find(list, QStringLiteral("link.wpx"), &Wallpaper::fileName);
    expect(link != list.end() && link->title == u"Linked" && link->filePath == at(QStringLiteral("link.wpx")),
           QStringLiteral("S15.34: the symlink to a package was not listed under its own name with the target's title"));

    // GOAL: S15.34 -- a folder that does not exist is an empty library, not an error; a path that
    //       is a file, or a folder that cannot be listed, is an error that says which.
    // CONTROL: app/library/wpx_package.cpp, `if (error == ENOENT) {` made `if (false) {` -> red,
    //       "a missing folder gave an error"; `if (error == ENOTDIR) {` made `if (false) {` -> red,
    //       "a path that is a file gave ... cannot be read: Not a directory"; the opendir failure made
    //       to return an empty list -> red, "a folder that cannot be listed gave a listing"; restored.
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
    }
}

// S15.50
void fileNamesThatAreNotUtf8(const QString& root) {
    const QString dir = root + QStringLiteral("/names");
    const QByteArray dirBytes = QFile::encodeName(dir);
    const auto package = zipBytes(dir + QStringLiteral(".scratch"), planEntries(QByteArray(kPlanManifest)));
    const RawNamesRemover removeRawNames(dir, {"caf\xe8.wpx", "caf\xe9.wpx"});
    if (!QDir().mkpath(dir) || !package || !writeRawName(dir, "caf\xe8.wpx", *package) || !writeRawName(dir, "caf\xe9.wpx", *package) ||
        !writeRawName(dir, "caf\xc3\xa9.wpx", *package) || !QFile::link(QStringLiteral("nothing-here.wpx"), dir + QStringLiteral("/dangling.wpx"))) {
        harnessFailure("S15.50: the folder of odd names could not be built");
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
    const QString notUtf8 = QStringLiteral("Its path is not valid UTF-8, so it cannot be set as a wallpaper. Rename the file.");
    const Wallpaper* e8 = rowFor("caf\xe8.wpx");
    const Wallpaper* e9 = rowFor("caf\xe9.wpx");
    const Wallpaper* valid = rowFor("caf\xc3\xa9.wpx");
    const Wallpaper* dangling = rowFor("dangling.wpx");
    QStringList listed;
    for (const Wallpaper& w : *scan) {
        listed.append(QString::fromLatin1(encodeFileName(w.fileName).toPercentEncoding()) + (w.usable() ? QString() : QStringLiteral("(broken: ") + w.problem + u')'));
    }
    // GOAL: S15.50 -- a package whose name is not UTF-8 is listed, broken, with the reason; two
    //       such names differing in one byte are two rows with distinct paths, each converting back
    //       to its own file's bytes; a valid UTF-8 name is read normally.
    // WHY-THIS-SHAPE: each row is looked up by converting its path back to bytes, so a row whose
    //       text no longer names its file is not found; comparing texts would pass two names both
    //       decoded to U+FFFD.
    // CONTROL: app/library/wpx_package.cpp, readWallpaper's `if (!QStringView(path).isValidUtf16()) {`
    //       made `if (false) {` -> red, "S15.50: the folder of odd names listed [caf%C3%A9.wpx,
    //       caf%E8.wpx, caf%E9.wpx, ...]", both read as usable; decodeFileName's
    //       `QChar(char16_t(0xDC00 + value))` made `QChar(char16_t(0xFFFD))` (what QFile::decodeName
    //       does) -> red, both rows became "caf%EF%BF%BD.wpx(broken: It cannot be opened: No such file
    //       or directory.)"; restored, green each time.
    expect(scan->size() == 4 && e8 && e9 && valid && e8 != e9 && e8->filePath != e9->filePath && e8->problem == notUtf8 &&
               e9->problem == notUtf8 && !QStringView(e8->filePath).isValidUtf16() && valid->usable() &&
               valid->fileName == QStringLiteral("café.wpx"),
           QStringLiteral("S15.50: the folder of odd names listed [%1]").arg(listed.join(u", ")));

    // GOAL: S15.50 -- a link to a file that does not exist is listed broken, saying so.
    // CONTROL: app/library/wpx_package.cpp, scanLibrary's `if (error == ENOENT && ::fstatat(`
    //       made `if (error == ENOENT || ::fstatat(` (every ENOENT dropped) -> red, "dangling.wpx
    //       ... not listed"; restored.
    expect(dangling && !dangling->usable() && dangling->problem == u"It is a link to a file that does not exist.",
           QStringLiteral("S15.50: the dangling link is %1").arg(dangling ? dangling->problem : QStringLiteral("not listed: [%1]").arg(listed.join(u", "))));

    // GOAL: S15.50 -- a path's text converts back to exactly the bytes it came from, for any
    //       bytes: valid UTF-8 as the same text QString::fromUtf8 gives, anything else as text that
    //       is not valid UTF-16, so no other name can be taken for it.
    // WHY-THIS-SHAPE: fixed edge cases (a leading byte-order mark, which a UTF-8 decoder drops by
    //       default; overlong and surrogate encodings; a lone continuation byte) plus random bytes.
    // CONTROL: app/library/wpx_package.cpp, decodeFileName's `| QStringDecoder::Flag::ConvertInitialBom`
    //       deleted -> red, "S15.50: ef bb bf 62 6f 6d 2e 77 70 78 came back as 62 6f 6d 2e 77 70 78";
    //       encodeFileName's `bytes.append(static_cast<char>(unit & 0xFF));` made `bytes.append('?');`
    //       -> red, "S15.50: 63 61 66 e9 2e 77 70 78 came back as 3f 3f 3f 3f 3f 3f 3f 3f"; restored.
    // COVERAGE-ASSERT: the number of byte strings compared.
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
        const QString text = decodeFileName(bytes);
        const QByteArray back = encodeFileName(text);
        const bool utf8 = QString::fromUtf8(bytes).toUtf8() == bytes && !bytes.startsWith("\xef\xbb\xbf");
        const bool ok = back == bytes && (utf8 ? text == QString::fromUtf8(bytes) && text.isValidUtf16() : true) &&
                        (!utf8 && !bytes.startsWith("\xef\xbb\xbf") ? !text.isValidUtf16() : true);
        ++compared;
        expect(ok, QStringLiteral("S15.50: %1 came back as %2 (valid UTF-16: %3)")
                       .arg(QString::fromLatin1(bytes.toHex(' ')), QString::fromLatin1(back.toHex(' '))).arg(text.isValidUtf16()));
    }
    expect(compared == 311, QStringLiteral("S15.50: %1 byte strings were compared, not 311").arg(compared));
}

// S15.58
void emptyFiles(const QString& root) {
    const QString dir = root + QStringLiteral("/empties");
    const QString target = root + QStringLiteral("/empty-target.wpx");
    if (!QDir().mkpath(dir) || !writeZip(dir + QStringLiteral("/good.wpx"), planEntries(QByteArray(kPlanManifest))).isEmpty() ||
        !writeBytes(dir + QStringLiteral("/empty.wpx"), {}) || !writeBytes(target, {}) ||
        !QFile::link(target, dir + QStringLiteral("/emptylink.wpx"))) {
        harnessFailure("S15.58: the folder of empty files could not be built");
        return;
    }
    // GOAL: S15.58 -- an empty file in the folder is not listed at all (it is one not yet
    //       written), while a link to an empty file is listed, broken.
    // WHY-THIS-SHAPE: both are in one folder beside a usable package, so the listing as a whole
    //       is compared.
    // CONTROL: app/library/wpx_package.cpp, scanLibrary's empty-file `continue` block deleted ->
    //       red, "S15.58: ... listed [good.wpx, empty.wpx(broken), emptylink.wpx(broken)]";
    //       its `&& !S_ISLNK(link.st_mode)` deleted -> red, "... listed [good.wpx]"; restored.
    const auto scan = scanLibrary(dir);
    QStringList got;
    if (scan) {
        for (const Wallpaper& w : *scan) {
            got.append(w.fileName + (w.usable() ? QString() : QStringLiteral("(broken)")));
        }
    }
    expect(scan && got == QStringList{QStringLiteral("good.wpx"), QStringLiteral("emptylink.wpx(broken)")},
           QStringLiteral("S15.58: a folder with an empty file and a link to one listed [%1]%2")
               .arg(got.join(u", "), scan ? QString() : QStringLiteral(": ") + scan.error()));
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
    const QString path = root + QStringLiteral("/turned.wpx");
    if (!jpeg.startsWith("\xff\xd8") || stored.size() != QSize(400, 300) || (stored.transformation() & QImageIOHandler::TransformationRotate90) == 0 ||
        !writeZip(path, {{QStringLiteral("turned.jpg"), jpeg}}).isEmpty()) {
        harnessFailure("S15.51: a JPEG turned by its EXIF orientation could not be built");
        return;
    }
    PreviewImageProvider provider;
    const QString id = previewImageId(path, QStringLiteral("turned.jpg"), FileStamp{1, 2, 3, 4});
    // GOAL: S15.51 -- a preview turned 90 degrees by its EXIF orientation is reported at its shown
    //       size (300x400, not the stored 400x300) and decoded covering the requested box in that
    //       orientation, never enlarged.
    // WHY-THIS-SHAPE: a 160x90 box: covering the shown 300x400 gives 160x213; working in the stored
    //       orientation instead gives 160x120, which leaves the tile short, and a box larger than
    //       the image must give the whole image as shown, 300x400.
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
    const QString odd = root + QStringLiteral("/odd dir %41 ?q #frag ü/wall paper%2F?#.wpx");
    FileStamp stamp{1, 2, 3, 4};
    // GOAL: S15.45 -- a preview address carries the archive path and entry name through a URL and
    //       back unchanged, as QML's image loader splits it, for paths holding '/', '%', '?', '#',
    //       spaces and non-ASCII characters.
    // WHY-THIS-SHAPE: the id is taken from the URL the way QQuickPixmapCache does,
    //       `toString(RemoveScheme | RemoveAuthority).mid(1)`, not from previewImageId() directly:
    //       the URL is where percent-decoding and '?'/'#' splitting would damage a raw path.
    // CONTROL: app/library/wpx_package.cpp, previewImageId's `QString::fromLatin1(archivePath.toUtf8().toBase64(kIdEncoding))`
    //       made the raw `archivePath` -> red, "S15.45: ... came back as nothing"; restored.
    for (const auto& [path, entry] : {std::pair{odd, QStringLiteral("previews/p ü#?%.png")},
                                      std::pair{QStringLiteral("/tmp/plain.wpx"), QStringLiteral("preview.png")}}) {
        Wallpaper w;
        w.filePath = path;
        w.fileName = QFileInfo(path).fileName();
        w.previewName = entry;
        w.stamp = stamp;
        const QUrl url = w.previewSource();
        const auto back = parsePreviewImageId(url.toString(QUrl::RemoveScheme | QUrl::RemoveAuthority).mid(1));
        expect(url.scheme() == u"image" && url.host() == QLatin1String(kPreviewProviderId) && back && back->archivePath == path &&
                   back->entryName == entry,
               QStringLiteral("S15.45: %1 in %2 came back as %3").arg(entry, path, back ? back->entryName + QStringLiteral(" in ") + back->archivePath : QStringLiteral("nothing")));
    }
    // GOAL: S15.45 -- an id that was not made by previewImageId gives nothing, never a path.
    // CONTROL: app/library/wpx_package.cpp, parsePreviewImageId's `if (parts.size() != 3)` made
    //       `if (parts.size() < 3)` -> red, "the id "1-2-3-4/Zm9v/Zm9v/Zm9v" gave foo in foo"; restored.
    for (const QString& garbage : {QString(), QStringLiteral("a/b"), QStringLiteral("1-2-3-4/!!!/Zm9v"), QStringLiteral("1-2-3-4//Zm9v"),
                                   QStringLiteral("1-2-3-4/Zm9v/Zm9v/Zm9v"), QStringLiteral("1-2-3-4/Zm9v")}) {
        const auto got = parsePreviewImageId(garbage);
        expect(!got, QStringLiteral("S15.45: the id \"%1\" gave %2 in %3").arg(garbage, got ? got->entryName : QString(), got ? got->archivePath : QString()));
    }

    // GOAL: S15.45 -- a rewritten archive gets a different preview address, so QML does not show a
    //       cached image of the old one; an unchanged archive keeps its address; a broken one has none.
    // WHY-THIS-SHAPE: the rewrite keeps inode and size and is checked to have done so, and the
    //       rewritten file must still read as usable with an address, so a failed or truncated
    //       rewrite cannot pass as "a different address".
    // CONTROL: app/library/wpx_package.cpp, previewImageId's `QString::number(stamp.size),
    //       QString::number(stamp.modifiedNs)` made `QStringLiteral("0"), QStringLiteral("0")` -> red,
    //       "the rewritten archive kept its address"; its `QString::number(stamp.device)` made a
    //       per-call counter -> red, "an unchanged archive's address changed between reads";
    //       previewSource()'s `if (!usable()) {` made `if (false) {` -> red, "a broken wallpaper has
    //       a preview address"; restored, green each time.
    const QString path = root + QStringLiteral("/changing.wpx");
    if (!writeZip(path, planEntries(QByteArray(kPlanManifest))).isEmpty()) {
        harnessFailure("S15.45: changing.wpx could not be written");
        return;
    }
    const QUrl first = readWallpaper(path).previewSource();
    const QUrl again = readWallpaper(path).previewSource();
    expect(!first.isEmpty() && first == again, QStringLiteral("S15.45: an unchanged archive's address changed between reads"));
    const auto bytes = readBytes(path);
    struct stat before {};
    if (!bytes || ::stat(QFile::encodeName(path).constData(), &before) != 0) {
        harnessFailure("S15.45: changing.wpx could not be read back");
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    struct stat after {};
    if (!writeBytes(path, *bytes) || ::stat(QFile::encodeName(path).constData(), &after) != 0 || after.st_ino != before.st_ino ||
        after.st_size != before.st_size) {
        harnessFailure("S15.45: changing.wpx could not be rewritten in place with the same size");
        return;
    }
    const QUrl rewritten = readWallpaper(path).previewSource();
    if (before.st_mtim.tv_sec == after.st_mtim.tv_sec && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec) {
        notVerified("S15.45 (a rewrite changes the address)", "the file system kept the same modification time across a rewrite");
    } else {
        expect(!rewritten.isEmpty() && rewritten != first,
               QStringLiteral("S15.45: the rewritten archive kept its address %1 (now \"%2\")").arg(first.toString(), rewritten.toString()));
    }
    Wallpaper broken;
    broken.filePath = path;
    broken.previewName = QStringLiteral("preview.png");
    broken.problem = QStringLiteral("x");
    expect(broken.previewSource().isEmpty(), QStringLiteral("S15.45: a broken wallpaper has a preview address"));

    // GOAL: S15.45 -- the provider decodes the preview scaled down to cover the requested box and
    //       never enlarges it; it reports the full size, and an address it cannot serve gives a
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
    const QString id = readWallpaper(path).previewSource().toString(QUrl::RemoveScheme | QUrl::RemoveAuthority).mid(1);
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
               provider.requestImage(previewImageId(root + QStringLiteral("/absent.wpx"), QStringLiteral("preview.png"), stamp), &none, QSize()).isNull(),
           QStringLiteral("S15.45: an address the provider cannot serve gave an image"));
}

// S15.47
void defaultDirectory(const QString& root) {
    // GOAL: S15.47 -- the library folder is $XDG_DATA_HOME/wallpaperx, so a test or a user with a
    //       moved data folder never reads ~/.local/share.
    // CONTROL: app/library/wpx_package.cpp, `GenericDataLocation` made `HomeLocation` -> red,
    //       "defaultLibraryDirectory() is /home/.../wallpaperx"; restored, green.
    const QString want = root + QStringLiteral("/XDG_DATA_HOME/wallpaperx");
    expect(defaultLibraryDirectory() == want,
           QStringLiteral("S15.47: defaultLibraryDirectory() is %1, not %2").arg(defaultLibraryDirectory(), want));
}

} // namespace

int main(int argc, char* argv[]) {
    wallpaperx::testing::g_testName = "wpx_package";
    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        notVerified("S15.31-S15.35, S15.45, S15.47, S15.48, S15.50, S15.51, S15.53, S15.55, S15.58", "no temporary folder could be made");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    isolateXdg(scratch.path());
    QCoreApplication app(argc, argv);

    const QString root = scratch.path();
    runPhase("manifestOrderAndFields", manifestOrderAndFields);
    runPhase("malformedManifests", malformedManifests);
    runPhase("acceptValues", acceptValues);
    runPhase("badArchives", [&] { badArchives(root); });
    runPhase("scanFolder", [&] { scanFolder(root); });
    runPhase("fileNamesThatAreNotUtf8", [&] { fileNamesThatAreNotUtf8(root); });
    runPhase("emptyFiles", [&] { emptyFiles(root); });
    runPhase("turnedPreview", [&] { turnedPreview(root); });
    runPhase("previewAddresses", [&] { previewAddresses(root); });
    runPhase("defaultDirectory", [&] { defaultDirectory(root); });

    const bool failed = wallpaperx::testing::reportCheckFailures();
    const bool faulted = wallpaperx::testing::reportHarnessFaults("in this test file's fixtures, not in the app");
    if (failed || faulted) {
        return 1;
    }
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
