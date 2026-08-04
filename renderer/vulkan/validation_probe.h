#pragma once

// Read-side access to the Vulkan validation layer's warning counter.
//
// Separated from device.h because nothing in the shipped renderer, daemon, or app calls
// any of it -- every caller is a test. Leaving these three in device.h put test-only
// surface in a header every production translation unit includes, which is the inverse
// of the rule this project holds tests to (GOALS.md X.6: no test-only function, hook or
// field in a production header). The counter itself is production code and stays in
// device.cpp: the debug callback that increments it is part of how Device reports layer
// output. It is only the *readers* that are test-facing, and this is where they live.
//
// Implemented in device.cpp, next to the callback and the atomics they read, so the
// counter has exactly one definition and no accessor can drift from it.
//
// Nothing in renderer/, backend/daemon/, or app/ may include this header. A production
// file that needs one of these functions is a sign the counter should have been exposed
// through Device's real interface instead.

namespace wallpaperx {

// Zero the counter. Call before the region a test intends to judge -- warnings from an
// earlier phase would otherwise be attributed to it.
void resetValidationWarningCount();

// Validation warnings seen since the last reset.
int validationWarningCount();

// Whether a Device actually loaded VK_LAYER_KHRONOS_validation. Distinct from having
// asked for it: Device degrades silently when the layer package is not installed, so a
// test that treats validationWarningCount() == 0 as proof of GPU-side correctness must
// check this too, or it passes vacuously on any machine without the layers.
bool validationLayerActive();

}  // namespace wallpaperx
