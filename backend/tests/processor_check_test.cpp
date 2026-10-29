// SPDX-License-Identifier: GPL-2.0-or-later
//
// The processor-decoding check below the bus: processorDecoding()'s states, ProcessorCheck's reuse of an answer for
// unchanged bytes and its forget(), and the scan record's round trip. No device, no bus. Every open() of a scratch
// copy is counted here, so "answered without reading the file" is observed rather than inferred from timing.

#include "daemon/processor_check.h"
#include "daemon/scan_record.h"
#include "tests/test_phase.h"
#include "tests/test_report.h"

#include "support/loaded_range.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

using wallpaperx::FileScan;
using wallpaperx::ProcessorCheck;
using wallpaperx::ProcessorDecoding;
using wallpaperx::ProcessorUse;
using wallpaperx::SoftwareStretches;
using wallpaperx::testing::check;
using wallpaperx::testing::harnessFailure;
namespace fs = std::filesystem;

// Opens of watched paths, and one path whose open is held until the test releases it.
std::mutex g_openMutex;
std::condition_variable g_openChanged;
std::map<std::string, int> g_opens;
std::string g_gatePath;
bool g_gateEntered = false;
bool g_gateReleased = false;

#if defined(__SANITIZE_THREAD__)
// ThreadSanitizer's symboliser opens files while it holds its report lock; the mutex and the map below would
// re-enter the sanitizer and deadlock it, so its own calls go straight through. It never opens a scratch copy.
const wallpaperx::testing::LoadedRange g_sanitizerRuntime{"libtsan.so"};
bool calledFromSanitizer(const void* returnAddress) { return g_sanitizerRuntime.contains(returnAddress); }
#else
constexpr bool calledFromSanitizer(const void*) { return false; }
#endif

}  // namespace

extern "C" int open(const char* path, int flags, ...) {
    using Fn = int (*)(const char*, int, mode_t);
    static const Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "open"));
    if (real == nullptr) return -1;
    mode_t mode = 0;
    if ((flags & (O_CREAT | O_TMPFILE)) != 0) {
        va_list args;
        va_start(args, flags);
        mode = static_cast<mode_t>(va_arg(args, unsigned int));
        va_end(args);
    }
    if (calledFromSanitizer(__builtin_return_address(0))) return real(path, flags, mode);
    if (path != nullptr) {
        std::unique_lock<std::mutex> lock(g_openMutex);
        if (const auto counted = g_opens.find(path); counted != g_opens.end()) ++counted->second;
        if (!g_gatePath.empty() && g_gatePath == path) {
            g_gateEntered = true;
            g_openChanged.notify_all();
            // Bounded: a test that never releases must not hang the scan thread past its own failure.
            g_openChanged.wait_for(lock, std::chrono::seconds(20), [] { return g_gateReleased; });
        }
    }
    return real(path, flags, mode);
}

namespace {

int opensOf(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_openMutex);
    return g_opens[path];
}

void watch(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_openMutex);
    g_opens[path] = 0;
}

std::optional<std::string> readWhole(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream text;
    text << in.rdbuf();
    if (in.bad()) return std::nullopt;
    return text.str();
}

// Every answer the check's thread reported, by monitor.
struct Answers {
    std::mutex mutex;
    std::condition_variable changed;
    std::map<int, std::vector<ProcessorCheck::Found>> byMonitor;

    ProcessorCheck::Done callback() {
        return [this](int monitorId, uint64_t, ProcessorCheck::Found found) {
            std::lock_guard<std::mutex> lock(mutex);
            byMonitor[monitorId].push_back(std::move(found));
            changed.notify_all();
        };
    }
    // The answer for `monitorId`, or nothing after ten seconds -- which the caller reports as a harness fault.
    std::optional<ProcessorCheck::Found> await(int monitorId) {
        std::unique_lock<std::mutex> lock(mutex);
        if (!changed.wait_for(lock, std::chrono::seconds(10), [&] { return !byMonitor[monitorId].empty(); })) {
            return std::nullopt;
        }
        return byMonitor[monitorId].front();
    }
    size_t count(int monitorId) {
        std::lock_guard<std::mutex> lock(mutex);
        return byMonitor[monitorId].size();
    }
};

std::string asset(const char* name) { return (fs::path(WALLPAPERX_RENDERER_VIDEO_ASSET_DIR) / name).string(); }

// A scratch copy, so a check can change its identity without touching the build tree's fixtures.
std::optional<std::string> copyOf(const fs::path& dir, const char* name) {
    std::error_code error;
    const fs::path to = dir / name;
    fs::copy_file(asset(name), to, fs::copy_options::overwrite_existing, error);
    if (error) {
        harnessFailure(std::string("could not copy the fixture ") + name + ": " + error.message());
        return std::nullopt;
    }
    return to.string();
}

std::string describe(const wallpaperx::ProcessorDecodingStatus& status) {
    std::string out = toString(status.state);
    for (const char* reason : status.reasons) out += std::string(" ") + reason;
    return out;
}

void runDecodingStateChecks() {
    using First = ProcessorUse::FirstPicture;
    const auto scanned = [](bool missing, bool change, bool throughout = false) {
        SoftwareStretches found;
        found.missingReference = missing;
        found.parameterSetsChangeOffReset = change;
        found.missingReferenceThroughout = throughout;
        return FileScan{found, std::nullopt};
    };
    struct Row {
        const char* name;
        ProcessorUse use;
        std::optional<bool> inlineParameters;
        const char* expected;
    };
    // GOAL: S15.952 -- GetStatus's state for each combination: "all" when the decoder chose software for the whole
    //       stream at setup, whatever the scan holds; "parts" with each reason that holds; "none" only once the scan,
    //       the first picture and (for a parameter-set change) the device have all answered; "checking" before that;
    //       "unknown" for a scan that failed with nothing else known.
    // WHY-THIS-SHAPE: the derivation is a pure function of the record, so it is graded on every branch here; the
    //       daemon over the bus only feeds it. A state per row named by the row, so a red line says which rule broke.
    // CONTROL: control_batch.sh 2026-10-24 (T18b), each alone, reverted, backend/daemon/processor_check.cpp
    //       processorDecoding(): the SoftwareThroughout branch made `if (false)` -> "FAIL: S15.952 refused at setup,
    //       gap in file: got 'parts missing_reference', want 'all'"; `parameterSets == false` made `!= true` ->
    //       "FAIL: S15.952 change, device not known: got 'none', want 'checking'".
    const std::vector<Row> rows = {
        {"refused at setup, gap in file", ProcessorUse{1, scanned(true, false), First::SoftwareThroughout}, true, "all"},
        {"refused at setup, still scanning", ProcessorUse{1, ProcessorUse::Scanning{}, First::SoftwareThroughout}, true,
         "all"},
        {"healthy, drawn on the device", ProcessorUse{1, scanned(false, false), First::Device}, true, "none"},
        {"healthy, no device answer", ProcessorUse{1, scanned(false, false), First::Device}, std::nullopt, "none"},
        {"missing reference", ProcessorUse{1, scanned(true, false), First::Device}, true, "parts missing_reference"},
        // S15.992: no reset point after a damaged start, so no picture reaches the device.
        // CONTROL: control_batch.sh 2026-10-28, reverted, backend/daemon/processor_check.cpp processorDecoding():
        //       `|| (found != nullptr && found->missingReferenceThroughout)` deleted -> "FAIL: S15.952 S15.992 missing
        //       reference throughout: got 'parts missing_reference', want 'all'".
        {"S15.992 missing reference throughout", ProcessorUse{1, scanned(true, false, true), First::Device}, true, "all"},
        {"S15.992 missing reference throughout, first picture in software",
         ProcessorUse{1, scanned(true, false, true), First::Software}, true, "all"},
        {"first picture in software", ProcessorUse{1, scanned(false, false), First::Software}, true,
         "parts first_picture"},
        {"change, no inline parameters", ProcessorUse{1, scanned(false, true), First::Device}, false,
         "parts parameter_set_change"},
        {"change, inline parameters", ProcessorUse{1, scanned(false, true), First::Device}, true, "none"},
        {"change, device not known", ProcessorUse{1, scanned(false, true), First::Device}, std::nullopt, "checking"},
        {"healthy, nothing drawn yet", ProcessorUse{1, scanned(false, false), std::nullopt}, true, "checking"},
        {"still scanning, drawn on the device", ProcessorUse{1, ProcessorUse::Scanning{}, First::Device}, true,
         "checking"},
        {"scan failed", ProcessorUse{1, ProcessorUse::ScanFailed{}, First::Device}, true, "unknown"},
        {"scan failed, first picture in software", ProcessorUse{1, ProcessorUse::ScanFailed{}, First::Software}, true,
         "parts first_picture"},
    };
    for (const Row& row : rows) {
        const std::string got = describe(wallpaperx::processorDecoding(row.use, row.inlineParameters));
        if (got != row.expected) {
            std::fprintf(stderr, "FAIL: S15.952 %s: got '%s', want '%s'\n", row.name, got.c_str(), row.expected);
            check(false, "processorDecoding() reports the state its record calls for");
        }
    }
}

const FileScan* scanOf(const std::optional<ProcessorCheck::Found>& found) {
    return found ? std::get_if<FileScan>(&*found) : nullptr;
}

void runReuseChecks(const fs::path& dir) {
    const std::optional<std::string> gap = copyOf(dir, "counter_gap.mp4");
    if (!gap) return;
    watch(*gap);
    // A checker per step: one checker keeps what it read while its queue is not empty, which would answer a step
    // queued before the thread came back for it from that cache instead of from the answer handed in.
    Answers answers;
    std::optional<ProcessorCheck> checker(std::in_place, answers.callback());

    checker->check(1, 1, *gap, "");
    const std::optional<ProcessorCheck::Found> first = answers.await(1);
    const FileScan* fresh = scanOf(first);
    if (fresh == nullptr || !fresh->file || !fresh->found.missingReference || opensOf(*gap) != 1) {
        std::fprintf(stderr, "FAIL: first scan: %s, %d open(s)\n",
                     !first ? "no answer in 10 s" : fresh == nullptr ? "failed" : !fresh->file ? "no identity" : "answered",
                     opensOf(*gap));
        harnessFailure("a fresh scan of counter_gap.mp4 did not read it once and find its missing reference, so reuse "
                       "cannot be graded");
        return;
    }

    // GOAL: S15.954 -- an answer handed in for bytes the path still names is used without opening the file.
    // WHY-THIS-SHAPE: the saved answer is deliberately the wrong one for this file (no missing reference), so the
    //       answer returned shows which source it came from, and the open count shows the file was not read; either
    //       alone would pass a check that read the file and happened to agree.
    // CONTROL: control_batch.sh 2026-10-24 (T18b), reverted, backend/daemon/processor_check.cpp run(): `job.saved
    //       && job.saved->file && before == job.saved->file` given `false &&` -> "FAIL: S15.954 a saved answer for unchanged bytes is used
    //       without reading the file".
    FileScan saved = *fresh;
    saved.found.missingReference = false;
    checker.emplace(answers.callback());
    checker->check(2, 2, *gap, "", saved);
    const FileScan* reused = scanOf(answers.await(2));
    if (reused == nullptr || reused->found.missingReference || opensOf(*gap) != 1) {
        std::fprintf(stderr, "FAIL: reuse: %s, %d open(s) in all\n",
                     reused == nullptr ? "no answer" : reused->found.missingReference ? "answer read again" : "saved answer",
                     opensOf(*gap));
        check(false, "S15.954 a saved answer for unchanged bytes is used without reading the file");
    }

    // GOAL: S15.954 -- once the path names other bytes (a new modification time), the saved answer is not trusted:
    //       the file is read again and its own answer reported.
    // WHY-THIS-SHAPE: only the modification time moves, the least the identity distinguishes; a size change would
    //       pass even with the time dropped from the comparison.
    // CONTROL: control_batch.sh 2026-10-24 (T18b), reverted, backend/daemon/processor_check.cpp run(): `before ==
    //       job.saved->file` made
    //       `before.has_value()` -> "FAIL: S15.954 a saved answer for changed bytes is not used: the file is read
    //       again".
    const timespec times[2] = {{0, UTIME_OMIT}, {1'000'000, 0}};
    if (::utimensat(AT_FDCWD, gap->c_str(), times, 0) != 0) {
        harnessFailure("could not change the copy's modification time");
        return;
    }
    checker.emplace(answers.callback());
    checker->check(3, 3, *gap, "", saved);
    const FileScan* reread = scanOf(answers.await(3));
    if (reread == nullptr || !reread->found.missingReference || opensOf(*gap) != 2) {
        std::fprintf(stderr, "FAIL: changed file: %s, %d open(s) in all\n",
                     reread == nullptr ? "no answer" : reread->found.missingReference ? "read again" : "saved answer used",
                     opensOf(*gap));
        check(false, "S15.954 a saved answer for changed bytes is not used: the file is read again");
    }
}

void runForgetChecks(const fs::path& dir) {
    const std::optional<std::string> held = copyOf(dir, "counter_opengop.ts");
    const std::optional<std::string> queued = copyOf(dir, "counter_gap.mp4");
    const std::optional<std::string> last = copyOf(dir, "counter_two_idr.mp4");
    if (!held || !queued || !last) return;
    watch(*held);
    watch(*queued);
    Answers answers;
    {
        std::lock_guard<std::mutex> lock(g_openMutex);
        g_gatePath = *held;
        g_gateEntered = g_gateReleased = false;
    }
    const auto release = [] {
        std::lock_guard<std::mutex> lock(g_openMutex);
        g_gateReleased = true;
        g_gatePath.clear();
        g_openChanged.notify_all();
    };
    ProcessorCheck checker(answers.callback());
    // GOAL: S15.954 -- forget() withdraws a monitor's scan, queued or already reading: no answer is reported for it,
    //       and a queued one never opens its file.
    // WHY-THIS-SHAPE: the scan thread is held inside its open of monitor 1's file, so monitor 2's job is provably
    //       queued behind it when forgotten, and monitor 1's scan is provably running when it is forgotten. Monitor
    //       3's answer, queued last, is the barrier: the thread serves in order, so once it answers nothing earlier
    //       can still arrive. Waiting a while for "no answer" instead would pass on a slow thread.
    // CONTROL: control_batch.sh 2026-10-24 (T18b), each alone, reverted, backend/daemon/processor_check.cpp
    //       forgetLocked(): the erase_if deleted -> "FAIL:
    //       S15.954 a queued scan forgotten ..."; the m_stopRunning store deleted -> "FAIL: S15.954 a running scan
    //       forgotten ...".
    checker.check(1, 1, *held, "");
    {
        std::unique_lock<std::mutex> lock(g_openMutex);
        if (!g_openChanged.wait_for(lock, std::chrono::seconds(10), [] { return g_gateEntered; })) {
            lock.unlock();
            release();
            harnessFailure("the scan thread never opened the first file, so forget() cannot be graded");
            return;
        }
    }
    checker.check(2, 2, *queued, "");
    checker.forget(2);
    checker.forget(1);
    release();
    checker.check(3, 3, *last, "");
    if (!answers.await(3)) {
        harnessFailure("the barrier scan never answered in 10 s, so forget() was not graded");
        return;
    }
    if (answers.count(2) != 0 || opensOf(*queued) != 0) {
        std::fprintf(stderr, "FAIL: monitor 2: %zu answer(s), %d open(s)\n", answers.count(2), opensOf(*queued));
        check(false, "S15.954 a queued scan forgotten is never run and never reported");
    }
    if (answers.count(1) != 0 || opensOf(*held) != 1) {
        std::fprintf(stderr, "FAIL: monitor 1: %zu answer(s), %d open(s)\n", answers.count(1), opensOf(*held));
        check(false, "S15.954 a running scan forgotten reports nothing");
    }
}

void runRecordChecks(const fs::path& dir) {
    const std::optional<std::string> file = copyOf(dir, "counter_gap.mp4");
    if (!file) return;
    const fs::path state = dir / "state";
    fs::create_directories(state);
    const std::optional<wallpaperx::FileIdentity> identity = wallpaperx::fileIdentityOf(*file);
    if (!identity) {
        harnessFailure("could not stat the copy");
        return;
    }
    SoftwareStretches found;
    found.missingReference = true;
    found.missingReferenceThroughout = true;
    // A second entry with every flag the other way round, so each flag is read back in both states.
    SoftwareStretches foundB;
    foundB.missingReference = true;
    foundB.missingReferenceThroughout = false;
    foundB.parameterSetsChangeOffReset = true;
    const std::string fileB = *file + ".b";
    const std::string written = wallpaperx::writeScanRecord(
        state, {wallpaperx::SavedScan{*file, *identity, found}, wallpaperx::SavedScan{fileB, *identity, foundB}});
    if (!written.empty()) {
        harnessFailure("the scan record could not be written: " + written);
        return;
    }
    // GOAL: S15.954 -- an answer saved by one run comes back in the next with the identity it was saved against, so
    //       ProcessorCheck can match it to unchanged bytes; a record written under another scan rule is not read.
    // WHY-THIS-SHAPE: the whole identity is compared, field by field through operator==, against a fresh stat();
    //       comparing the path alone would pass a record that lost the inode or the time.
    // CONTROL: control_batch.sh 2026-10-24 (T18b), each alone, reverted, backend/daemon/scan_record.cpp
    //       readScanRecord(): the inode read into
    //       scan.file.device -> "FAIL: S15.954 a saved answer reads back with the identity it was saved against";
    //       the rule comparison deleted -> "FAIL: S15.954 a record from another scan rule is not read".
    const std::vector<wallpaperx::SavedScan> back = wallpaperx::readScanRecord(state);
    const auto entry = [&back](const std::string& path) -> const wallpaperx::SavedScan* {
        for (const wallpaperx::SavedScan& scan : back)
            if (scan.path == path) return &scan;
        return nullptr;
    };
    const wallpaperx::SavedScan* const a = entry(*file);
    const wallpaperx::SavedScan* const b = entry(fileB);
    const bool same = back.size() == 2 && a != nullptr && b != nullptr && a->file == *identity &&
                      a->found.missingReference && b->found.missingReference;
    if (!same) std::fprintf(stderr, "FAIL: %zu entr(ies) read back\n", back.size());
    check(same, "S15.954 a saved answer reads back with the identity it was saved against");
    // GOAL: S15.992 -- the record round-trips missingReferenceThroughout and parameterSetsChangeOffReset, each
    //       as saved, true and false: a reader that fills a flag from another flag or from a constant is red.
    // WHY-THIS-SHAPE: two entries with opposite flags, looked up by path; one all-true entry cannot tell a
    //       restored flag from one copied off missingReference.
    // CONTROL: control_batch.sh 2026-10-29, each alone, reverted, backend/daemon/scan_record.cpp writeScanRecord():
    //       the kKeyThroughout entry written as `scan.found.missingReference` -> "...false entry reads back false";
    //       written as `false` -> "...true entry reads back true"; kKeyParameterSets written as `false` -> the
    //       parameter-sets line.
    check(a != nullptr && a->found.missingReferenceThroughout, "S15.992 the throughout flag of the true entry reads back true");
    check(b != nullptr && !b->found.missingReferenceThroughout, "S15.992 the throughout flag of the false entry reads back false");
    check(a != nullptr && b != nullptr && !a->found.parameterSetsChangeOffReset && b->found.parameterSetsChangeOffReset,
          "S15.992 the parameter-sets flag reads back as saved, false and true");

    const fs::path record = wallpaperx::scanRecordPath(state);
    std::string text = readWhole(record).value_or(std::string());
    const std::string key = "\"rule\":" + std::to_string(wallpaperx::kSoftwareScanRule);
    const size_t at = text.find(key);
    if (at == std::string::npos) {
        harnessFailure("the record does not hold '" + key + "' as written, so the rule check cannot be graded");
        return;
    }
    text.replace(at, key.size(), "\"rule\":" + std::to_string(wallpaperx::kSoftwareScanRule + 1));
    // WHY-THIS-SHAPE: the planted bytes are read back before the rule check: a failed rewrite would leave the
    //       current rule in place and grade the reader on a record it accepts.
    // CONTROL: test-side, by hand, 2026-10-24 (T19a), restored: `record` replaced by `state` (a directory) in the
    //       write -> "FAIL (harness): could not plant the other-rule record at ...", the rule check not reached.
    {
        std::ofstream out(record, std::ios::binary | std::ios::trunc);
        out << text;
        out.close();
        if (!out || readWhole(record) != text) {
            harnessFailure("could not plant the other-rule record at " + record.string() +
                           ", so the rule check cannot be graded");
            return;
        }
    }
    check(wallpaperx::readScanRecord(state).empty(), "S15.954 a record from another scan rule is not read");

    // GOAL: S15.992 -- the current scan rule is past rule 1, so a rule-1 answer is never taken as current.
    // WHY-THIS-SHAPE: a check of its own, not part of the planting guard below: a revert to 1 is a production
    //       regression and must fail as one, not as a harness fault.
    // CONTROL: via control_batch.sh 2026-10-28, reverted: kSoftwareScanRule (renderer/video/software_scan.h)
    //       made 1 -> this line red, alone.
    check(wallpaperx::kSoftwareScanRule != 1, "S15.992 the current scan rule is not rule 1, so rule-1 records are rescanned");
    {
        std::string rule1 = readWhole(record).value_or(std::string());
        const std::string otherRule = "\"rule\":" + std::to_string(wallpaperx::kSoftwareScanRule + 1);
        const size_t ruleAt = rule1.find(otherRule);
        const std::string throughoutTrue = "\"missing_reference_throughout\":true,";
        const std::string throughoutFalse = "\"missing_reference_throughout\":false,";
        if (ruleAt == std::string::npos || rule1.find(throughoutTrue) == std::string::npos ||
            rule1.find(throughoutFalse) == std::string::npos) {
            harnessFailure("the record does not hold the rule and both throughout keys as expected (" + rule1 +
                           "), so the rule-1 check cannot be graded");
            return;
        }
        rule1.erase(rule1.find(throughoutTrue), throughoutTrue.size());
        rule1.erase(rule1.find(throughoutFalse), throughoutFalse.size());
        rule1.replace(rule1.find(otherRule), otherRule.size(), "\"rule\":1");
        std::ofstream out(record, std::ios::binary | std::ios::trunc);
        out << rule1;
        out.close();
        if (!out || readWhole(record) != rule1) {
            harnessFailure("could not plant the rule-1 record, so the rule-1 check cannot be graded");
            return;
        }
    }
    // GOAL: S15.992 -- a record saved under rule 1, which had no throughout flag, is scanned again, not reused.
    // WHY-THIS-SHAPE: the rule-1 record is planted as rule 1 wrote it (no throughout key), so it fails both on the
    //       rule and on the missing key; the rule bump is graded alone by the check above with the key present.
    // CONTROL: control_batch.sh 2026-10-29, reverted, backend/daemon/scan_record.cpp readScanRecord(): the rule
    //       comparison made `rule == 0` and a missing throughout key accepted -> "FAIL: S15.992 a rule-1 record is not
    //       read" (with the S15.954 other-rule line).
    check(wallpaperx::readScanRecord(state).empty(), "S15.992 a rule-1 record is not read");
}

}  // namespace

int main() {
    wallpaperx::testing::g_testName = "processor_check_test";
    using wallpaperx::testing::phaseSelected;
    const char* tmp = std::getenv("TMPDIR");
    const fs::path dir = fs::path(tmp != nullptr && *tmp != '\0' ? tmp : "/tmp") /
                         ("wpx-processor-check-" + std::to_string(::getpid()));
    try {
        fs::create_directories(dir);
        if (!fs::exists(asset("counter_gap.mp4"))) {
            wallpaperx::testing::notVerified("S15.952-S15.954's reuse, forget and record checks",
                                             "the renderer's video fixtures were not generated (no ffmpeg)");
        }
        if (phaseSelected("decodingState")) runDecodingStateChecks();
        if (fs::exists(asset("counter_gap.mp4"))) {
            if (phaseSelected("reuse")) runReuseChecks(dir);
            if (phaseSelected("forget")) runForgetChecks(dir);
            if (phaseSelected("record")) runRecordChecks(dir);
        }
    } catch (const std::exception& e) {
        harnessFailure(std::string("the test could not run: ") + e.what());
    }
    std::error_code ignored;
    fs::remove_all(dir, ignored);
    const bool red = wallpaperx::testing::reportCheckFailures();
    const bool harnessRed = wallpaperx::testing::reportHarnessFaults("in this test file, not in the daemon");
    if (red || harnessRed) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
