// System-level test for Step 6 task 1: stands up the real DbusService on the
// session bus and drives it through a real sdbus-c++ proxy client (same process,
// separate connections -- messages still round-trip through the bus). Run under a
// private bus, e.g. `dbus-run-session -- ./wallpaperx-dbus-service-test`.
#include "daemon/dbus_service.h"

#include <sdbus-c++/sdbus-c++.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <regex>
#include <string>
#include <utility>
#include <vector>

using wallpaperx::DbusService;

namespace {

// Every <arg> of `method` in introspection XML, in document order, as
// (direction, "type:name"). Attribute order and whitespace inside the tag don't
// matter -- only the values, which are the wire contract.
std::vector<std::pair<std::string, std::string>> parseMethodArgs(const std::string& xml, const std::string& method) {
    std::vector<std::pair<std::string, std::string>> args;
    // [\s\S] rather than . -- the XML is multi-line and . does not match newline.
    const std::regex methodRe("<method name=\"" + method + "\">([\\s\\S]*?)</method>");
    std::smatch methodMatch;
    if (!std::regex_search(xml, methodMatch, methodRe)) {
        return args;
    }
    const std::string body = methodMatch[1].str();

    const std::regex argRe("<arg\\b[^>]*/>");
    const std::regex attrRe("(\\w+)=\"([^\"]*)\"");
    for (auto it = std::sregex_iterator(body.begin(), body.end(), argRe); it != std::sregex_iterator(); ++it) {
        const std::string tag = it->str();
        std::string type, name, direction;
        for (auto a = std::sregex_iterator(tag.begin(), tag.end(), attrRe); a != std::sregex_iterator(); ++a) {
            const std::string key = (*a)[1].str(), value = (*a)[2].str();
            if (key == "type") type = value;
            else if (key == "name") name = value;
            else if (key == "direction") direction = value;
        }
        args.emplace_back(direction, type + ":" + name);
    }
    return args;
}

}  // namespace

int main() {
    int failures = 0;
    auto check = [&](bool cond, const char* msg) {
        if (!cond) {
            std::fprintf(stderr, "FAIL: %s\n", msg);
            ++failures;
        }
    };

    // Service side: requests the name and serves on a background event loop.
    auto serviceConn = sdbus::createSessionBusConnection(sdbus::ServiceName{DbusService::kBusName});
    DbusService service(*serviceConn);
    serviceConn->enterEventLoopAsync();

    // Client side: its own connection + loop, managed by the proxy.
    auto proxy = sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName}, sdbus::ObjectPath{DbusService::kObjectPath});

    // GetStatus returns valid JSON reporting the daemon is running.
    std::string status;
    proxy->callMethod("GetStatus").onInterface(DbusService::kInterface).storeResultsTo(status);
    nlohmann::json json = nlohmann::json::parse(status, nullptr, /*allow_exceptions=*/false);
    check(!json.is_discarded(), "GetStatus returns parseable JSON");
    check(!json.is_discarded() && json.value("running", false), "GetStatus reports running=true");

    // SetWallpaper records the request; GetStatus reflects it.
    proxy->callMethod("SetWallpaper")
        .onInterface(DbusService::kInterface)
        .withArguments(std::string("static"), std::string("/tmp/wallpaperx-test.png"), int32_t{0});
    proxy->callMethod("GetStatus").onInterface(DbusService::kInterface).storeResultsTo(status);
    json = nlohmann::json::parse(status, nullptr, false);
    const bool reflected = !json.is_discarded() && json["wallpapers"].contains("0") &&
                           json["wallpapers"]["0"]["type"] == "static" &&
                           json["wallpapers"]["0"]["path"] == "/tmp/wallpaperx-test.png";
    check(reflected, "SetWallpaper is reflected in GetStatus");

    // GetFrameFd has no frame to hand out yet (renderer wired in task 2) -- it must
    // throw a clean DBus error, not return a bogus fd.
    bool threw = false;
    try {
        sdbus::UnixFd fd;
        uint32_t width = 0, height = 0;
        uint64_t memorySize = 0;
        int32_t handleType = 0;
        std::vector<sdbus::UnixFd> syncFds;
        uint64_t frameGeneration = 0;
        uint32_t drmFormat = 0;
        uint64_t drmModifier = 0;
        std::vector<sdbus::Struct<uint64_t, uint64_t>> planes;
        proxy->callMethod("GetFrameFd")
            .onInterface(DbusService::kInterface)
            .withArguments(int32_t{0})
            .storeResultsTo(fd, width, height, memorySize, handleType, syncFds, frameGeneration, drmFormat,
                            drmModifier, planes);
    } catch (const sdbus::Error&) {
        threw = true;
    }
    check(threw, "GetFrameFd throws a DBus error when no frame is available");

    // Lock the whole wire signature, every arg, in order. The KDE plugin (Step 12) is
    // generated against it, so a reordering, a type change or a dropped slot must fail
    // here rather than at plugin-import time -- a partial check would let width/height
    // be swapped and still report green.
    //
    // Parsed rather than substring-matched: matching sd-bus's emitted XML verbatim
    // would couple this test to libsystemd's attribute order and spacing, so a
    // formatting change there would look like a wire-format regression.
    std::string introspection;
    proxy->callMethod("Introspect").onInterface("org.freedesktop.DBus.Introspectable").storeResultsTo(introspection);

    const std::vector<std::pair<std::string, std::string>> expectedArgs = {
        {"in", "i:monitor_id"},   {"out", "h:fd"},
        {"out", "u:width"},       {"out", "u:height"},
        {"out", "t:memory_size"}, {"out", "i:handle_type"},
        {"out", "ah:sync_fds"},   {"out", "t:frame_generation"},
        {"out", "u:drm_format"},  {"out", "t:drm_modifier"},
        {"out", "a(tt):planes"},
    };
    const std::vector<std::pair<std::string, std::string>> actualArgs = parseMethodArgs(introspection, "GetFrameFd");
    check(actualArgs == expectedArgs, "GetFrameFd's full wire signature and arg order are unchanged");
    if (actualArgs != expectedArgs) {
        std::fprintf(stderr, "  expected:");
        for (const auto& [dir, arg] : expectedArgs) std::fprintf(stderr, " %s(%s)", arg.c_str(), dir.c_str());
        std::fprintf(stderr, "\n  actual:  ");
        for (const auto& [dir, arg] : actualArgs) std::fprintf(stderr, " %s(%s)", arg.c_str(), dir.c_str());
        std::fprintf(stderr, "\n");
    }

    if (failures == 0) {
        std::printf("dbus_service_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "dbus_service_test: %d check(s) failed\n", failures);
    return 1;
}
