// SPDX-License-Identifier: GPL-2.0-or-later

// The shared fault classifier -- renderer/vulkan/gpu_fault.cpp -- on its own.
//
// Which exception a failed driver call is raised with IS the diagnosis an operator reads:
// the daemon latches its recorded GPU health to failed for a GpuFailure and leaves the
// record alone for anything else. This binary grades that decision directly, with no
// device, which makes it total -- every result the classifier can be handed, not the two or
// three a fault injector can reach -- and lets it run on a host with no GPU at all.
//
// What it deliberately cannot show is that the decision reaches the daemon's published
// health. Two arms in backend/tests/http_server_test.cpp do that, on the real render path.

#include "vulkan/gpu_fault.h"

#include "support/check.h"

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace {

using wallpaperx::GpuFailure;
using wallpaperx::ResourceExhausted;
using wallpaperx::WallpaperContentError;
using wallpaperx::testing::check;

// What the classifier is required to decide. `ResourceLimit` means the GPU's recorded health
// is left alone AND the caller is told it may be able to free something and ask again;
// `DriverFault` means the GpuFailure arm catches it and the health is latched to failed.
enum class Verdict { ResourceLimit, DriverFault };

// What it actually did. Folded into a value rather than asserted inside the catch blocks so
// that "threw nothing at all" has a name of its own -- inside a catch block that outcome is
// simply the absence of an assertion, which is indistinguishable from a pass.
enum class Observed { Nothing, GpuFault, ContentError, Exhausted, PlainRuntimeError, ForeignException };

const char* describe(Observed o) {
    switch (o) {
        case Observed::Nothing: return "returned without throwing";
        case Observed::GpuFault: return "GpuFailure";
        case Observed::ContentError: return "WallpaperContentError";
        case Observed::Exhausted: return "ResourceExhausted";
        case Observed::PlainRuntimeError: return "std::runtime_error";
        case Observed::ForeignException: return "some other std::exception";
    }
    return "unreachable";
}

Observed classify(VkResult result, std::string* message) {
    message->clear();
    try {
        wallpaperx::throwVkFailure("probe", result);
    } catch (const GpuFailure& e) {
        // Before std::runtime_error, which it derives from -- the order here mirrors the
        // daemon's own catch order, so a type this file reports is a type the daemon sees.
        *message = e.what();
        return Observed::GpuFault;
    } catch (const WallpaperContentError& e) {
        *message = e.what();
        return Observed::ContentError;
    } catch (const ResourceExhausted& e) {
        // Before std::runtime_error, which it derives from. Its own arm because the daemon
        // now acts on it: an open that raises it frees an idle video decode session and
        // tries once more, which a plain runtime_error does not get.
        *message = e.what();
        return Observed::Exhausted;
    } catch (const std::runtime_error& e) {
        *message = e.what();
        return Observed::PlainRuntimeError;
    } catch (const std::exception& e) {
        *message = e.what();
        return Observed::ForeignException;
    }
    return Observed::Nothing;
}

struct Case {
    VkResult result;
    Verdict expected;
    const char* why;
};

// Every result vkResultName() knows how to spell, plus two the build does not name at all.
//
// The non-error results (VK_NOT_READY, VK_TIMEOUT, VK_EVENT_SET, VK_EVENT_RESET,
// VK_INCOMPLETE) are DriverFault on purpose and it is not an oversight: every production
// caller compares against VK_SUCCESS, so one of these reaching the classifier means a call
// returned a code it was not permitted to return in that configuration -- a fence wait with
// no timeout coming back VK_TIMEOUT is a hung GPU, not a shortage of anything.
const std::vector<Case> kCases = {
    {VK_ERROR_OUT_OF_HOST_MEMORY, Verdict::ResourceLimit, "the host is out of memory"},
    {VK_ERROR_OUT_OF_DEVICE_MEMORY, Verdict::ResourceLimit, "the device is out of memory"},
    {VK_ERROR_OUT_OF_POOL_MEMORY, Verdict::ResourceLimit, "a descriptor pool is out of memory"},
    {VK_ERROR_FRAGMENTED_POOL, Verdict::ResourceLimit, "a descriptor pool is fragmented"},
    {VK_ERROR_FRAGMENTATION, Verdict::ResourceLimit, "the descriptor heap is fragmented"},
    {VK_ERROR_TOO_MANY_OBJECTS, Verdict::ResourceLimit, "an object-count limit was reached"},
    {VK_ERROR_MEMORY_MAP_FAILED, Verdict::ResourceLimit, "a mapping could not be placed"},

    {VK_ERROR_DEVICE_LOST, Verdict::DriverFault, "the device stopped working"},
    {VK_ERROR_INITIALIZATION_FAILED, Verdict::DriverFault, "the driver could not initialize the object"},
    {VK_ERROR_LAYER_NOT_PRESENT, Verdict::DriverFault, "a layer the build asked for is gone"},
    {VK_ERROR_EXTENSION_NOT_PRESENT, Verdict::DriverFault, "an extension the build asked for is gone"},
    {VK_ERROR_FEATURE_NOT_PRESENT, Verdict::DriverFault, "a feature the build asked for is gone"},
    {VK_ERROR_INCOMPATIBLE_DRIVER, Verdict::DriverFault, "no usable driver"},
    {VK_ERROR_FORMAT_NOT_SUPPORTED, Verdict::DriverFault, "the format was rejected"},
    {VK_ERROR_UNKNOWN, Verdict::DriverFault, "the driver will not say"},
    {VK_ERROR_INVALID_EXTERNAL_HANDLE, Verdict::DriverFault, "an exported handle was refused"},
    {VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS, Verdict::DriverFault, "a capture address was refused"},
    {VK_ERROR_VALIDATION_FAILED_EXT, Verdict::DriverFault, "validation rejected the call"},
    {VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT, Verdict::DriverFault, "a plane layout was refused"},
    {VK_NOT_READY, Verdict::DriverFault, "a non-error result the call may not return here"},
    {VK_TIMEOUT, Verdict::DriverFault, "a wait with no timeout timed out"},
    {VK_EVENT_SET, Verdict::DriverFault, "a non-error result the call may not return here"},
    {VK_EVENT_RESET, Verdict::DriverFault, "a non-error result the call may not return here"},
    {VK_INCOMPLETE, Verdict::DriverFault, "a non-error result the call may not return here"},
    {static_cast<VkResult>(-999), Verdict::DriverFault, "a negative code this build cannot name"},
    {static_cast<VkResult>(4242), Verdict::DriverFault, "a positive code this build cannot name"},
};

// GOAL: S2.15 -- every result in the resource-exhaustion set is raised as something the
//       daemon's GpuFailure arm does NOT catch, and every other result, including codes
//       this build cannot even name, is raised as GpuFailure. False if running the host out
//       of memory can make /health report the graphics driver as broken, and false the
//       other way if a lost device arrives as an unclassified fault nothing attributes.
// WHY-THIS-SHAPE: the assertion is on the exception *type*, matched in the daemon's own
//       catch order (GpuFailure before WallpaperContentError before runtime_error), not on
//       the message text. Matching on text is the obvious alternative and it grades the
//       wrong thing: the daemon dispatches on type, so a classifier that kept the wording
//       and swapped the type would pass a text check while every out-of-memory reported a
//       broken GPU. The table is swept rather than sampled because the property is "these
//       seven and nothing else", which a sample cannot express.
// CONTROL: the `isResourceExhaustion(result)` branch deleted from throwVkFailure in
//       renderer/vulkan/gpu_fault.cpp, so every failure becomes a GpuFailure -> seven reds
//       here, "FAIL: VK_ERROR_OUT_OF_HOST_MEMORY (the host is out of memory) is a resource
//       limit, so it must be raised as ResourceExhausted -- got GpuFailure" and one per
//       remaining limit code, exit 1. Reverted. Second, aimed the other way:
//       `case VK_ERROR_DEVICE_LOST:` added to isResourceExhaustion -> "FAIL:
//       VK_ERROR_DEVICE_LOST (the device stopped working) is the driver going wrong, so it
//       must be raised as GpuFailure -- got ResourceExhausted". Reverted. Third, for the
//       TYPE rather than the arm: that branch made to throw a plain std::runtime_error, which
//       is what it threw before the daemon learnt to retry an exhausted open -> the same
//       seven reds, "got std::runtime_error". Reverted. Fourth, for the two message checks:
//       the `context +` dropped from the message throwVkFailure builds -> 26 reds, "FAIL: ...
//       message names the calling context". Reverted. All four observed 2026-09-18.
void testClassification() {
    for (const Case& c : kCases) {
        std::string message;
        const Observed observed = classify(c.result, &message);
        const std::string named = wallpaperx::vkResultName(c.result);
        const std::string prefix = named + " (" + c.why + ") ";

        if (c.expected == Verdict::ResourceLimit) {
            // ResourceExhausted specifically, not "anything but GpuFailure": a caller that can
            // free something asks again on this type alone, so a limit raised as a plain
            // runtime_error would leave the GPU's health correctly alone and still refuse a
            // wallpaper the machine has room for.
            check(observed == Observed::Exhausted,
                  prefix + "is a resource limit, so it must be raised as ResourceExhausted -- got " +
                      describe(observed));
        } else {
            check(observed == Observed::GpuFault,
                  prefix + "is the driver going wrong, so it must be raised as GpuFailure -- got " +
                      describe(observed));
        }

        // The context is the only thing in the message that says which entry point failed,
        // and the result name the only thing that says what it failed with. Both end up in
        // the daemon's last_error verbatim.
        check(message.find("probe") != std::string::npos, prefix + "message names the calling context");
        // CONTROL (this line): renderer/vulkan/gpu_fault.cpp, throwVkFailure's message built from the
        //       result's number instead of vkResultName -> "FAIL: VK_ERROR_OUT_OF_HOST_MEMORY (the host
        //       is out of memory) message names the result" and one per named result. Reverted.
        check(message.find(named) != std::string::npos, prefix + "message names the result");
    }
}

// A code this build cannot name must still be readable. An integer in last_error names
// nothing, but it is at least unambiguous; an empty string or a fixed "unknown" is not.
// CONTROL: renderer/vulkan/gpu_fault.cpp vkResultName, each reverted: the DEVICE_LOST case spelled
//       "device lost" -> "FAIL: a named result is spelled out"; the default returning "unknown" ->
//       "FAIL: an unnamed result is reported as its number".
void testResultNaming() {
    check(wallpaperx::vkResultName(VK_ERROR_DEVICE_LOST) == "VK_ERROR_DEVICE_LOST",
          "a named result is spelled out");
    check(wallpaperx::vkResultName(static_cast<VkResult>(-999)) == "-999",
          "an unnamed result is reported as its number");
}

}  // namespace

int main() {
    testClassification();
    testResultNaming();

    if (wallpaperx::testing::reportFailures("gpu_fault_test") == 0) {
        std::cout << "gpu_fault_test: all checks passed (" << kCases.size() << " results classified)" << std::endl;
        return 0;
    }
    return 1;
}
