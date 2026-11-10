// SPDX-License-Identifier: GPL-2.0-or-later

// wpxctl: the WallpaperX daemon from the command line, over io.wallpaperx.Daemon on the session bus.

#include "daemon/daemon_bus.h"
#include "daemon/frame_rate_setting.h"
#include "daemon/pause_mode.h"
#include "plasma_activation.h"
#include "playback_speed.h"
#include "wallpaper_manifest.h"
#include "wallpaper_position.h"

#include <QMimeDatabase>
#include <QString>

#include <sdbus-c++/sdbus-c++.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>

namespace {

namespace fs = std::filesystem;
namespace manifest = wallpaperx::manifest;
using nlohmann::json;
using manifest::Value;

enum Exit : int { kDone = 0, kRefused = 1, kUsage = 2, kNotRunning = 3, kTimedOut = 4 };

// How a command ends other than in success: the exit status and the sentence printed for it.
struct Failure {
    int status;
    std::string message;
};

[[noreturn]] void fail(int status, std::string message) { throw Failure{status, std::move(message)}; }

// ---- help ------------------------------------------------------------------------------------------

constexpr std::string_view kOverview = R"(Usage: wpxctl <command> [arguments]

Controls the WallpaperX daemon over the session bus.

Commands:
  status [--json]                  what every screen shows and how it is set
  monitors                         the screens the daemon knows, by connector and monitor number
  set <screen> <file|folder>       show an image, a video or a library wallpaper folder
  props <screen> key=value...      change the properties of the wallpaper a screen shows
  pause <screen> <mode>            when a screen's video pauses
  sound on|off [<screen>]          sound for every screen, or for one
  fps <n>|off [<screen>]           the most pictures a second a video is drawn at
  activate <screen>                make WallpaperX the Plasma wallpaper of a screen

A screen is its connector name (DP-3) or the daemon's monitor number (0).
"wpxctl <command> --help" describes one command.

Exit status: 0 done, 1 the daemon (or Plasma) refused, 2 a usage error, 3 the daemon (or Plasma's
desktop shell) is not running, 4 set --wait saw no picture in time, or the daemon cannot say when it
is drawn.
)";

std::string pauseHelp() {
    std::string text = "Usage: wpxctl pause <screen> <mode>\n\n";
    for (const wallpaperx::PauseModeNames& row : wallpaperx::kPauseModes) {
        text += std::format("  {:<12}{}\n", row.shortName, row.description);
    }
    return text;
}

const std::map<std::string_view, std::string> kCommandHelp = {
    {"status", R"(Usage: wpxctl status [--json]

Prints, for every screen the daemon knows: its connector, the wallpaper and its properties, whether
the screen is shown, its pause mode, sound and frame-rate settings, and the last error. --json prints
the daemon's own status document instead, for scripts.
)"},
    {"monitors", R"(Usage: wpxctl monitors

Lists the screens the daemon knows: each monitor number and the connector name it answers to.
)"},
    {"set", R"(Usage: wpxctl set <screen> <file|folder> [options]

Shows a file or a library wallpaper on a screen. A file is shown as an image or a video by what it
holds. A folder is a library wallpaper: its wallpaper.json gives the type, the file and the defaults
of its properties, which the options below override. A value outside what the wallpaper declares is
refused before anything is sent.

Options:
  --speed 0.5|1|2        a video's playback speed
  --volume 0-100         a video's volume, in percent
  --mute                 a video without sound
  --paused               a video held on its first picture
  --position MODE        cover, fit, center, tile (images only), stretch or free
  --wait                 return once the first picture is drawn, not when the daemon accepts
  --timeout SECONDS      how long --wait waits (10)
)"},
    {"props", R"(Usage: wpxctl props <screen> key=value...

Changes properties of the wallpaper a screen shows; a playing video carries on. Every other property
keeps its value, and "key=" with nothing after it removes one. A value is a number, true or false, a
#rrggbb colour, or a position mode. volume is in percent, as on set. A library wallpaper's values are
held to what its wallpaper.json declares.
)"},
    {"pause", pauseHelp()},
    {"sound", R"(Usage: wpxctl sound on|off [<screen>]

Switches sound on or off for every screen, or for one screen.
)"},
    {"fps", R"(Usage: wpxctl fps <n>|off [<screen>] [--keep-higher|--no-keep-higher]

Sets the most pictures a second a video is drawn at, for every screen or for one: 30, 60 or 120.
"off" drops a screen's own setting so it follows the one for every screen. --keep-higher draws at
the setting even where it is above what the screen refreshes at; --no-keep-higher stops that.
)"},
    {"activate", R"(Usage: wpxctl activate <screen>

Makes WallpaperX the Plasma wallpaper of a screen, on every activity, through Plasma's desktop shell.
)"},
};

// ---- arguments -------------------------------------------------------------------------------------

struct Arguments {
    std::vector<std::string> positional;
    // An option's value, empty for a switch.
    std::map<std::string, std::string> options;

    bool has(const std::string& name) const { return options.contains(name); }
};

// `args` past the command; `switches` and `valued` name the options it takes, `--name`. --help ends
// the run with the command's help.
Arguments parseArguments(std::string_view command, std::span<char*> args, const std::set<std::string>& switches,
                         const std::set<std::string>& valued) {
    Arguments parsed;
    bool optionsEnded = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string arg = args[i];
        if (optionsEnded || arg.size() < 2 || !arg.starts_with("-") || arg == "-") {
            parsed.positional.push_back(arg);
            continue;
        }
        if (arg == "--") {
            optionsEnded = true;
            continue;
        }
        if (arg == "--help" || arg == "-h") fail(kDone, std::string(kCommandHelp.at(command)));
        const std::size_t equals = arg.find('=');
        const std::string name = arg.substr(0, equals);
        if (switches.contains(name)) {
            if (equals != std::string::npos) fail(kUsage, name + " takes no value.");
            parsed.options[name];
        } else if (valued.contains(name)) {
            if (equals != std::string::npos) {
                parsed.options[name] = arg.substr(equals + 1);
            } else if (i + 1 < args.size()) {
                parsed.options[name] = args[++i];
            } else {
                fail(kUsage, name + " needs a value.");
            }
        } else {
            fail(kUsage, "wpxctl " + std::string(command) + " has no option " + name + ". See wpxctl " +
                             std::string(command) + " --help.");
        }
    }
    return parsed;
}

void expectPositional(std::string_view command, const Arguments& args, std::size_t least, std::size_t most) {
    if (args.positional.size() < least || args.positional.size() > most) {
        fail(kUsage, "Wrong arguments for wpxctl " + std::string(command) + ".\n" +
                         std::string(kCommandHelp.at(command)));
    }
}

std::optional<double> parseNumber(std::string_view text) {
    double value = 0.0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size() || !std::isfinite(value)) return std::nullopt;
    return value;
}

std::optional<int> parseIndex(std::string_view text) {
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc() || end != text.data() + text.size() || value < 0) return std::nullopt;
    return value;
}

// ---- the daemon ------------------------------------------------------------------------------------

bool nameIn(const std::string& name, std::initializer_list<std::string_view> names) {
    return std::ranges::find(names, name) != names.end();
}

bool unreachable(const std::string& name) {
    return nameIn(name, {"org.freedesktop.DBus.Error.ServiceUnknown", "org.freedesktop.DBus.Error.NameHasNoOwner"});
}

bool noAnswer(const std::string& name) {
    return nameIn(name, {"org.freedesktop.DBus.Error.NoReply", "org.freedesktop.DBus.Error.Timeout",
                         "org.freedesktop.DBus.Error.TimedOut", "org.freedesktop.DBus.Error.Disconnected"});
}

[[noreturn]] void daemonFailed(const std::string& method, const sdbus::Error& error) {
    const std::string name = error.getName();
    if (unreachable(name)) fail(kNotRunning, "The WallpaperX daemon is not running.");
    if (noAnswer(name)) fail(kNotRunning, "The WallpaperX daemon did not answer (" + name + ").");
    fail(kRefused, "The daemon refused " + method + ": " + error.getMessage() + " (" + name + ")");
}

// The daemon's object on the session bus. With `listen`, it runs its own event-loop thread, which
// FrameReady needs; otherwise nothing runs but the calls.
class Daemon {
public:
    explicit Daemon(bool listen = false) {
        try {
            m_proxy = listen ? sdbus::createProxy(sdbus::ServiceName{wallpaperx::kDaemonBusName},
                                                  sdbus::ObjectPath{wallpaperx::kDaemonObjectPath})
                             : sdbus::createProxy(sdbus::ServiceName{wallpaperx::kDaemonBusName},
                                                  sdbus::ObjectPath{wallpaperx::kDaemonObjectPath},
                                                  sdbus::dont_run_event_loop_thread);
        } catch (const sdbus::Error& e) {
            fail(kNotRunning, "There is no session bus to reach the daemon on: " + e.getMessage());
        }
    }

    sdbus::IProxy& proxy() { return *m_proxy; }

    template <class... Args>
    sdbus::MethodReply call(const std::string& method, const Args&... args) {
        try {
            sdbus::MethodCall message = m_proxy->createMethodCall(sdbus::InterfaceName{wallpaperx::kDaemonInterface}, sdbus::MethodName{method});
            (message << ... << args);
            return m_proxy->callMethod(message);
        } catch (const sdbus::Error& e) {
            daemonFailed(method, e);
        }
    }

    // GetStatus as the daemon sent it.
    std::string statusText() {
        std::string text;
        try {
            m_proxy->callMethod("GetStatus").onInterface(wallpaperx::kDaemonInterface).storeResultsTo(text);
        } catch (const sdbus::Error& e) {
            daemonFailed("GetStatus", e);
        }
        return text;
    }

    json status() {
        json parsed = json::parse(statusText(), nullptr, /*allow_exceptions=*/false);
        if (!parsed.is_object()) fail(kRefused, "The daemon answered GetStatus with something that is not its status.");
        return parsed;
    }

private:
    std::unique_ptr<sdbus::IProxy> m_proxy;
};

// `status[key]` when it is an object; an empty one otherwise.
const json& objectAt(const json& status, std::string_view key) {
    static const json empty = json::object();
    const auto it = status.find(key);
    return it != status.end() && it->is_object() ? *it : empty;
}

// Every monitor the status names anywhere, and the connector each answers to, for display.
struct Screens {
    std::set<int> monitors;
    std::map<int, std::string> connector;
};

Screens screensIn(const json& status) {
    Screens screens;
    for (const char* section : {"wallpapers", "pause"}) {
        for (const auto& [key, value] : objectAt(status, section).items()) {
            if (const auto id = parseIndex(key)) screens.monitors.insert(*id);
        }
    }
    for (const char* section : {"sound", "frame_rate"}) {
        for (const auto& [key, value] : objectAt(objectAt(status, section), "monitors").items()) {
            if (const auto id = parseIndex(key)) screens.monitors.insert(*id);
        }
    }
    for (const auto& [name, id] : objectAt(status, "outputs").items()) {
        if (!id.is_number_integer()) continue;
        screens.monitors.insert(id.get<int>());
        screens.connector[id.get<int>()] = name;
    }
    return screens;
}

std::string screenName(const Screens& screens, int monitor) {
    const auto it = screens.connector.find(monitor);
    return it == screens.connector.end() ? std::format("monitor {}", monitor)
                                         : std::format("{} (monitor {})", it->second, monitor);
}

// The monitor `screen` names: a connector the daemon knows, matched on the bytes it was given, or a
// monitor number.
int resolveScreen(const json& status, const std::string& screen) {
    const json& named = objectAt(status, "outputs_literal");
    if (const auto it = named.find(screen); it != named.end() && it->is_number_integer()) return it->get<int>();
    if (const auto index = parseIndex(screen)) return *index;
    std::string known;
    for (const auto& [name, id] : objectAt(status, "outputs").items()) known += (known.empty() ? "" : ", ") + name;
    fail(kUsage, "The daemon knows no screen named " + screen + "." +
                     (known.empty() ? " It knows no connector names yet; use a monitor number."
                                    : " It knows " + known + "."));
}

// The connector `monitor` answers to, its bytes as they were set; nothing when it has none.
std::optional<std::string> connectorOf(const json& status, int monitor) {
    for (const auto& [name, id] : objectAt(status, "outputs_literal").items()) {
        if (id.is_number_integer() && id.get<int>() == monitor) return name;
    }
    return std::nullopt;
}

std::map<std::string, sdbus::Variant> toWire(const std::map<std::string, Value>& values) {
    std::map<std::string, sdbus::Variant> wire;
    for (const auto& [key, value] : values) {
        std::visit([&wire, &key](const auto& v) { wire.emplace(key, sdbus::Variant(v)); }, value);
    }
    return wire;
}

// ---- status and monitors ---------------------------------------------------------------------------

// A volume of 0 to 1 as the percentage wpxctl shows and takes, to hundredths: 0.2 reads 20.
std::string percent(double fraction) { return std::format("{}", std::round(fraction * 10000.0) / 100.0); }

std::string propertyText(const std::string& key, const json& value) {
    if (key == manifest::kVolumeKey && value.is_number()) return percent(value.get<double>()) + "%";
    if (value.is_number()) return std::format("{}", value.get<double>());
    if (value.is_string()) return value.get<std::string>();
    return value.dump();
}

std::string pauseText(const json& entry) {
    const json& mode = entry.contains("mode") ? entry["mode"] : json();
    const std::optional<wallpaperx::PauseMode> known =
        mode.is_string() ? wallpaperx::pauseModeFromName(mode.get<std::string>()) : std::nullopt;
    std::string text = known ? wallpaperx::toShortName(*known) : "not known to this wpxctl";
    if (entry.value("paused", false)) text += ", paused now";
    return text;
}

std::string onOff(const json& value, std::string_view unknown) {
    return value.is_boolean() ? (value.get<bool>() ? "on" : "off") : std::string(unknown);
}

int runStatus(std::span<char*> args) {
    const Arguments parsed = parseArguments("status", args, {"--json"}, {});
    expectPositional("status", parsed, 0, 0);
    Daemon daemon;
    if (parsed.has("--json")) {
        std::printf("%s\n", daemon.statusText().c_str());
        return kDone;
    }
    const json status = daemon.status();
    const Screens screens = screensIn(status);
    const json& wallpapers = objectAt(status, "wallpapers");
    const json& sound = objectAt(status, "sound");
    const json& frameRate = objectAt(status, "frame_rate");

    for (const int monitor : screens.monitors) {
        const std::string key = std::to_string(monitor);
        std::printf("%s\n", screenName(screens, monitor).c_str());
        const json& entry = objectAt(wallpapers, key);
        if (entry.empty()) {
            std::printf("  wallpaper: none\n");
        } else {
            std::printf("  wallpaper: %s %s\n", entry.value("type", std::string("?")).c_str(),
                        entry.value("path", std::string()).c_str());
            std::string properties;
            for (const auto& [name, value] : objectAt(entry, "properties").items()) {
                properties += (properties.empty() ? "" : ", ") + name + "=" + propertyText(name, value);
            }
            if (!properties.empty()) std::printf("  properties: %s\n", properties.c_str());
            std::string drawn = entry.value("has_frame", false)
                                    ? std::format("yes, {}x{}, picture {}", entry.value("width", 0),
                                                  entry.value("height", 0), entry.value("generation", 0))
                                    : std::string("not yet");
            std::printf("  drawn: %s\n", drawn.c_str());
            std::printf("  screen shown: %s\n", entry.value("screen_visible", true) ? "yes" : "no, hidden");
            if (entry.value("unsupported", false)) std::printf("  error: this daemon cannot draw this type\n");
            if (entry.contains("last_error")) {
                std::printf("  error: %s\n", entry.value("last_error", std::string()).c_str());
            }
        }
        if (const json& pause = objectAt(objectAt(status, "pause"), key); !pause.empty()) {
            std::printf("  pause: %s\n", pauseText(pause).c_str());
        }
        if (const json& own = objectAt(objectAt(sound, "monitors"), key); !own.empty()) {
            std::printf("  sound: %s%s\n", onOff(own.contains("enabled") ? own["enabled"] : json(), "on").c_str(),
                        own.value("heard", false) ? ", heard" : "");
        }
        if (const json& own = objectAt(objectAt(frameRate, "monitors"), key); !own.empty()) {
            const json& rate = own.contains("rate") ? own["rate"] : json();
            const std::string setting = own.value("follows_global", true) ? std::string("as every screen")
                                        : rate.is_number_integer()        ? std::format("{} a second", rate.get<int>())
                                                                          : std::string("not known to this wpxctl");
            std::printf("  frame rate: %s, drawn at up to %d a second\n", setting.c_str(), own.value("target", 0));
        }
    }

    const json& rate = frameRate.contains("rate") ? frameRate["rate"] : json();
    const json& keep = frameRate.contains("keep_higher") ? frameRate["keep_higher"] : json();
    std::printf("Every screen: sound %s, frame rate %s, %s\n",
                onOff(sound.contains("enabled") ? sound["enabled"] : json(), "on").c_str(),
                rate.is_number_integer() ? std::format("{} a second", rate.get<int>()).c_str() : "not known",
                keep.is_boolean() && keep.get<bool>() ? "kept above a screen's refresh rate"
                                                      : "held to a screen's refresh rate");
    const json& gpu = objectAt(status, "gpu");
    if (gpu.value("initialized", false)) {
        std::printf("GPU: %s (%s)\n", gpu.value("name", std::string()).c_str(),
                    gpu.value("descriptor_tier", std::string()).c_str());
    } else {
        std::printf("GPU: not started yet\n");
    }
    const json& persistence = objectAt(status, "persistence");
    if (const std::string state = persistence.value("status", std::string("ok")); state != "ok") {
        std::printf("Saving settings: %s, %s\n", state.c_str(), persistence.value("detail", std::string()).c_str());
    }
    return kDone;
}

int runMonitors(std::span<char*> args) {
    const Arguments parsed = parseArguments("monitors", args, {}, {});
    expectPositional("monitors", parsed, 0, 0);
    Daemon daemon;
    const Screens screens = screensIn(daemon.status());
    if (screens.monitors.empty()) std::printf("The daemon knows no screens yet.\n");
    for (const int monitor : screens.monitors) {
        const auto it = screens.connector.find(monitor);
        std::printf("%d\t%s\n", monitor, it == screens.connector.end() ? "(no connector name)" : it->second.c_str());
    }
    return kDone;
}

// ---- set and props ---------------------------------------------------------------------------------

// "static" or "video" by what the file holds, through the shared MIME database.
std::string fileType(const fs::path& file) {
    const QString name = QMimeDatabase().mimeTypeForFile(QString::fromStdString(file.string())).name();
    if (name.startsWith(QStringLiteral("image/"))) return "static";
    if (name.startsWith(QStringLiteral("video/"))) return "video";
    fail(kUsage, file.string() + " is neither an image nor a video (it reads as " + name.toStdString() + ").");
}

// A library wallpaper folder, read by the app's own rules.
struct Library {
    manifest::Manifest wallpaper;
    // The file the daemon opens: the folder's path, then the name wallpaper.json gives.
    std::string asset;
};

// wpxctl does not ask a video whether it has sound, which needs the app's FFmpeg probe: every video is
// taken to have it, and so has the sound controls the app gives a video with sound.
constexpr wallpaperx::AudioStreams kVideoAudio = wallpaperx::AudioStreams::Present;
wallpaperx::AudioStreams takenToHaveSound(const manifest::Read&) { return kVideoAudio; }

// The folder named, a link to one followed: unlike the app's library scan, it is the one the user chose.
// What is inside is then read beneath it, by the shared reader's rules.
std::expected<wallpaperx::UniqueFd, std::string> openFolder(const fs::path& folder) {
    wallpaperx::UniqueFd dir(::open(folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!dir) {
        const int error = errno;
        return std::unexpected(error == ENOTDIR ? std::string("It is not a folder.")
                                                : "It cannot be opened: " + manifest::systemError(error) + ".");
    }
    return dir;
}

// The whole folder checked, as the app's library checks it before it can be set. The path sent is
// `folder` as given, its links not resolved.
std::expected<Library, std::string> readLibrary(const fs::path& folder) {
    const auto dir = openFolder(folder);
    if (!dir) return std::unexpected(dir.error());
    auto read = manifest::readFolder(dir->get(), ".", folder.string(), takenToHaveSound);
    if (!read) return std::unexpected(std::move(read.error().text));
    return Library{std::move(read->read.manifest), std::move(read->assetPath)};
}

// The library wallpaper `asset` is the file of: the nearest folder above it holding a wallpaper.json,
// when that names it; nothing for a plain file. Only wallpaper.json is read, since the wallpaper is
// already set and its rules are all that is asked. One that names `asset` but breaks the rules ends the
// run, as values sent unchecked against it would not be the wallpaper's; one that cannot be read, or
// names no such file, leaves `asset` a plain file, and says so on stderr.
std::optional<Library> libraryOfAsset(const std::string& asset) {
    const fs::path file(asset);
    if (!file.is_absolute()) return std::nullopt;
    std::error_code error;
    for (fs::path folder = file.parent_path();; folder = folder.parent_path()) {
        const fs::path found = folder / manifest::kManifestName;
        if (fs::exists(found, error)) {
            const auto plain = [&found](const std::string& why) -> std::optional<Library> {
                std::fprintf(stderr, "wpxctl: %s is not taken as this wallpaper's: %s The values are checked as for "
                                     "a plain file.\n", found.c_str(), why.c_str());
                return std::nullopt;
            };
            const auto dir = openFolder(folder);
            if (!dir) return plain("Its folder cannot be read: " + dir.error());
            manifest::FolderContents contents(dir->get());
            const auto text = contents.read(manifest::kManifestName, manifest::kMaxManifestBytes);
            if (!text) return plain(text.error());
            const std::optional<std::string> named = manifest::assetNameIn(text->bytes);
            if (!named) return plain("It names no file that could be this one.");
            if ((folder / *named).string() != asset) return plain("It names " + *named + ", not this file.");

            const auto refuse = [&found](const std::string& why) {
                fail(kUsage, found.string() + " names this wallpaper's file, but the values cannot be checked "
                                 "against it: " + why + " Nothing was sent.");
            };
            auto parsed = manifest::parse(text->bytes);
            if (!parsed) refuse(parsed.error());
            if (const auto audio = manifest::settleSoundFor(*parsed, [] { return kVideoAudio; }); !audio) {
                refuse(audio.error());
            }
            return Library{std::move(*parsed), asset};
        }
        if (folder == folder.root_path()) return std::nullopt;
    }
}

std::string listed(const std::vector<double>& numbers) {
    std::string text;
    for (std::size_t i = 0; i < numbers.size(); ++i) {
        if (i > 0) text += i + 1 < numbers.size() ? ", " : " or ";
        text += std::format("{}", numbers[i]);
    }
    return text;
}

bool soundOrSpeed(std::string_view key) {
    return key == manifest::kSpeedKey || key == manifest::kMuteKey || key == manifest::kVolumeKey;
}

// Why `key` cannot be `value` on a wallpaper of `type`. "volume" is in the daemon's units, 0 to 1, and
// refused in percent, as wpxctl takes it.
std::string refusal(std::string_view type, const manifest::Manifest* library, const std::string& key,
                    const Value& value) {
    const bool video = type == "video";
    if (key == wallpaperx::kPositionKey) {
        const std::string* name = std::get_if<std::string>(&value);
        if (!name || !wallpaperx::positionModeFromName(*name)) {
            return "position must be " + wallpaperx::positionModesText("or") + ".";
        }
        return "position " + *name + " is for still images only.";
    }
    if (key == wallpaperx::kPositionZoomKey) {
        return std::format("position.zoom must be a number from {} to {}.", wallpaperx::kMinPositionZoom,
                           wallpaperx::kMaxPositionZoom);
    }
    if (manifest::isPositionKey(key)) {
        return std::format("{} must be a number from -{} to {}.", key, wallpaperx::kMaxPositionOffset,
                           wallpaperx::kMaxPositionOffset);
    }
    if (key == manifest::kPausedKey) {
        return video ? std::string("paused must be true or false.")
                     : "Only a video can be paused, and this wallpaper is " + std::string(type) + ".";
    }
    const manifest::Property* declared = library ? library->find(key) : nullptr;
    if (!declared && soundOrSpeed(key) && !video) {
        return key + " applies to a video only, and this wallpaper is " + std::string(type) + ".";
    }
    if (declared) {
        if (const std::vector<double> choices = declared->choices(); !choices.empty()) {
            return "This wallpaper plays at " + listed(choices) + ".";
        }
        if (key == manifest::kVolumeKey) {
            return "This wallpaper's volume goes from " + percent(declared->minimum) + " to " +
                   percent(declared->maximum) + ".";
        }
        switch (declared->type) {
            case manifest::Property::Type::Float:
                return std::format("{} goes from {} to {} on this wallpaper.", key, declared->minimum,
                                   declared->maximum);
            case manifest::Property::Type::Bool: return key + " must be true or false.";
            case manifest::Property::Type::Color: return key + " must be a colour written #rrggbb.";
        }
    }
    if (library) {
        std::string keys;
        for (const manifest::Property& property : library->properties) {
            keys += (keys.empty() ? "" : ", ") + property.key;
        }
        return "This wallpaper has no property \"" + key + "\"" +
               (keys.empty() ? std::string(".") : "; it has " + keys + ", besides position.");
    }
    if (key == manifest::kSpeedKey) return "speed must be " + wallpaperx::playbackSpeedsText("or") + ".";
    if (key == manifest::kMuteKey) return "mute must be true or false.";
    return "volume must be from 0 to 100.";
}

// What a plain file's video takes for the keys the daemon gives a meaning; any other key is the daemon's
// to carry.
std::optional<Value> plainVideoValue(const std::string& key, const Value& value) {
    const double* number = std::get_if<double>(&value);
    if (key == manifest::kSpeedKey) {
        const bool known = number && std::ranges::find(wallpaperx::kPlaybackSpeeds, *number) != wallpaperx::kPlaybackSpeeds.end();
        return known ? std::optional(value) : std::nullopt;
    }
    if (key == manifest::kMuteKey) return std::holds_alternative<bool>(value) ? std::optional(value) : std::nullopt;
    const bool audible = number && std::isfinite(*number) && *number >= 0.0 && *number <= manifest::kMaxVolume;
    return audible ? std::optional(value) : std::nullopt;
}

// `value` as it is sent for `key`, or the run ends with the reason. A library wallpaper's values are
// held to the app's rules for it; a plain file's to the daemon's, as is the speed of a library video
// that declares none.
Value allowedValue(std::string_view type, const Library* library, const std::string& key, const Value& value) {
    const manifest::Manifest* rules = library ? &library->wallpaper : nullptr;
    if (rules && key == manifest::kSpeedKey && type == "video" && !rules->find(key)) rules = nullptr;
    std::optional<Value> kept;
    if (rules) {
        kept = manifest::acceptValue(type, rules->find(key), key, value);
    } else if (key == manifest::kPausedKey || manifest::isPositionKey(key)) {
        kept = manifest::acceptValue(type, nullptr, key, value);
    } else if (!soundOrSpeed(key)) {
        kept = value;
    } else if (type == "video") {
        kept = plainVideoValue(key, value);
    }
    if (!kept) fail(kUsage, refusal(type, rules, key, value) + " Nothing was sent.");
    return std::move(*kept);
}

// The newest picture FrameReady announced for each monitor, counted from before the request is sent:
// the request's first picture can be announced before its reply arrives.
class FrameWatch {
public:
    // Shared with the handler, which runs on the proxy's thread until the proxy is destroyed.
    explicit FrameWatch(sdbus::IProxy& proxy) : m_state(std::make_shared<State>()) {
        proxy.uponSignal("FrameReady")
            .onInterface(wallpaperx::kDaemonInterface)
            .call([state = m_state](int32_t monitorId, uint64_t generation) {
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    uint64_t& newest = state->newest[monitorId];
                    newest = std::max(newest, generation);
                }
                state->arrived.notify_all();
            });
    }

    // `accepted` is the generation the daemon answered the request with: no picture of an earlier
    // request is numbered above it.
    std::optional<uint64_t> wait(int monitor, uint64_t accepted, std::chrono::milliseconds timeout) {
        State& s = *m_state;
        std::unique_lock<std::mutex> lock(s.mutex);
        if (!s.arrived.wait_for(lock, timeout, [&] { return s.newest[monitor] > accepted; })) return std::nullopt;
        return s.newest[monitor];
    }

private:
    struct State {
        std::mutex mutex;
        std::condition_variable arrived;
        std::map<int, uint64_t> newest;
    };
    std::shared_ptr<State> m_state;
};

// The frame generation a SetWallpaper* reply carries; nothing from a daemon older than this wpxctl,
// whose reply is empty.
std::optional<uint64_t> acceptedGeneration(sdbus::MethodReply& reply) {
    if (reply.isEmpty() || reply.peekType().first != 't') return std::nullopt;
    uint64_t generation = 0;
    reply >> generation;
    return generation;
}

int awaitPicture(Daemon& daemon, FrameWatch& watch, int monitor, uint64_t accepted, const std::string& where,
                 double seconds) {
    const auto timeout = std::chrono::milliseconds(static_cast<long long>(seconds * 1000.0));
    if (const std::optional<uint64_t> picture = watch.wait(monitor, accepted, timeout)) {
        std::printf("Drawn on %s: picture %llu.\n", where.c_str(), static_cast<unsigned long long>(*picture));
        return kDone;
    }
    const json entry = objectAt(objectAt(daemon.status(), "wallpapers"), std::to_string(monitor));
    if (entry.contains("last_error")) {
        fail(kRefused, "The daemon could not draw it: " + entry.value("last_error", std::string()));
    }
    if (entry.value("unsupported", false)) fail(kRefused, "The daemon cannot draw a wallpaper of this type.");
    fail(kTimedOut, std::format("No picture was drawn on {} within {} seconds.", where, seconds));
}

int runSet(std::span<char*> args) {
    const Arguments parsed =
        parseArguments("set", args, {"--mute", "--paused", "--wait"}, {"--speed", "--volume", "--position", "--timeout"});
    expectPositional("set", parsed, 2, 2);
    const std::string& screen = parsed.positional[0];
    const fs::path given = parsed.positional[1];

    double timeout = 10.0;
    if (parsed.has("--timeout")) {
        const auto value = parseNumber(parsed.options.at("--timeout"));
        if (!value || *value <= 0.0 || *value > 3600.0) fail(kUsage, "--timeout takes a number of seconds, up to 3600.");
        timeout = *value;
    }

    // Absolute and without "." or "..", but links not resolved: the path the app's library knows it by.
    // It is the one checked, read and sent, so "link/.." means the folder its text names.
    std::error_code error;
    fs::path where = fs::absolute(given, error);
    if (error) fail(kUsage, "\"" + given.string() + "\" cannot be set: " + error.message() + ".");
    where = where.lexically_normal();
    if (!where.has_filename()) where = where.parent_path();

    std::optional<Library> library;
    std::string type;
    std::string asset;
    std::map<std::string, Value> values;
    if (fs::is_directory(where, error)) {
        auto read = readLibrary(where);
        if (!read) fail(kUsage, given.string() + " cannot be set: " + read.error());
        library = std::move(*read);
        type = library->wallpaper.type;
        asset = library->asset;
        for (const manifest::Property& property : library->wallpaper.properties) values[property.key] = property.fallback;
    } else if (fs::is_regular_file(where, error)) {
        type = fileType(where);
        asset = where.string();
        // The daemon takes the path as a D-Bus string.
        if (!manifest::isValidUtf8(asset)) {
            fail(kUsage, given.string() + " cannot be set as a wallpaper: its path is not valid UTF-8. Rename the file.");
        }
    } else {
        fail(kUsage, given.string() + " is neither a file nor a wallpaper folder.");
    }

    if (parsed.has("--speed")) {
        const auto speed = parseNumber(parsed.options.at("--speed"));
        if (!speed || std::ranges::find(wallpaperx::kPlaybackSpeeds, *speed) == wallpaperx::kPlaybackSpeeds.end()) {
            fail(kUsage, "--speed takes " + wallpaperx::playbackSpeedsText("or") + ".");
        }
        values[manifest::kSpeedKey] = *speed;
    }
    if (parsed.has("--volume")) {
        const auto volume = parseNumber(parsed.options.at("--volume"));
        if (!volume || *volume < 0.0 || *volume > 100.0) fail(kUsage, "--volume takes a number from 0 to 100.");
        values[manifest::kVolumeKey] = *volume / 100.0;
    }
    if (parsed.has("--mute")) values[manifest::kMuteKey] = true;
    if (parsed.has("--paused")) values[manifest::kPausedKey] = true;
    if (parsed.has("--position")) values[std::string(wallpaperx::kPositionKey)] = parsed.options.at("--position");
    for (auto& [key, value] : values) value = allowedValue(type, library ? &*library : nullptr, key, value);

    Daemon daemon(parsed.has("--wait"));
    std::optional<FrameWatch> watch;
    if (parsed.has("--wait")) watch.emplace(daemon.proxy());
    const json status = daemon.status();
    const int monitor = resolveScreen(status, screen);
    const std::string screenText = screenName(screensIn(status), monitor);

    sdbus::MethodReply reply =
        daemon.call("SetWallpaperWithProperties", type, asset, static_cast<int32_t>(monitor), toWire(values));
    std::printf("Set %s on %s.\n", asset.c_str(), screenText.c_str());
    if (!watch) return kDone;
    std::fflush(stdout);
    const std::optional<uint64_t> accepted = acceptedGeneration(reply);
    if (!accepted) {
        fail(kTimedOut, "The wallpaper was applied, but this daemon cannot report when it is drawn: it is older than "
                        "this wpxctl, and its answer does not say which picture it took the request at.");
    }
    return awaitPicture(daemon, *watch, monitor, *accepted, screenText, timeout);
}

// A value as the command line writes it.
std::optional<Value> parseValue(const std::string& text) {
    if (text == "true") return Value(true);
    if (text == "false") return Value(false);
    if (const auto number = parseNumber(text)) return Value(*number);
    if (text.starts_with('#') || wallpaperx::positionModeFromName(text)) return Value(text);
    return std::nullopt;
}

std::optional<Value> fromJson(const json& value) {
    if (value.is_boolean()) return Value(value.get<bool>());
    if (value.is_number()) return Value(value.get<double>());
    if (value.is_string()) return Value(value.get<std::string>());
    return std::nullopt;
}

int runProps(std::span<char*> args) {
    const Arguments parsed = parseArguments("props", args, {}, {});
    expectPositional("props", parsed, 2, SIZE_MAX);
    Daemon daemon;
    const json status = daemon.status();
    const int monitor = resolveScreen(status, parsed.positional[0]);
    const json entry = objectAt(objectAt(status, "wallpapers"), std::to_string(monitor));
    const std::string type = entry.value("type", std::string());

    std::optional<Library> library;
    if (const auto path = entry.find("path_literal"); path != entry.end() && path->is_string()) {
        library = libraryOfAsset(path->get<std::string>());
    }

    std::map<std::string, Value> values;
    for (const auto& [key, value] : objectAt(entry, "properties").items()) {
        if (const auto read = fromJson(value)) values[key] = *read;
    }
    for (std::size_t i = 1; i < parsed.positional.size(); ++i) {
        const std::string& arg = parsed.positional[i];
        const std::size_t equals = arg.find('=');
        if (equals == std::string::npos || equals == 0) fail(kUsage, "\"" + arg + "\" is not key=value.");
        const std::string key = arg.substr(0, equals);
        const std::string text = arg.substr(equals + 1);
        if (text.empty()) {
            values.erase(key);
            continue;
        }
        std::optional<Value> value = parseValue(text);
        if (!value) {
            fail(kUsage, "The value of " + key + ", \"" + text +
                             "\", is not a number, true or false, a #rrggbb colour or a position mode.");
        }
        if (key == manifest::kVolumeKey && std::holds_alternative<double>(*value)) {
            *value = std::get<double>(*value) / 100.0;
        }
        // A monitor holding nothing has no type to judge by; the daemon refuses the call itself.
        if (!entry.empty()) *value = allowedValue(type, library ? &*library : nullptr, key, *value);
        values[key] = std::move(*value);
    }

    daemon.call("SetWallpaperProperties", static_cast<int32_t>(monitor), toWire(values));
    std::printf("Properties set on %s.\n", screenName(screensIn(status), monitor).c_str());
    return kDone;
}

// ---- settings --------------------------------------------------------------------------------------

int runPause(std::span<char*> args) {
    const Arguments parsed = parseArguments("pause", args, {}, {});
    expectPositional("pause", parsed, 2, 2);
    const std::optional<wallpaperx::PauseMode> mode = wallpaperx::pauseModeFromShortName(parsed.positional[1]);
    if (!mode) {
        fail(kUsage, "\"" + parsed.positional[1] + "\" is not a pause mode: " +
                         wallpaperx::pauseModesText(&wallpaperx::PauseModeNames::shortName) + ".");
    }
    Daemon daemon;
    const json status = daemon.status();
    const int monitor = resolveScreen(status, parsed.positional[0]);
    daemon.call("SetMonitorPause", static_cast<int32_t>(monitor), std::string(wallpaperx::toString(*mode)));
    std::printf("Pause set to %s on %s.\n", wallpaperx::toShortName(*mode), screenName(screensIn(status), monitor).c_str());
    return kDone;
}

bool parseOnOff(const std::string& text) {
    if (text == "on") return true;
    if (text == "off") return false;
    fail(kUsage, "\"" + text + "\" is not on or off.");
}

int runSound(std::span<char*> args) {
    const Arguments parsed = parseArguments("sound", args, {}, {});
    expectPositional("sound", parsed, 1, 2);
    const bool on = parseOnOff(parsed.positional[0]);
    Daemon daemon;
    if (parsed.positional.size() == 1) {
        daemon.call("SetSound", on);
        std::printf("Sound %s for every screen.\n", on ? "on" : "off");
        return kDone;
    }
    const json status = daemon.status();
    const int monitor = resolveScreen(status, parsed.positional[1]);
    daemon.call("SetMonitorSound", static_cast<int32_t>(monitor), on);
    std::printf("Sound %s on %s.\n", on ? "on" : "off", screenName(screensIn(status), monitor).c_str());
    return kDone;
}

int runFps(std::span<char*> args) {
    const Arguments parsed = parseArguments("fps", args, {"--keep-higher", "--no-keep-higher"}, {});
    expectPositional("fps", parsed, 1, 2);
    if (parsed.has("--keep-higher") && parsed.has("--no-keep-higher")) {
        fail(kUsage, "--keep-higher and --no-keep-higher cannot both be given.");
    }
    const std::string& given = parsed.positional[0];
    const bool off = given == "off";
    const std::optional<int> rate = off ? std::optional<int>(0) : parseIndex(given);
    if (!rate || (!off && !wallpaperx::isFrameRate(*rate))) {
        fail(kUsage, "\"" + given + "\" is not a frame rate: " + wallpaperx::frameRateChoices() + ", or off.");
    }
    if (off && parsed.positional.size() == 1) {
        fail(kUsage, "off drops one screen's own frame rate; name the screen. The rate for every screen is " +
                         wallpaperx::frameRateChoices() + ".");
    }

    Daemon daemon;
    if (parsed.positional.size() == 1) {
        daemon.call("SetFrameRate", static_cast<int32_t>(*rate));
        std::printf("Frame rate %d a second for every screen.\n", *rate);
    } else {
        const json status = daemon.status();
        const int monitor = resolveScreen(status, parsed.positional[1]);
        daemon.call("SetMonitorFrameRate", static_cast<int32_t>(monitor), static_cast<int32_t>(*rate));
        const std::string where = screenName(screensIn(status), monitor);
        if (off) {
            std::printf("%s follows the frame rate for every screen.\n", where.c_str());
        } else {
            std::printf("Frame rate %d a second on %s.\n", *rate, where.c_str());
        }
    }
    if (parsed.has("--keep-higher") || parsed.has("--no-keep-higher")) {
        const bool keep = parsed.has("--keep-higher");
        daemon.call("SetKeepHigherFrameRate", keep);
        std::printf("%s\n", keep ? "Rates above a screen's refresh rate are kept."
                                 : "Rates are held to each screen's refresh rate.");
    }
    return kDone;
}

// ---- activate --------------------------------------------------------------------------------------

std::string firstLine(const std::string& text) { return text.substr(0, text.find('\n')); }

int runActivate(std::span<char*> args) {
    const Arguments parsed = parseArguments("activate", args, {}, {});
    expectPositional("activate", parsed, 1, 1);
    std::string connector = parsed.positional[0];
    // A monitor number is the daemon's; Plasma knows a screen only by its connector, as it was set.
    if (const auto index = parseIndex(connector)) {
        Daemon daemon;
        std::optional<std::string> named = connectorOf(daemon.status(), *index);
        if (!named) {
            fail(kUsage, std::format("Monitor {} has no connector name the daemon knows; name the screen by its "
                                     "connector instead.", *index));
        }
        connector = std::move(*named);
    }

    std::unique_ptr<sdbus::IProxy> shell;
    try {
        shell = sdbus::createProxy(sdbus::ServiceName{wallpaperx::kPlasmaShellService},
                                   sdbus::ObjectPath{wallpaperx::kPlasmaShellPath}, sdbus::dont_run_event_loop_thread);
    } catch (const sdbus::Error& e) {
        fail(kNotRunning, "There is no session bus to reach Plasma on: " + e.getMessage());
    }
    const std::u16string wide = QString::fromStdString(connector).toStdU16String();
    std::string printed;
    try {
        shell->callMethod("evaluateScript")
            .onInterface(wallpaperx::kPlasmaShellInterface)
            .withTimeout(std::chrono::milliseconds(wallpaperx::kPlasmaScriptTimeoutMs))
            .withArguments(wallpaperx::plasmaActivationScript(wide))
            .storeResultsTo(printed);
    } catch (const sdbus::Error& e) {
        const std::string name = e.getName();
        if (unreachable(name)) fail(kNotRunning, "Plasma's desktop shell (plasmashell) is not running.");
        if (noAnswer(name)) fail(kNotRunning, "Plasma's desktop shell did not answer.");
        if (nameIn(name, {"org.freedesktop.DBus.Error.UnknownMethod", "org.freedesktop.DBus.Error.UnknownObject",
                          "org.freedesktop.DBus.Error.UnknownInterface"})) {
            fail(kRefused, "The desktop shell does not take Plasma's desktop scripts.");
        }
        if (const auto thrown = wallpaperx::plasmaActivationFailure(e.getMessage())) fail(kRefused, *thrown + ".");
        fail(kRefused, "Plasma could not run the script that switches the desktop: " + firstLine(e.getMessage()));
    }
    const int switched = wallpaperx::plasmaActivationSwitched(printed);
    if (switched < 1) fail(kRefused, "Plasma ran the script that switches the desktop but did not say it had.");
    std::printf("WallpaperX is the wallpaper of %d desktop%s on %s.\n", switched, switched == 1 ? "" : "s",
                connector.c_str());
    return kDone;
}

}  // namespace

int main(int argc, char** argv) {
    const std::span<char*> all(argv, static_cast<std::size_t>(argc));
    const std::map<std::string_view, std::function<int(std::span<char*>)>> commands = {
        {"status", runStatus}, {"monitors", runMonitors}, {"set", runSet},       {"props", runProps},
        {"pause", runPause},   {"sound", runSound},       {"fps", runFps},       {"activate", runActivate},
    };
    try {
        if (all.size() < 2) fail(kUsage, std::string(kOverview));
        const std::string_view command = all[1];
        if (command == "--help" || command == "-h" || command == "help") {
            std::fputs(kOverview.data(), stdout);
            return kDone;
        }
        const auto it = commands.find(command);
        if (it == commands.end()) {
            fail(kUsage, "wpxctl has no command " + std::string(command) + ".\n" + std::string(kOverview));
        }
        return it->second(all.subspan(2));
    } catch (const Failure& failure) {
        if (failure.status == kDone) {
            std::fputs(failure.message.c_str(), stdout);
        } else {
            std::fprintf(stderr, "wpxctl: %s\n", failure.message.c_str());
        }
        return failure.status;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "wpxctl: %s\n", e.what());
        return kRefused;
    }
}
