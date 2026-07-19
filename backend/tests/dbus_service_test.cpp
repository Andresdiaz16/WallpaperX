// System-level test for Step 6 task 1: stands up the real DbusService on the
// session bus and drives it through a real sdbus-c++ proxy client (same process,
// separate connections -- messages still round-trip through the bus). Run under a
// private bus, e.g. `dbus-run-session -- ./wallpaperx-dbus-service-test`.
#include "daemon/dbus_service.h"

#include <sdbus-c++/sdbus-c++.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <string>

using wallpaperx::DbusService;

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
        proxy->callMethod("GetFrameFd")
            .onInterface(DbusService::kInterface)
            .withArguments(int32_t{0})
            .storeResultsTo(fd, width, height, memorySize, handleType);
    } catch (const sdbus::Error&) {
        threw = true;
    }
    check(threw, "GetFrameFd throws a DBus error when no frame is available");

    if (failures == 0) {
        std::printf("dbus_service_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "dbus_service_test: %d check(s) failed\n", failures);
    return 1;
}
