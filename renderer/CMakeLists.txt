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

# ffmpeg — decode for the video renderer. libavcodec decodes into the Vulkan device
# this renderer already owns (AVVulkanDeviceContext takes an existing VkDevice and
# returns VkImages), which is why no second GPU context is needed for video.
pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET
    libavcodec>=61
    libavformat>=61
    libavutil>=59
)

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
    vulkan/gpu_fault.cpp
    vulkan/device.cpp
    vulkan/memory.cpp
    vulkan/memory_export.cpp
    vulkan/tier_selection.cpp
    vulkan/descriptor_slots.cpp
    vulkan/descriptor_classic.cpp
    vulkan/descriptor_buffer.cpp
    vulkan/descriptor_heap.cpp
    vulkan/offscreen.cpp
    vulkan/frame_export.cpp
    vulkan/shader_module.cpp
    vulkan/format_features.cpp
    renderers/static_renderer.cpp
    renderers/video_renderer.cpp
    video/video_decoder.cpp
    video/frame_converter.cpp
)

target_include_directories(wallpaperx-renderer
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${Vulkan_INCLUDE_DIRS}
        ${STB_INCLUDE_DIR}
        ${VMA_INCLUDE_DIR}
)

target_link_libraries(wallpaperx-renderer
    PUBLIC
        Vulkan::Vulkan
        ${CMAKE_DL_LIBS}
        PkgConfig::LIBAV
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
message(STATUS "  ffmpeg:   libavcodec ${LIBAV_libavcodec_VERSION}")
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
foreach(SHADER_SRC fullscreen.vert fullscreen_set.frag fullscreen_heap.frag ycbcr_convert.vert ycbcr_convert.frag)
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

add_executable(wallpaperx-gpu-fault-test tests/gpu_fault_test.cpp)
target_link_libraries(wallpaperx-gpu-fault-test PRIVATE wallpaperx-renderer)

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

# Interposes vkCmdPipelineBarrier to read back the ownership-transfer barriers the export
# and the next render actually submitted. Unlike image_bounds_probe's malloc instrument
# this coexists with every sanitizer -- it replaces a Vulkan entry point, not the
# allocator -- so it needs no WALLPAPERX_SANITIZE opt-out.
wallpaperx_add_shader_probe(wallpaperx-ownership-transfer-probe tests/ownership_transfer_probe.cpp)

# The exporter's half of the dma_buf modifier negotiation the daemon's SetImportModifiers
# drives. Forces the dma_buf handle type itself (see its main()), so it runs on this GPU as
# well as on Lavapipe.
wallpaperx_add_shader_probe(wallpaperx-consumer-modifier-probe tests/consumer_modifier_probe.cpp)

# The acquire descriptor the exporter hands a consumer: that it is already signalled when it is
# handed over, and that each hand-over gets a payload of its own. It imports the descriptor back
# onto the same device and waits on it with a bounded fence timeout, so a defect reports as a
# failing check rather than as a ctest timeout.
wallpaperx_add_shader_probe(wallpaperx-frame-acquire-probe tests/frame_acquire_probe.cpp)

# Which image of a monitor's ring the next render is aimed at, and that holding one really
# does protect its pixels. Reads the held image back through Vulkan rather than through a
# CPU mapping of its exported fd, so it answers on the opaque-fd GPU as well as on Lavapipe.
# Writes ring_*.png into the shared asset directory -- keep that prefix.
wallpaperx_add_shader_probe(wallpaperx-target-ring-probe tests/target_ring_probe.cpp)

# Whether a descriptor the GPU may still be reading can be rewritten. Interposes
# vkWaitForFences, vkQueueSubmit and vkResetFences so "did the allocator have to wait?" is
# answered by the driver rather than by Device::hasCompleted() -- the function a mutation
# aimed at this fix would poison, taking any oracle routed through it along with it. Like
# ownership_transfer_probe it replaces Vulkan entry points and not the allocator, so it
# needs no WALLPAPERX_SANITIZE opt-out. No shaders and no PNG fixture, hence no lock.
add_executable(wallpaperx-descriptor-inflight-probe tests/descriptor_inflight_probe.cpp)
target_link_libraries(wallpaperx-descriptor-inflight-probe PRIVATE wallpaperx-renderer)

# The video renderer's only caller anywhere: nothing decodes yet, so without this probe its
# 343 lines are never executed. Interposes vkCmdPipelineBarrier to read the layout transitions
# it submitted -- like ownership_transfer_probe that replaces a Vulkan entry point and not the
# allocator, so it needs no WALLPAPERX_SANITIZE opt-out. No shaders and no PNG fixture (it
# builds its source frame in memory), hence no RESOURCE_LOCK.
add_executable(wallpaperx-video-renderer-probe tests/video_renderer_probe.cpp)
target_link_libraries(wallpaperx-video-renderer-probe PRIVATE wallpaperx-renderer)

# --------------------------------------------------------------------------- #
# Video fixtures for the decoder probe
# --------------------------------------------------------------------------- #
#
# Built with the ffmpeg command line rather than checked in: the files are derived, and each one
# exists to make a specific decision come out a specific way, which a filename cannot record but a
# generating command can. They are also tiny (one second at 320x240) and regenerating them is
# cheaper than reviewing a binary in a diff.
#
# The command line is optional. Where it is missing nothing is generated and the probe reports a
# named skip; the alternative -- refusing to configure -- would make a full ffmpeg install a build
# requirement for a project that only links the libraries.
set(WALLPAPERX_VIDEO_ASSET_DIR ${CMAKE_CURRENT_BINARY_DIR}/video_assets)
find_program(WALLPAPERX_FFMPEG_EXECUTABLE ffmpeg)
if(WALLPAPERX_FFMPEG_EXECUTABLE)
    file(MAKE_DIRECTORY ${WALLPAPERX_VIDEO_ASSET_DIR})
    set(WALLPAPERX_VIDEO_ASSETS "")
    # name;extra encoder arguments. The height of 238 in the first is deliberate: H.264 codes in
    # 16-pixel macroblocks, so the coded picture is 320x240 and two rows are cropped -- which is
    # what makes "the visible rectangle is read, not assumed" a question with a wrong answer.
    # 10-bit H.264 is what this decoder refuses outright, so it is the cheapest way to make the
    # hardware say no; mpeg4 is a codec no Vulkan decode queue advertises.
    set(WALLPAPERX_VIDEO_RECIPES
        "h264_crop.mp4|-f lavfi -i testsrc2=size=320x238:rate=10:duration=1|-c:v libx264 -pix_fmt yuv420p -profile:v baseline -g 5 -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv"
        "h264_10bit.mp4|-f lavfi -i testsrc2=size=320x240:rate=10:duration=1|-c:v libx264 -pix_fmt yuv420p10le -profile:v high10 -g 5"
        "mpeg4.avi|-f lavfi -i testsrc2=size=320x240:rate=10:duration=1|-c:v mpeg4 -g 5"
        "h264_raw.h264|-f lavfi -i testsrc2=size=320x240:rate=10:duration=2|-c:v libx264 -pix_fmt yuv420p -profile:v baseline -g 5 -bsf:v h264_mp4toannexb -f h264"
        "audio_only.m4a|-f lavfi -i sine=frequency=440:duration=1|-c:a aac"
    )
    foreach(RECIPE ${WALLPAPERX_VIDEO_RECIPES})
        string(REPLACE "|" ";" RECIPE_PARTS "${RECIPE}")
        list(GET RECIPE_PARTS 0 ASSET_NAME)
        list(GET RECIPE_PARTS 1 ASSET_INPUT)
        list(GET RECIPE_PARTS 2 ASSET_ENCODE)
        separate_arguments(ASSET_INPUT UNIX_COMMAND "${ASSET_INPUT}")
        separate_arguments(ASSET_ENCODE UNIX_COMMAND "${ASSET_ENCODE}")
        add_custom_command(
            OUTPUT ${WALLPAPERX_VIDEO_ASSET_DIR}/${ASSET_NAME}
            COMMAND ${WALLPAPERX_FFMPEG_EXECUTABLE} -y -v error ${ASSET_INPUT} ${ASSET_ENCODE}
                    ${WALLPAPERX_VIDEO_ASSET_DIR}/${ASSET_NAME}
            COMMENT "Generating video fixture ${ASSET_NAME}"
            VERBATIM
        )
        list(APPEND WALLPAPERX_VIDEO_ASSETS ${WALLPAPERX_VIDEO_ASSET_DIR}/${ASSET_NAME})
    endforeach()

    # The colour conversion probe's fixtures. Tags are set with the setparams filter, never with
    # -color_primaries / -color_trc: libx264 drops those two silently, which produced streams that
    # looked tagged and decoded untagged. SMPTE HD bars give large flat saturated regions, which is
    # where colour is compared. The edge fixtures pad the picture in red and crop the red away, so
    # reading past the visible rectangle is visible; the 4:2:0 _hw pair crops whole macroblocks at a
    # low quantiser with deblocking off, so the hardware decoder's lossy output stays flat at the edge,
    # and the 4:4:4 _hw pair is lossless H.265, which a hardware decoder takes at any crop offset.
    set(BT709 "colorspace=bt709:color_primaries=bt709:color_trc=bt709:range=tv")
    set(BARS "-f lavfi -i smptehdbars=size=320x240:rate=10:duration=1")
    set(WALLPAPERX_CONVERTER_RECIPES
        "conv_709_tv.mp4|${BARS}|-vf format=yuv420p,setparams=${BT709} -c:v libx264 -threads 1 -g 5"
        "conv_709_as_601.mp4|${BARS}|-vf format=yuv420p,setparams=colorspace=smpte170m:color_primaries=bt709:color_trc=bt709:range=tv -c:v libx264 -threads 1 -g 5"
        "conv_601_pc.mp4|${BARS}|-vf format=yuv420p,setparams=colorspace=smpte170m:color_primaries=smpte170m:color_trc=smpte170m:range=pc -c:v libx264 -g 5"
        "conv_709_tv_10bit.mp4|${BARS}|-vf format=yuv420p10le,setparams=${BT709} -c:v libx264 -profile:v high10 -g 5"
        "conv_709_tv_422.mp4|${BARS}|-vf format=yuv422p,setparams=${BT709} -c:v libx264 -g 5"
        "conv_709_tv_444.mp4|${BARS}|-vf format=yuv444p,setparams=${BT709} -c:v libx264 -g 5"
        "conv_hevc10.mp4|${BARS}|-vf format=yuv420p10le,setparams=${BT709} -c:v libx265 -x265-params log-level=error -g 5"
        "conv_hevc444.mp4|${BARS}|-vf format=yuv444p,setparams=${BT709} -c:v libx265 -x265-params log-level=error -g 5"
        "conv_hevc444_10.mp4|${BARS}|-vf format=yuv444p10le,setparams=${BT709} -c:v libx265 -x265-params log-level=error -g 5"
        "conv_hevc444_12.mp4|${BARS}|-vf format=yuv444p12le,setparams=${BT709} -c:v libx265 -x265-params log-level=error -g 5"
        "edge_offset_420.mp4|-f lavfi -i smptehdbars=size=312x230:rate=10:duration=0.5|-vf pad=w=320:h=240:x=4:y=6:color=red,format=yuv420p,setparams=${BT709} -c:v libx264 -qp 0 -g 5 -bsf:v h264_metadata=crop_left=4:crop_right=4:crop_top=6:crop_bottom=4"
        "edge_corner_420.mp4|-f lavfi -i smptehdbars=size=314x236:rate=10:duration=0.5|-vf pad=w=320:h=240:x=0:y=0:color=red,format=yuv420p,setparams=${BT709} -c:v libx264 -qp 0 -g 5 -bsf:v h264_metadata=crop_right=6:crop_bottom=4"
        "edge_offset_422.mp4|-f lavfi -i smptehdbars=size=312x232:rate=10:duration=0.5|-vf format=yuv422p,pad=w=320:h=240:x=4:y=3:color=red,setparams=${BT709} -c:v libx264 -qp 0 -g 5 -bsf:v h264_metadata=crop_left=4:crop_right=4:crop_top=3:crop_bottom=5"
        "edge_offset_444.mp4|-f lavfi -i smptehdbars=size=314x232:rate=10:duration=0.5|-vf format=yuv444p,crop=313:231:0:0,pad=w=320:h=240:x=3:y=5:color=red,setparams=${BT709} -c:v libx264 -qp 0 -g 5 -bsf:v h264_metadata=crop_left=3:crop_right=4:crop_top=5:crop_bottom=4"
        "edge_offset_420_hw.mp4|-f lavfi -i smptehdbars=size=288x208:rate=10:duration=0.5|-vf pad=w=320:h=240:x=16:y=16:color=red,format=yuv420p,setparams=${BT709} -c:v libx264 -qp 2 -x264-params no-deblock=1 -g 5 -bsf:v h264_metadata=crop_left=16:crop_right=16:crop_top=16:crop_bottom=16"
        "edge_corner_420_hw.mp4|-f lavfi -i smptehdbars=size=304x224:rate=10:duration=0.5|-vf pad=w=320:h=240:x=0:y=0:color=red,format=yuv420p,setparams=${BT709} -c:v libx264 -qp 2 -x264-params no-deblock=1 -g 5 -bsf:v h264_metadata=crop_right=16:crop_bottom=16"
        "edge_offset_444_hw.mp4|-f lavfi -i smptehdbars=size=314x232:rate=10:duration=0.5|-vf format=yuv444p,crop=313:231:0:0,pad=w=320:h=240:x=3:y=5:color=red,setparams=${BT709} -c:v libx265 -x265-params log-level=error:lossless=1 -g 5 -bsf:v hevc_metadata=crop_left=3:crop_right=4:crop_top=5:crop_bottom=4"
        "edge_offset_444_10_hw.mp4|-f lavfi -i smptehdbars=size=314x232:rate=10:duration=0.5|-vf format=yuv444p10le,crop=313:231:0:0,pad=w=320:h=240:x=3:y=5:color=red,setparams=${BT709} -c:v libx265 -x265-params log-level=error:lossless=1 -g 5 -bsf:v hevc_metadata=crop_left=3:crop_right=4:crop_top=5:crop_bottom=4"
        "tag_p3.mp4|-f lavfi -i testsrc2=size=64x48:rate=10:duration=0.3|-vf format=yuv420p,setparams=colorspace=bt709:color_primaries=smpte432:color_trc=bt709:range=tv -c:v libx264"
        "tag_bt470m.mp4|-f lavfi -i testsrc2=size=64x48:rate=10:duration=0.3|-vf format=yuv420p,setparams=colorspace=bt709:color_primaries=bt470m:color_trc=bt709:range=tv -c:v libx264"
        "tag_linear.mp4|-f lavfi -i testsrc2=size=64x48:rate=10:duration=0.3|-vf format=yuv420p,setparams=colorspace=bt709:color_primaries=bt709:color_trc=linear:range=tv -c:v libx264"
        "tag_pq.mp4|-f lavfi -i testsrc2=size=64x48:rate=10:duration=0.3|-vf format=yuv420p,setparams=colorspace=bt709:color_primaries=bt709:color_trc=smpte2084:range=tv -c:v libx264"
        "tag_hlg.mp4|-f lavfi -i testsrc2=size=64x48:rate=10:duration=0.3|-vf format=yuv420p,setparams=colorspace=bt709:color_primaries=bt709:color_trc=arib-std-b67:range=tv -c:v libx264"
        "tag_bt2020.mp4|-f lavfi -i testsrc2=size=64x48:rate=10:duration=0.3|-vf format=yuv420p,setparams=colorspace=bt2020nc:color_primaries=bt2020:color_trc=bt2020-10:range=tv -c:v libx264"
        "tag_bt2020_matrix.mp4|-f lavfi -i testsrc2=size=64x48:rate=10:duration=0.3|-vf format=yuv420p,setparams=colorspace=bt2020nc:color_primaries=bt709:color_trc=bt709:range=tv -c:v libx264"
        "tag_bt709.mp4|-f lavfi -i testsrc2=size=64x48:rate=10:duration=0.3|-vf format=yuv420p,setparams=${BT709} -c:v libx264"
        "tag_srgb.mp4|-f lavfi -i testsrc2=size=64x48:rate=10:duration=0.3|-vf format=yuv420p,setparams=colorspace=bt709:color_primaries=bt709:color_trc=iec61966-2-1:range=tv -c:v libx264"
        "tag_smpte170m.mp4|-f lavfi -i testsrc2=size=64x48:rate=10:duration=0.3|-vf format=yuv420p,setparams=colorspace=smpte170m:color_primaries=smpte170m:color_trc=smpte170m:range=tv -c:v libx264"
        "tag_bt2020_10_transfer.mp4|-f lavfi -i testsrc2=size=64x48:rate=10:duration=0.3|-vf format=yuv420p,setparams=colorspace=bt709:color_primaries=bt709:color_trc=bt2020-10:range=tv -c:v libx264"
        "shape_part_large.h264|-f lavfi -i smptehdbars=size=320x240:rate=10:duration=0.5|-vf format=yuv420p,setparams=${BT709} -c:v libx264 -g 5 -bsf:v h264_mp4toannexb -f h264"
        "shape_part_small.h264|-f lavfi -i smptehdbars=size=160x120:rate=10:duration=0.5|-vf format=yuv420p,setparams=${BT709} -c:v libx264 -g 5 -bsf:v h264_mp4toannexb -f h264"
        "shape_part_pq.h264|-f lavfi -i smptehdbars=size=320x240:rate=10:duration=0.5|-vf format=yuv420p,setparams=colorspace=bt709:color_primaries=bt709:color_trc=smpte2084:range=tv -c:v libx264 -g 5 -bsf:v h264_mp4toannexb -f h264"
    )
    foreach(RECIPE ${WALLPAPERX_CONVERTER_RECIPES})
        string(REPLACE "|" ";" RECIPE_PARTS "${RECIPE}")
        list(GET RECIPE_PARTS 0 ASSET_NAME)
        list(GET RECIPE_PARTS 1 ASSET_INPUT)
        list(GET RECIPE_PARTS 2 ASSET_ENCODE)
        separate_arguments(ASSET_INPUT UNIX_COMMAND "${ASSET_INPUT}")
        separate_arguments(ASSET_ENCODE UNIX_COMMAND "${ASSET_ENCODE}")
        add_custom_command(
            OUTPUT ${WALLPAPERX_VIDEO_ASSET_DIR}/${ASSET_NAME}
            COMMAND ${WALLPAPERX_FFMPEG_EXECUTABLE} -y -v error ${ASSET_INPUT} ${ASSET_ENCODE}
                    ${WALLPAPERX_VIDEO_ASSET_DIR}/${ASSET_NAME}
            COMMENT "Generating video fixture ${ASSET_NAME}"
            VERBATIM
        )
        list(APPEND WALLPAPERX_VIDEO_ASSETS ${WALLPAPERX_VIDEO_ASSET_DIR}/${ASSET_NAME})
    endforeach()
    # Two elementary streams back to back: a picture that changes size, and one whose transfer
    # changes to PQ at the same size. Raw Annex B concatenates byte for byte.
    foreach(PAIR "shape_resize.h264|shape_part_large.h264|shape_part_small.h264"
                 "shape_tags.h264|shape_part_large.h264|shape_part_pq.h264")
        string(REPLACE "|" ";" PAIR_PARTS "${PAIR}")
        list(GET PAIR_PARTS 0 PAIR_NAME)
        list(GET PAIR_PARTS 1 PAIR_FIRST)
        list(GET PAIR_PARTS 2 PAIR_SECOND)
        add_custom_command(
            OUTPUT ${WALLPAPERX_VIDEO_ASSET_DIR}/${PAIR_NAME}
            COMMAND ${WALLPAPERX_FFMPEG_EXECUTABLE} -y -v error
                    -i "concat:${WALLPAPERX_VIDEO_ASSET_DIR}/${PAIR_FIRST}|${WALLPAPERX_VIDEO_ASSET_DIR}/${PAIR_SECOND}"
                    -c copy -f h264 ${WALLPAPERX_VIDEO_ASSET_DIR}/${PAIR_NAME}
            DEPENDS ${WALLPAPERX_VIDEO_ASSET_DIR}/${PAIR_FIRST} ${WALLPAPERX_VIDEO_ASSET_DIR}/${PAIR_SECOND}
            COMMENT "Generating video fixture ${PAIR_NAME}"
            VERBATIM
        )
        list(APPEND WALLPAPERX_VIDEO_ASSETS ${WALLPAPERX_VIDEO_ASSET_DIR}/${PAIR_NAME})
    endforeach()
    add_custom_target(wallpaperx-video-fixtures DEPENDS ${WALLPAPERX_VIDEO_ASSETS})
else()
    message(STATUS "  ffmpeg CLI: not found -- video decoder fixtures are not generated and "
                   "video_decoder_probe entries will report a skip")
endif()

# The ffmpeg bridge: every path state, the refusal-versus-fault classifier, and the frame's
# semaphore protocol. Interposes av_hwframe_ctx_init, vkQueueSubmit and
# vkEnumerateDeviceExtensionProperties -- the first two so a failure that no available file can
# produce is produced anyway, the third so "this device has no decoder at all" is answerable on a
# GPU that has one. Its fixtures are its own directory, so it shares no RESOURCE_LOCK with the
# probes that write into test_assets.
add_executable(wallpaperx-video-decoder-probe tests/video_decoder_probe.cpp)
target_link_libraries(wallpaperx-video-decoder-probe PRIVATE wallpaperx-renderer)
target_compile_definitions(wallpaperx-video-decoder-probe PRIVATE
    WALLPAPERX_VIDEO_ASSET_DIR="${WALLPAPERX_VIDEO_ASSET_DIR}")
if(WALLPAPERX_FFMPEG_EXECUTABLE)
    add_dependencies(wallpaperx-video-decoder-probe wallpaperx-video-fixtures)
endif()

# The colour conversion pass, graded against libswscale told the matrix and range explicitly.
# libswscale is a test-only dependency and optional: without it the entries that need a reference
# exit 77 with the reason. Replaces operator new to count allocations, which a sanitizer's
# allocator cannot coexist with, so the counter compiles out there and its entry skips.
pkg_check_modules(SWSCALE IMPORTED_TARGET libswscale)
add_executable(wallpaperx-frame-converter-probe tests/frame_converter_probe.cpp)
target_link_libraries(wallpaperx-frame-converter-probe PRIVATE wallpaperx-renderer)
target_compile_definitions(wallpaperx-frame-converter-probe PRIVATE
    WALLPAPERX_VIDEO_ASSET_DIR="${WALLPAPERX_VIDEO_ASSET_DIR}"
    WALLPAPERX_SHADER_DIR="${SHADER_OUTPUT_DIR}")
add_dependencies(wallpaperx-frame-converter-probe wallpaperx-shaders)
if(SWSCALE_FOUND)
    target_link_libraries(wallpaperx-frame-converter-probe PRIVATE PkgConfig::SWSCALE)
    target_compile_definitions(wallpaperx-frame-converter-probe PRIVATE WALLPAPERX_HAVE_SWSCALE)
else()
    message(STATUS "  libswscale: not found -- frame_converter colour entries will report a skip")
endif()
if(WALLPAPERX_SANITIZE)
    target_compile_definitions(wallpaperx-frame-converter-probe PRIVATE WALLPAPERX_ALLOCATION_INSTRUMENT=0)
endif()
if(WALLPAPERX_FFMPEG_EXECUTABLE)
    add_dependencies(wallpaperx-frame-converter-probe wallpaperx-video-fixtures)
endif()

# The Device's creation record and its video-decode surface. Interposes vkCreateInstance,
# vkCreateDevice and vkGetPhysicalDeviceQueueFamilyProperties2, so it can compare what an
# accessor reports against the argument the driver was actually given -- the only way to grade
# that without a test-only seam in a production header. Replaces Vulkan entry points and not the
# allocator, so it needs no WALLPAPERX_SANITIZE opt-out. No shaders and no PNG fixture.
add_executable(wallpaperx-device-video-probe tests/device_video_probe.cpp)
target_link_libraries(wallpaperx-device-video-probe PRIVATE wallpaperx-renderer)

add_executable(wallpaperx-handle-type-env-test tests/handle_type_env_test.cpp)
target_link_libraries(wallpaperx-handle-type-env-test PRIVATE wallpaperx-renderer)

add_executable(wallpaperx-validation-grade-probe tests/validation_grade_probe.cpp)
target_link_libraries(wallpaperx-validation-grade-probe PRIVATE wallpaperx-renderer)

enable_testing()
add_test(NAME tier_selection COMMAND wallpaperx-tier-selection-test)
add_test(NAME memory_export COMMAND wallpaperx-memory-export-test)
add_test(NAME gpu_fault COMMAND wallpaperx-gpu-fault-test)

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
# entry". None of these six constructs a Device or calls a vk* entry point, so the
# proprietary-driver SEGV that forces that tier to lavapipe-only cannot reach them.
# backend/CMakeLists.txt previously keyed its disable on the *absence* of a lavapipe
# label, which swept all five out and left tier selection and the handle-type cache
# unexercised under the sanitizer -- while the run still reported green.
set_tests_properties(tier_selection memory_export gpu_fault
                     handle_type_env_default handle_type_env_opaque handle_type_env_dmabuf
                     PROPERTIES LABELS "novulkan")

add_test(NAME device_probe COMMAND wallpaperx-device-probe)
add_test(NAME offscreen_probe COMMAND wallpaperx-offscreen-probe)
add_test(NAME device_affinity_probe COMMAND wallpaperx-device-affinity-probe)
add_test(NAME static_renderer_probe COMMAND wallpaperx-static-renderer-probe)
add_test(NAME image_bounds_probe COMMAND wallpaperx-image-bounds-probe)
add_test(NAME descriptor_allocator_probe COMMAND wallpaperx-descriptor-allocator-probe)
# Three entries, one binary. The three descriptor tiers protect different things and can be
# asked on different hardware, so they cannot share a verdict: on Lavapipe the heap entry
# genuinely cannot be answered (that driver completes outstanding work inside the descriptor
# write, see the probe's header), while the buffer and classic entries are answered there and
# are the only place the whole-set rule is graded in CI. One entry would report all three as a
# skip, which is the wrong answer for two of them.
add_test(NAME descriptor_inflight_heap COMMAND wallpaperx-descriptor-inflight-probe heap)
add_test(NAME descriptor_inflight_buffer COMMAND wallpaperx-descriptor-inflight-probe buffer)
add_test(NAME descriptor_inflight_classic COMMAND wallpaperx-descriptor-inflight-probe classic)
add_test(NAME frame_export_probe COMMAND wallpaperx-frame-export-probe)
# Two entries, one binary. The probe makes two claims that are verifiable on different
# hardware: the round trip needs one usable DRM modifier, while modifier *sensitivity*
# needs two, so a wrong one can be substituted. Lavapipe offers exactly one (LINEAR), so
# it verifies the first and cannot verify the second. Reporting both as a single verdict
# means either claiming the control ran when it did not, or marking the whole probe
# unverified when the round trip demonstrably passed.
add_test(NAME modifier_roundtrip_probe COMMAND wallpaperx-modifier-roundtrip-probe roundtrip)
add_test(NAME modifier_sensitivity_probe COMMAND wallpaperx-modifier-roundtrip-probe sensitivity)
add_test(NAME ownership_transfer_probe COMMAND wallpaperx-ownership-transfer-probe)
add_test(NAME consumer_modifier_probe COMMAND wallpaperx-consumer-modifier-probe)
add_test(NAME frame_acquire_probe COMMAND wallpaperx-frame-acquire-probe)
add_test(NAME target_ring_probe COMMAND wallpaperx-target-ring-probe)
# Two entries, one binary, and they answer on different hardware. `render` needs nothing but a
# Device, so it is answerable everywhere. `modifiers` grades that a DRM format modifier whose
# tiling features lack a required one is dropped, and telling that apart from a filter that
# drops everything needs a driver offering two modifiers that differ in some feature -- which
# Lavapipe, with its single LINEAR modifier, cannot. One verdict for both would either claim
# that measurement was made where it was not, or mark a working renderer unverified.
add_test(NAME video_renderer_probe COMMAND wallpaperx-video-renderer-probe render)
add_test(NAME video_modifier_filter_probe COMMAND wallpaperx-video-renderer-probe modifiers)

# Six entries, one binary, because they answer different questions on different configurations.
# `enabled` is registered three times so the feature chain is walked once per descriptor tier:
# which structs are linked is a function of the tier, and the default entry only ever sees
# whichever tier this GPU resolves to, leaving the other two shapes ungraded. `decode` grades the
# invariants that hold with and without a hardware decoder, plus the family shape where there is
# one. `shared-decode-queue` manufactures a queue-family report in which the graphics family also
# decodes -- neither driver here does -- and exits 77 where the driver enables no video extension
# at all, since the report cannot be manufactured onto a device that has none.
add_test(NAME device_video_enabled COMMAND wallpaperx-device-video-probe enabled)
add_test(NAME device_video_enabled_buffer COMMAND wallpaperx-device-video-probe enabled)
add_test(NAME device_video_enabled_classic COMMAND wallpaperx-device-video-probe enabled)
add_test(NAME device_video_decode COMMAND wallpaperx-device-video-probe decode)
add_test(NAME device_video_waits COMMAND wallpaperx-device-video-probe waits)
add_test(NAME device_video_shared_queue COMMAND wallpaperx-device-video-probe shared-decode-queue)
set_tests_properties(device_video_enabled_buffer PROPERTIES ENVIRONMENT "WALLPAPERX_FORCE_TIER=buffer")
set_tests_properties(device_video_enabled_classic PROPERTIES ENVIRONMENT "WALLPAPERX_FORCE_TIER=classic")
# Explicitly cleared, like handle_type_env_default: ctest inherits the caller's environment, so a
# developer who exported the override would otherwise turn this into a fourth copy of it.
set_tests_properties(device_video_enabled PROPERTIES ENVIRONMENT "WALLPAPERX_FORCE_TIER=")
set_tests_properties(device_video_shared_queue PROPERTIES SKIP_RETURN_CODE 77)

# Six entries, one binary. Each needs a different device or a different injected failure, and a
# single verdict would report states that were never reached as verified: `hardware`, `refused`,
# `refused-after-setup` and `fault` all need a GPU that actually decodes and skip where there is
# none, while `no-decoder` and `content` build a device with the video extensions stripped and
# answer everywhere. `refused-after-setup` and `fault` are each other's control -- the same
# injected ffmpeg failure, differing only in whether the device still answers -- so they must be
# separate entries or one of the two outcomes goes ungraded.
#
# SKIP_RETURN_CODE on all six: without a hardware decoder, or without the ffmpeg command line at
# configure time, these exit 0 and would be indistinguishable from a run that established
# something.
add_test(NAME video_decoder_hardware COMMAND wallpaperx-video-decoder-probe hardware)
add_test(NAME video_decoder_refused COMMAND wallpaperx-video-decoder-probe refused)
add_test(NAME video_decoder_no_decoder COMMAND wallpaperx-video-decoder-probe no-decoder)
add_test(NAME video_decoder_refused_after_setup COMMAND wallpaperx-video-decoder-probe refused-after-setup)
add_test(NAME video_decoder_fault COMMAND wallpaperx-video-decoder-probe fault)
add_test(NAME video_decoder_content COMMAND wallpaperx-video-decoder-probe content)
set_tests_properties(video_decoder_hardware video_decoder_refused video_decoder_no_decoder
                     video_decoder_refused_after_setup video_decoder_fault video_decoder_content
                     PROPERTIES SKIP_RETURN_CODE 77)

# One entry per mode: each builds a different device -- video extensions stripped for the host-path
# entries, a feature hidden, a plane format made unfilterable -- and one verdict would report
# unreached states as verified. The device-path, gate, no-ycbcr and shape-fallback entries need a
# hardware decoder and skip without one. shader_read opens no driver. SKIP_RETURN_CODE throughout:
# a missing ffmpeg command line, libswscale or decoder must not read as a pass.
add_test(NAME frame_converter_colour_host COMMAND wallpaperx-frame-converter-probe colour-host)
add_test(NAME frame_converter_colour_device COMMAND wallpaperx-frame-converter-probe colour-device)
add_test(NAME frame_converter_edges_host COMMAND wallpaperx-frame-converter-probe edges-host)
add_test(NAME frame_converter_edges_device COMMAND wallpaperx-frame-converter-probe edges-device)
add_test(NAME frame_converter_siting COMMAND wallpaperx-frame-converter-probe siting)
add_test(NAME frame_converter_refusal COMMAND wallpaperx-frame-converter-probe refusal)
add_test(NAME frame_converter_output COMMAND wallpaperx-frame-converter-probe output)
add_test(NAME frame_converter_allocation COMMAND wallpaperx-frame-converter-probe allocation)
add_test(NAME frame_converter_gate COMMAND wallpaperx-frame-converter-probe gate)
add_test(NAME frame_converter_no_ycbcr COMMAND wallpaperx-frame-converter-probe no-ycbcr)
add_test(NAME frame_converter_shape_resize COMMAND wallpaperx-frame-converter-probe shape-resize)
add_test(NAME frame_converter_shape_tags COMMAND wallpaperx-frame-converter-probe shape-tags)
add_test(NAME frame_converter_shape_fallback COMMAND wallpaperx-frame-converter-probe shape-fallback)
add_test(NAME frame_converter_shader_read COMMAND wallpaperx-frame-converter-probe shader-read)
set_tests_properties(frame_converter_colour_host frame_converter_colour_device frame_converter_edges_host
                     frame_converter_edges_device frame_converter_siting frame_converter_refusal
                     frame_converter_output frame_converter_allocation frame_converter_gate
                     frame_converter_no_ycbcr frame_converter_shape_resize frame_converter_shape_tags
                     frame_converter_shape_fallback
                     frame_converter_shader_read
                     PROPERTIES SKIP_RETURN_CODE 77)
set_tests_properties(frame_converter_shader_read PROPERTIES LABELS "novulkan")

# 77 means this driver cannot export an opaque-fd binary semaphore at all, so there is no acquire
# descriptor to wait on -- Lavapipe. That run still grades what such a driver must do (hand back
# no descriptor rather than fail the export) and says the rest was not exercised. Its own PNG
# fixture, so its two entries share a lock of their own.
set_tests_properties(frame_acquire_probe PROPERTIES
                     RESOURCE_LOCK frame_acquire_probe_test_image SKIP_RETURN_CODE 77)

# 77 means the driver has no dma_buf with a describable layout, or offers only one usable
# modifier so narrowing cannot be observed. Its own PNG fixture, hence its own lock.
set_tests_properties(consumer_modifier_probe PROPERTIES
                     RESOURCE_LOCK consumer_modifier_probe_test_image SKIP_RETURN_CODE 77)

# 77 means the driver could not export at all, so no ownership transfer was ever issued
# and the pairing rule was not exercised. Its own PNG fixture, so its two entries share a
# lock of their own.
set_tests_properties(ownership_transfer_probe PROPERTIES
                     RESOURCE_LOCK ownership_transfer_probe_test_image SKIP_RETURN_CODE 77)

# 77 means the tier could not be put in the state the entry grades: the tier is absent on
# this driver, or nothing kept a batch in flight across the descriptor write. Without
# SKIP_RETURN_CODE such a run exits 0 and is indistinguishable from one that established the
# rule -- the same wrong-green this probe exists to close.
set_tests_properties(descriptor_inflight_heap descriptor_inflight_buffer descriptor_inflight_classic
                     PROPERTIES SKIP_RETURN_CODE 77)

# 77 means the driver could not be put in the state the modifier entry grades: no DRM format
# modifiers at all, none usable for a target-shaped image, or -- the Lavapipe case -- only one,
# so no feature splits the set and a filter that keeps the advertisers cannot be told from one
# that drops everything. Without SKIP_RETURN_CODE that run exits 0 and reads as a verified
# filter. The render entry has no such code: everything it grades is available wherever a
# Device comes up.
set_tests_properties(video_modifier_filter_probe PROPERTIES SKIP_RETURN_CODE 77)

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

# Its own four ring_*.png fixtures, and like the others it is one binary registered twice,
# so both entries share a lock. No SKIP_RETURN_CODE: nothing it grades depends on a driver
# capability -- an offscreen image, a render pass and a device-side copy are available
# wherever a Device comes up at all, so an inconclusive outcome has no way to arise.
set_tests_properties(target_ring_probe PROPERTIES RESOURCE_LOCK target_ring_probe_test_image)

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
    add_test(NAME descriptor_inflight_heap_lavapipe COMMAND wallpaperx-descriptor-inflight-probe heap)
    add_test(NAME descriptor_inflight_buffer_lavapipe COMMAND wallpaperx-descriptor-inflight-probe buffer)
    add_test(NAME descriptor_inflight_classic_lavapipe COMMAND wallpaperx-descriptor-inflight-probe classic)
    # Unlike the Heap tier, frame export needs no capability-skip under Lavapipe:
    # Lavapipe advertises both VK_KHR_external_memory_fd and
    # VK_EXT_external_memory_dma_buf, so exportFrame() takes the DMA_BUF path and
    # succeeds here (verified: distinct fds, correct size, zero validation
    # warnings). This is the only place the DMA_BUF branch runs in CI, since the
    # owner's real GPU is NVIDIA (OPAQUE_FD).
    add_test(NAME frame_export_probe_lavapipe COMMAND wallpaperx-frame-export-probe)
    add_test(NAME modifier_roundtrip_probe_lavapipe COMMAND wallpaperx-modifier-roundtrip-probe roundtrip)
    add_test(NAME modifier_sensitivity_probe_lavapipe COMMAND wallpaperx-modifier-roundtrip-probe sensitivity)
    add_test(NAME ownership_transfer_probe_lavapipe COMMAND wallpaperx-ownership-transfer-probe)
    add_test(NAME consumer_modifier_probe_lavapipe COMMAND wallpaperx-consumer-modifier-probe)
    add_test(NAME frame_acquire_probe_lavapipe COMMAND wallpaperx-frame-acquire-probe)
    add_test(NAME target_ring_probe_lavapipe COMMAND wallpaperx-target-ring-probe)
    add_test(NAME video_renderer_probe_lavapipe COMMAND wallpaperx-video-renderer-probe render)
    add_test(NAME video_modifier_filter_probe_lavapipe COMMAND wallpaperx-video-renderer-probe modifiers)
    # The software driver is where "a device with no hardware decoder reports none and still
    # builds, submits and tears down" is actually answered -- the real GPU cannot show it. The
    # other three entries grade properties that hold on any driver and are registered here so CI,
    # which selects by this label, runs them at all.
    add_test(NAME device_video_enabled_lavapipe COMMAND wallpaperx-device-video-probe enabled)
    add_test(NAME device_video_decode_lavapipe COMMAND wallpaperx-device-video-probe decode)
    add_test(NAME device_video_waits_lavapipe COMMAND wallpaperx-device-video-probe waits)
    add_test(NAME device_video_shared_queue_lavapipe COMMAND wallpaperx-device-video-probe shared-decode-queue)
    # Only the two entries that need no hardware decoder. The other four would report a skip here
    # and say nothing the real-GPU entries do not already say; these two exercise the software
    # decode path and the content classifier, which are what CI can actually answer.
    add_test(NAME video_decoder_no_decoder_lavapipe COMMAND wallpaperx-video-decoder-probe no-decoder)
    add_test(NAME video_decoder_content_lavapipe COMMAND wallpaperx-video-decoder-probe content)
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
        descriptor_inflight_heap_lavapipe
        descriptor_inflight_buffer_lavapipe
        descriptor_inflight_classic_lavapipe
        frame_export_probe_lavapipe
        modifier_roundtrip_probe_lavapipe
        modifier_sensitivity_probe_lavapipe
        ownership_transfer_probe_lavapipe
        consumer_modifier_probe_lavapipe
        frame_acquire_probe_lavapipe
        target_ring_probe_lavapipe
        video_renderer_probe_lavapipe
        video_modifier_filter_probe_lavapipe
        device_video_enabled_lavapipe
        device_video_decode_lavapipe
        device_video_waits_lavapipe
        device_video_shared_queue_lavapipe
        video_decoder_no_decoder_lavapipe
        video_decoder_content_lavapipe
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
    set_tests_properties(descriptor_inflight_heap_lavapipe descriptor_inflight_buffer_lavapipe
                         descriptor_inflight_classic_lavapipe PROPERTIES SKIP_RETURN_CODE 77)
    set_tests_properties(frame_export_probe_lavapipe PROPERTIES RESOURCE_LOCK frame_export_probe_test_image)
    set_tests_properties(modifier_roundtrip_probe_lavapipe modifier_sensitivity_probe_lavapipe
                         PROPERTIES RESOURCE_LOCK modifier_roundtrip_probe_test_image SKIP_RETURN_CODE 77)
    set_tests_properties(ownership_transfer_probe_lavapipe PROPERTIES
                         RESOURCE_LOCK ownership_transfer_probe_test_image SKIP_RETURN_CODE 77)
    set_tests_properties(consumer_modifier_probe_lavapipe PROPERTIES
                         RESOURCE_LOCK consumer_modifier_probe_test_image SKIP_RETURN_CODE 77)
    set_tests_properties(frame_acquire_probe_lavapipe PROPERTIES
                         RESOURCE_LOCK frame_acquire_probe_test_image SKIP_RETURN_CODE 77)
    set_tests_properties(validation_grade_installed_lavapipe validation_grade_absent_lavapipe
                         PROPERTIES SKIP_RETURN_CODE 77)
    # Lavapipe offers one modifier, so this entry is expected to report the split half as
    # unverified and exit 77 there -- see the comment on the non-lavapipe entry.
    set_tests_properties(video_modifier_filter_probe_lavapipe PROPERTIES SKIP_RETURN_CODE 77)
    # Expected to skip here: no video extension means no decode family to manufacture one onto.
    set_tests_properties(device_video_shared_queue_lavapipe PROPERTIES SKIP_RETURN_CODE 77)
    # 77 here means the ffmpeg command line was missing at configure time and no fixture exists.
    set_tests_properties(video_decoder_no_decoder_lavapipe video_decoder_content_lavapipe
                         PROPERTIES SKIP_RETURN_CODE 77)
    # The converter's entries that need no hardware decoder. On this driver every stream is decoded
    # in software, so these are its host path; the device path is graded on the real GPU only.
    add_test(NAME frame_converter_colour_host_lavapipe COMMAND wallpaperx-frame-converter-probe colour-host)
    add_test(NAME frame_converter_edges_host_lavapipe COMMAND wallpaperx-frame-converter-probe edges-host)
    add_test(NAME frame_converter_siting_lavapipe COMMAND wallpaperx-frame-converter-probe siting)
    add_test(NAME frame_converter_refusal_lavapipe COMMAND wallpaperx-frame-converter-probe refusal)
    add_test(NAME frame_converter_output_lavapipe COMMAND wallpaperx-frame-converter-probe output)
    add_test(NAME frame_converter_allocation_lavapipe COMMAND wallpaperx-frame-converter-probe allocation)
    add_test(NAME frame_converter_shape_resize_lavapipe COMMAND wallpaperx-frame-converter-probe shape-resize)
    add_test(NAME frame_converter_shape_tags_lavapipe COMMAND wallpaperx-frame-converter-probe shape-tags)
    set_tests_properties(frame_converter_colour_host_lavapipe frame_converter_edges_host_lavapipe
                         frame_converter_siting_lavapipe frame_converter_refusal_lavapipe
                         frame_converter_output_lavapipe frame_converter_allocation_lavapipe
                         frame_converter_shape_resize_lavapipe frame_converter_shape_tags_lavapipe
                         PROPERTIES ENVIRONMENT "VK_ICD_FILENAMES=${WALLPAPERX_LAVAPIPE_ICD}"
                                    LABELS "lavapipe" SKIP_RETURN_CODE 77)
endif()
