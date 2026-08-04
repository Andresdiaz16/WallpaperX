#pragma once

// Test-only observation of the renderer's Vulkan validation output.
//
// Nothing in renderer/vulkan/, backend/daemon/ or app/ may include this header, and
// nothing there needs to: the implementation reads only what production already prints
// to stderr and what the Vulkan loader already reports. There is no counter, no hook and
// no accessor anywhere in the production tree for these functions to reach into.
//
// It used to work the other way round. device.cpp kept a `g_validationWarningCount` that
// its debug callback incremented, plus three free functions that read it, existing
// solely so tests could assert "zero validation warnings" -- test-only state and
// test-only accessors compiled into the shipped renderer. Moving the *declarations* out
// to a test-facing header did not fix that; the state and the functions were still in
// production code, with a comment as the only boundary. The counting moved out here
// instead, where the build enforces it.

namespace wallpaperx {

// Discards everything observed so far. Call before the region under test when an earlier
// phase is expected to have produced warnings of its own.
void resetValidationWarningCount();

// Number of validation warnings the renderer has printed since the last reset.
//
// Synchronous with respect to anything already written to stderr: the call flushes and
// then blocks until the reader thread has drained past a marker written at call time, so
// a warning emitted immediately before this call is always included. Without that a
// check could sample the count while the line producing it was still in flight and pass
// on a race rather than on the absence of warnings.
int validationWarningCount();

// Whether VK_LAYER_KHRONOS_validation can actually be loaded here. Asked of the loader
// directly, so it does not depend on a Device having been built.
//
// The distinction matters: `Device` degrades silently when the layer package is absent,
// and the warning count is then 0 no matter what the GPU did. A suite whose only
// GPU-side oracle is that count is fully vacuous on such a host, and this is what lets a
// test say so rather than report green.
bool validationLayerActive();

}  // namespace wallpaperx
