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
//
// HOW IT WORKS, because it is not free: linking this library redirects the process's
// stderr to a temporary file at static-initialisation time and copies it through to the
// real stderr whenever the count is read and at exit. There is no reader thread and no
// second thread of any kind -- an earlier version used one and deadlocked
// device_affinity_probe's fork() children in malloc. The consequence a caller must know
// is that stderr is *batched*: a line printed now reaches the terminal at the next
// validationWarningCount()/resetValidationWarningCount() call, at exit, or when
// drainValidationCapture() is called explicitly.

namespace wallpaperx {

// Discards everything observed so far. Call before the region under test when an earlier
// phase is expected to have produced warnings of its own.
//
// Aborts the process if the capture could not be installed -- see validationWarningCount().
void resetValidationWarningCount();

// Number of validation warnings the renderer has printed since the last reset.
//
// Counted on the calling thread: the call flushes stderr and reads the capture file to
// its end before returning, so a warning emitted immediately before this call is always
// included. There is no concurrent reader to synchronise with.
//
// If the capture could not be installed (no writable temporary directory, exhausted file
// descriptors), this does NOT return 0 -- it prints why and aborts. A zero returned from
// a capture that was never listening is indistinguishable from a clean run, and every
// caller of this function uses it as the suite's only GPU-side correctness oracle.
int validationWarningCount();

// Copies everything captured so far to the real stderr without counting it.
//
// Call before any deliberate abnormal exit -- `_Exit`, `abort`, a re-raised signal --
// because those run no destructors, so the capture's own flush at exit never happens and
// the whole run's stderr would be discarded with the temporary file. Safe to call from a
// signal handler: it allocates nothing and uses only pread/write.
void drainValidationCapture();

// Whether VK_LAYER_KHRONOS_validation can actually be loaded here. Asked of the loader
// directly, so it does not depend on a Device having been built.
//
// The distinction matters: `Device` degrades silently when the layer package is absent,
// and the warning count is then 0 no matter what the GPU did. A suite whose only
// GPU-side oracle is that count is fully vacuous on such a host, and this is what lets a
// test say so rather than report green.
bool validationLayerActive();

}  // namespace wallpaperx
