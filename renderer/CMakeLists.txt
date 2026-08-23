cmake_minimum_required(VERSION 3.25)
project(wallpaperx-renderer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --------------------------------------------------------------------------- #
# Dependencies
# --------------------------------------------------------------------------- #

find_package(Vulkan REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(nlohmann_json REQUIRED)

pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
pkg_check_modules(FFTW3    REQUIRED fftw3)
pkg_check_modules(SHADERC  REQUIRED shaderc)

# libmpv — no pkg-config on some distros, find manually
find_library(MPV_LIBRARY NAMES mpv REQUIRED
    DOC "libmpv shared library")
find_path(MPV_INCLUDE_DIR NAMES mpv/client.h REQUIRED
    DOC "libmpv include directory")

if(NOT MPV_LIBRARY OR NOT MPV_INCLUDE_DIR)
    message(FATAL_ERROR
        "libmpv not found.\n"
        "Install mpv development package for your distro.\n"
        "See renderer/DEPENDENCIES.md for package names."
    )
endif()

# VulkanMemoryAllocator — vendored single header
set(VMA_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
if(NOT EXISTS "${VMA_INCLUDE_DIR}/vk_mem_alloc.h")
    message(FATAL_ERROR
        "vk_mem_alloc.h not found at renderer/third_party/vk_mem_alloc.h\n"
        "Run: curl -fsSL https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/master/include/vk_mem_alloc.h -o renderer/third_party/vk_mem_alloc.h"
    )
endif()

# stb_image — find system or vendored
find_path(STB_INCLUDE_DIR NAMES stb_image.h
    PATHS /usr/include /usr/local/include
    PATH_SUFFIXES stb
)
if(NOT STB_INCLUDE_DIR)
    message(FATAL_ERROR
        "stb_image.h not found.\n"
        "Arch: sudo pacman -S stb\n"
        "See renderer/DEPENDENCIES.md for other distros."
    )
endif()

find_program(GLSLC_EXECUTABLE glslc REQUIRED)

# Lavapipe (Mesa's software Vulkan implementation) — optional. When present,
# GPU-backed tests also get registered against it (separately from the real
# GPU), giving hardware-independent coverage and forcing the Classic/Buffer
# descriptor tiers to run every time instead of only when manually forced.
find_file(WALLPAPERX_LAVAPIPE_ICD
    NAMES lvp_icd.json lvp_icd.x86_64.json
    PATHS /usr/share/vulkan/icd.d
)

# --------------------------------------------------------------------------- #
# Static library target
# --------------------------------------------------------------------------- #

add_library(wallpaperx-renderer STATIC
    renderer.cpp
    vulkan/device.cpp
    vulkan/memory.cpp
    vulkan/memory_export.cpp
    vulkan/tier_selection.cpp
    vulkan/descriptor_classic.cpp
    vulkan/descriptor_buffer.cpp
    vulkan/descriptor_heap.cpp
    vulkan/offscreen.cpp
    vulkan/frame_export.cpp
    renderers/static_renderer.cpp
)

target_include_directories(wallpaperx-renderer
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${Vulkan_INCLUDE_DIRS}
        ${MPV_INCLUDE_DIR}
        ${STB_INCLUDE_DIR}
        ${VMA_INCLUDE_DIR}
)

target_link_libraries(wallpaperx-renderer
    PUBLIC
        Vulkan::Vulkan
        ${CMAKE_DL_LIBS}
        ${MPV_LIBRARY}
        ${PIPEWIRE_LIBRARIES}
        ${FFTW3_LIBRARIES}
        ${SHADERC_LIBRARIES}
        nlohmann_json::nlohmann_json
)

target_compile_options(wallpaperx-renderer PRIVATE
    ${PIPEWIRE_CFLAGS_OTHER}
    ${FFTW3_CFLAGS_OTHER}
    ${SHADERC_CFLAGS_OTHER}
)

# --------------------------------------------------------------------------- #
# Status
# --------------------------------------------------------------------------- #

message(STATUS "wallpaperx-renderer configured")
message(STATUS "  Vulkan:   ${Vulkan_VERSION}")
message(STATUS "  MPV lib:  ${MPV_LIBRARY}")
message(STATUS "  PipeWire: ${PIPEWIRE_VERSION}")
message(STATUS "  FFTW3:    ${FFTW3_VERSION}")
message(STATUS "  shaderc:  ${SHADERC_VERSION}")
message(STATUS "  VMA:      vendored (third_party/vk_mem_alloc.h)")
if(WALLPAPERX_LAVAPIPE_ICD)
    message(STATUS "  Lavapipe: found (${WALLPAPERX_LAVAPIPE_ICD}) -- lavapipe ctest pass enabled")
else()
    message(STATUS "  Lavapipe: not found -- lavapipe ctest pass skipped (optional; pacman -S vulkan-swrast to enable)")
endif()

set(SHADER_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUTPUT_DIR})

# Consumers built via add_subdirectory (the backend daemon) need this path at runtime
# to hand StaticRenderer a shader directory. A plain set() would not escape this
# scope, and hardcoding "<backend build>/renderer/shaders" on the other side would
# duplicate a layout only this file decides. INTERNAL rather than PARENT_SCOPE so it
# also works when the renderer is configured standalone (no parent to set).
set(WALLPAPERX_SHADER_OUTPUT_DIR ${SHADER_OUTPUT_DIR} CACHE INTERNAL
    "Directory holding the renderer's compiled SPIR-V")

set(SHADER_SPV_FILES "")
foreach(SHADER_SRC fullscreen.vert fullscreen_set.frag fullscreen_heap.frag)
    set(SHADER_SPV ${SHADER_OUTPUT_DIR}/${SHADER_SRC}.spv)
    add_custom_command(
        OUTPUT ${SHADER_SPV}
        COMMAND ${GLSLC_EXECUTABLE} --target-env=vulkan1.3 -o ${SHADER_SPV} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${SHADER_SRC}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${SHADER_SRC}
        COMMENT "Compiling shader ${SHADER_SRC}"
    )
    list(APPEND SHADER_SPV_FILES ${SHADER_SPV})
endforeach()

add_custom_target(wallpaperx-shaders DEPENDS ${SHADER_SPV_FILES})

# Where every probe that writes a PNG fixture puts it. Captured once, and used inside
# wallpaperx_add_shader_probe() below instead of ${CMAKE_CURRENT_BINARY_DIR}: the function
# expands its body at call time, so a future call from another add_subdirectory scope
# would otherwise silently point that probe's assets at a different directory.
set(WALLPAPERX_RENDERER_TEST_ASSET_DIR ${CMAKE_CURRENT_BINARY_DIR}/test_assets)

# The recipe every shader+asset probe needs, in one place. Four blocks used to repeat it
# character for character, so a change to the asset layout or the shader output path had
# to be made in lockstep in four places and a missed one produces a probe that silently
# reads a stale shader or writes into the previous layout.
#
# No option for the tests/ include path deliberately: quoted includes resolve relative to
# the including file, and every probe source lives in tests/, so none of them needs one.
function(wallpaperx_add_shader_probe target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE wallpaperx-renderer)
    add_dependencies(${target} wallpaperx-shaders)
    target_compile_definitions(${target} PRIVATE
        WALLPAPERX_SHADER_DIR="${SHADER_OUTPUT_DIR}"
        WALLPAPERX_TEST_IMAGE_DIR="${WALLPAPERX_RENDERER_TEST_ASSET_DIR}"
    )
endfunction()

add_executable(wallpaperx-device-probe tests/device_probe.cpp)
target_link_libraries(wallpaperx-device-probe PRIVATE wallpaperx-renderer)

add_executable(wallpaperx-tier-selection-test tests/tier_selection_test.cpp)
target_link_libraries(wallpaperx-tier-selection-test PRIVATE wallpaperx-renderer)

add_executable(wallpaperx-memory-export-test tests/memory_export_test.cpp)
target_link_libraries(wallpaperx-memory-export-test PRIVATE wallpaperx-renderer)

add_executable(wallpaperx-offscreen-probe tests/offscreen_probe.cpp)
target_link_libraries(wallpaperx-offscreen-probe PRIVATE wallpaperx-renderer)

add_executable(wallpaperx-device-affinity-probe tests/device_affinity_probe.cpp)
target_link_libraries(wallpaperx-device-affinity-probe PRIVATE wallpaperx-renderer)

wallpaperx_add_shader_probe(wallpaperx-static-renderer-probe tests/static_renderer_probe.cpp)

# S4.9's probe interposes malloc/calloc/realloc to see stb_image's pixel allocation, and
# fopen to make the file change between stbi_info() and stbi_load() for the post-load
# re-check. Its assets -- mostly header-only stubs, one per budget edge, plus a
# deliberately mode-000 one -- share WALLPAPERX_TEST_IMAGE_DIR with static_renderer_probe,
# frame_export_probe
# and modifier_roundtrip_probe -- the bounds_ filename prefix is the ONLY thing keeping
# them apart, so keep that prefix on anything added here and do not add directory-scoped
# cleanup (a remove_all or a glob) to any of these probes: they hold different
# RESOURCE_LOCKs and can run concurrently under `ctest -jN`.
wallpaperx_add_shader_probe(wallpaperx-image-bounds-probe tests/image_bounds_probe.cpp)

# The probe's malloc interposition cannot coexist with a sanitizer runtime that replaces
# the allocator: the executable's strong malloc wins the link and hands out glibc pointers
# that the sanitizer's free() then operates on. The process dies before main -- a SEGV
# under TSan, "LeakSanitizer: CHECK failed: lsan_interceptors.cpp:83" and exit 23 under
# LSan. The probe compiles its instrument out and exits 77 (Skipped) when this is 0.
#
# Decided here rather than from predefined macros in the source, because that enumeration
# cannot be completed: GCC defines no macro at all for -fsanitize=leak (checked with -dM
# -E on gcc 16), so `-DWALLPAPERX_SANITIZE=leak` crashed instead of skipping -- observed,
# then fixed by this line. The definition lands on the
# target, so both the image_bounds_probe and image_bounds_probe_lavapipe entries are
# covered by this one line. Any non-empty value counts -- UBSan does not replace the
# allocator and could in principle still run the probe, but an allowlist of sanitizer
# names is the same maintenance burden that produced the bug, and over-skipping announces
# itself as a ctest Skip with the probe's printed reason.
if(WALLPAPERX_SANITIZE)
    target_compile_definitions(wallpaperx-image-bounds-probe PRIVATE WALLPAPERX_ALLOCATION_INSTRUMENT=0)
endif()

add_executable(wallpaperx-descriptor-allocator-probe tests/descriptor_allocator_probe.cpp)
target_link_libraries(wallpaperx-descriptor-allocator-probe PRIVATE wallpaperx-renderer)

# tests/support/ holds header-only test scaffolding shared with backend/tests -- kept
# out of the renderer library because nothing WallpaperX ships CPU-maps an exported
# frame; only a test needs to. It needs no include path: the probes reach it by quoted
# include, which resolves relative to the including file.
wallpaperx_add_shader_probe(wallpaperx-frame-export-probe tests/frame_export_probe.cpp)

wallpaperx_add_shader_probe(wallpaperx-modifier-roundtrip-probe tests/modifier_roundtrip_probe.cpp)

add_executable(wallpaperx-handle-type-env-test tests/handle_type_env_test.cpp)
target_link_libraries(wallpaperx-handle-type-env-test PRIVATE wallpaperx-renderer)

add_executable(wallpaperx-validation-grade-probe tests/validation_grade_probe.cpp)
target_link_libraries(wallpaperx-validation-grade-probe PRIVATE wallpaperx-renderer)

enable_testing()
add_test(NAME tier_selection COMMAND wallpaperx-tier-selection-test)
add_test(NAME memory_export COMMAND wallpaperx-memory-export-test)

# The two-argument externalMemoryHandleType() caches WALLPAPERX_FORCE_HANDLE_TYPE in a
# function-local static, so one process can only ever observe one value of it. Three
# processes, three environments -- a single binary looping over setenv would test the
# first value three times and report the other two as passing.
add_test(NAME handle_type_env_default COMMAND wallpaperx-handle-type-env-test "")
add_test(NAME handle_type_env_opaque COMMAND wallpaperx-handle-type-env-test opaque)
add_test(NAME handle_type_env_dmabuf COMMAND wallpaperx-handle-type-env-test dmabuf)
set_tests_properties(handle_type_env_opaque PROPERTIES ENVIRONMENT "WALLPAPERX_FORCE_HANDLE_TYPE=opaque")
set_tests_properties(handle_type_env_dmabuf PROPERTIES ENVIRONMENT "WALLPAPERX_FORCE_HANDLE_TYPE=dmabuf")
# Explicitly unset for the default case: ctest inherits the caller's environment, so a
# developer who exported the variable would otherwise silently turn this into a fourth
# copy of whichever case they happened to have set.
set_tests_properties(handle_type_env_default PROPERTIES ENVIRONMENT "WALLPAPERX_FORCE_HANDLE_TYPE=")

# Labelled so the TSan tier can tell "never loads an ICD" apart from "not a lavapipe
# entry". None of these five constructs a Device or calls a vk* entry point, so the
# proprietary-driver SEGV that forces that tier to lavapipe-only cannot reach them.
# backend/CMakeLists.txt previously keyed its disable on the *absence* of a lavapipe
# label, which swept all five out and left tier selection and the handle-type cache
# unexercised under the sanitizer -- while the run still reported green.
set_tests_properties(tier_selection memory_export
                     handle_type_env_default handle_type_env_opaque handle_type_env_dmabuf
                     PROPERTIES LABELS "novulkan")

add_test(NAME device_probe COMMAND wallpaperx-device-probe)
add_test(NAME offscreen_probe COMMAND wallpaperx-offscreen-probe)
add_test(NAME device_affinity_probe COMMAND wallpaperx-device-affinity-probe)
add_test(NAME static_renderer_probe COMMAND wallpaperx-static-renderer-probe)
add_test(NAME image_bounds_probe COMMAND wallpaperx-image-bounds-probe)
add_test(NAME descriptor_allocator_probe COMMAND wallpaperx-descriptor-allocator-probe)
add_test(NAME frame_export_probe COMMAND wallpaperx-frame-export-probe)
# Two entries, one binary. The probe makes two claims that are verifiable on different
# hardware: the round trip needs one usable DRM modifier, while modifier *sensitivity*
# needs two, so a wrong one can be substituted. Lavapipe offers exactly one (LINEAR), so
# it verifies the first and cannot verify the second. Reporting both as a single verdict
# means either claiming the control ran when it did not, or marking the whole probe
# unverified when the round trip demonstrably passed.
add_test(NAME modifier_roundtrip_probe COMMAND wallpaperx-modifier-roundtrip-probe roundtrip)
add_test(NAME modifier_sensitivity_probe COMMAND wallpaperx-modifier-roundtrip-probe sensitivity)

# Two entries, one binary, and they cannot share a process. The probe asks the loader
# whether the validation layer exists at all, that answer is cached for the life of the
# process, and the two directions it grades need opposite answers. The binary sets the
# loader's layer filter itself (see its main()) rather than taking it from an ENVIRONMENT
# property here, so it means the same thing run by hand and an edit to this file cannot
# turn the "absent" entry into a second copy of "installed".
add_test(NAME validation_grade_installed COMMAND wallpaperx-validation-grade-probe installed)
add_test(NAME validation_grade_absent COMMAND wallpaperx-validation-grade-probe absent)

# 77 means the host could not be put in the state the entry grades: no validation-layer
# package for the installed direction, or a loader too old for the layer filter for the
# absent one. Without SKIP_RETURN_CODE such a run exits 0, and "the split was verified"
# becomes indistinguishable from "the question could not be asked" -- which is the exact
# confusion this probe exists to remove.
set_tests_properties(validation_grade_installed validation_grade_absent PROPERTIES SKIP_RETURN_CODE 77)

# static_renderer_probe and its _lavapipe twin run the same binary and both
# read/write the same WALLPAPERX_TEST_IMAGE_DIR path -- lock them to the same
# resource so ctest never runs them concurrently (e.g. under `ctest -jN`),
# which would otherwise risk one process reading a half-written PNG from
# the other.
set_tests_properties(static_renderer_probe PROPERTIES RESOURCE_LOCK static_renderer_probe_test_image)

# image_bounds_probe writes bounds_*.png under the same WALLPAPERX_TEST_IMAGE_DIR and,
# like the others, is one binary registered twice -- so its two entries share a lock of
# their own. Its measurement is an allocation *count*, not a timing, so it does not need
# to be serialised against the rest of the suite.
#
# SKIP_RETURN_CODE 77 because a build with a sanitizer runtime cannot host the probe's
# malloc interposition -- any allocator-replacing sanitizer, not only TSan/ASan -- and the
# probe says so and exits 77 there (see the WALLPAPERX_SANITIZE block above the target).
# Without this, that run would exit 0 and a tier that verified nothing about the
# decoded-size bound would be indistinguishable from one that verified it.
set_tests_properties(image_bounds_probe PROPERTIES
                     RESOURCE_LOCK image_bounds_probe_test_image SKIP_RETURN_CODE 77)

# Same hazard for frame_export_probe, which writes its own gradient PNG: the probe and
# its _lavapipe twin are one binary run twice, so they share the file regardless of it
# being named differently from static_renderer_probe's.
set_tests_properties(frame_export_probe PROPERTIES RESOURCE_LOCK frame_export_probe_test_image)

# 77 means "this driver could not answer the question": no dma_buf/modifier support at
# all for either entry, or -- for the sensitivity entry only -- no second modifier to
# substitute. Without SKIP_RETURN_CODE such a run exits 0 and is indistinguishable from
# one that actually verified something, so a regression making the importer
# modifier-blind would ship under a green tick.
#
# All four entries share the lock: they are one binary writing one PNG path, so `ctest
# -jN` would otherwise have one process reading a file another is still emitting.
set_tests_properties(modifier_roundtrip_probe modifier_sensitivity_probe
                     PROPERTIES RESOURCE_LOCK modifier_roundtrip_probe_test_image SKIP_RETURN_CODE 77)

if(WALLPAPERX_LAVAPIPE_ICD)
    add_test(NAME device_probe_lavapipe COMMAND wallpaperx-device-probe)
    add_test(NAME offscreen_probe_lavapipe COMMAND wallpaperx-offscreen-probe)
    add_test(NAME device_affinity_probe_lavapipe COMMAND wallpaperx-device-affinity-probe)
    add_test(NAME static_renderer_probe_lavapipe COMMAND wallpaperx-static-renderer-probe)
    add_test(NAME image_bounds_probe_lavapipe COMMAND wallpaperx-image-bounds-probe)
    add_test(NAME descriptor_allocator_probe_lavapipe COMMAND wallpaperx-descriptor-allocator-probe)
    # Unlike the Heap tier, frame export needs no capability-skip under Lavapipe:
    # Lavapipe advertises both VK_KHR_external_memory_fd and
    # VK_EXT_external_memory_dma_buf, so exportFrame() takes the DMA_BUF path and
    # succeeds here (verified: distinct fds, correct size, zero validation
    # warnings). This is the only place the DMA_BUF branch runs in CI, since the
    # owner's real GPU is NVIDIA (OPAQUE_FD).
    add_test(NAME frame_export_probe_lavapipe COMMAND wallpaperx-frame-export-probe)
    add_test(NAME modifier_roundtrip_probe_lavapipe COMMAND wallpaperx-modifier-roundtrip-probe roundtrip)
    add_test(NAME modifier_sensitivity_probe_lavapipe COMMAND wallpaperx-modifier-roundtrip-probe sensitivity)
    # Registered here for the label, not for the ICD: what this probe grades is loader- and
    # driver-independent. CI selects by the lavapipe label and has no GPU, so without these two
    # the only check of how a missing validator is reported never runs there.
    add_test(NAME validation_grade_installed_lavapipe COMMAND wallpaperx-validation-grade-probe installed)
    add_test(NAME validation_grade_absent_lavapipe COMMAND wallpaperx-validation-grade-probe absent)
    set_tests_properties(
        device_probe_lavapipe
        offscreen_probe_lavapipe
        device_affinity_probe_lavapipe
        static_renderer_probe_lavapipe
        image_bounds_probe_lavapipe
        descriptor_allocator_probe_lavapipe
        frame_export_probe_lavapipe
        modifier_roundtrip_probe_lavapipe
        modifier_sensitivity_probe_lavapipe
        validation_grade_installed_lavapipe
        validation_grade_absent_lavapipe
        PROPERTIES
            # WALLPAPERX_ALLOW_NO_HEAP_TIER tells descriptor_allocator_probe's
            # Heap-tier tests that resolving to a non-Heap tier here is expected
            # (Lavapipe doesn't implement VK_EXT_descriptor_heap) rather than a
            # regression -- see heapTierGuard() in descriptor_allocator_probe.cpp.
            ENVIRONMENT "VK_ICD_FILENAMES=${WALLPAPERX_LAVAPIPE_ICD};WALLPAPERX_ALLOW_NO_HEAP_TIER=1"
            LABELS "lavapipe"
    )
    set_tests_properties(static_renderer_probe_lavapipe PROPERTIES RESOURCE_LOCK static_renderer_probe_test_image)
    set_tests_properties(image_bounds_probe_lavapipe PROPERTIES
                         RESOURCE_LOCK image_bounds_probe_test_image SKIP_RETURN_CODE 77)
    set_tests_properties(frame_export_probe_lavapipe PROPERTIES RESOURCE_LOCK frame_export_probe_test_image)
    set_tests_properties(modifier_roundtrip_probe_lavapipe modifier_sensitivity_probe_lavapipe
                         PROPERTIES RESOURCE_LOCK modifier_roundtrip_probe_test_image SKIP_RETURN_CODE 77)
    set_tests_properties(validation_grade_installed_lavapipe validation_grade_absent_lavapipe
                         PROPERTIES SKIP_RETURN_CODE 77)
endif()
