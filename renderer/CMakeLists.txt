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

add_executable(wallpaperx-device-probe tests/device_probe.cpp)
target_link_libraries(wallpaperx-device-probe PRIVATE wallpaperx-renderer)

add_executable(wallpaperx-tier-selection-test tests/tier_selection_test.cpp)
target_link_libraries(wallpaperx-tier-selection-test PRIVATE wallpaperx-renderer)

add_executable(wallpaperx-memory-export-test tests/memory_export_test.cpp)
target_link_libraries(wallpaperx-memory-export-test PRIVATE wallpaperx-renderer)

add_executable(wallpaperx-offscreen-probe tests/offscreen_probe.cpp)
target_link_libraries(wallpaperx-offscreen-probe PRIVATE wallpaperx-renderer)

add_executable(wallpaperx-static-renderer-probe tests/static_renderer_probe.cpp)
target_link_libraries(wallpaperx-static-renderer-probe PRIVATE wallpaperx-renderer)
add_dependencies(wallpaperx-static-renderer-probe wallpaperx-shaders)
target_compile_definitions(wallpaperx-static-renderer-probe PRIVATE
    WALLPAPERX_SHADER_DIR="${SHADER_OUTPUT_DIR}"
    WALLPAPERX_TEST_IMAGE_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_assets"
)

add_executable(wallpaperx-descriptor-allocator-probe tests/descriptor_allocator_probe.cpp)
target_link_libraries(wallpaperx-descriptor-allocator-probe PRIVATE wallpaperx-renderer)

enable_testing()
add_test(NAME tier_selection COMMAND wallpaperx-tier-selection-test)
add_test(NAME memory_export COMMAND wallpaperx-memory-export-test)
add_test(NAME device_probe COMMAND wallpaperx-device-probe)
add_test(NAME offscreen_probe COMMAND wallpaperx-offscreen-probe)
add_test(NAME static_renderer_probe COMMAND wallpaperx-static-renderer-probe)
add_test(NAME descriptor_allocator_probe COMMAND wallpaperx-descriptor-allocator-probe)

# static_renderer_probe and its _lavapipe twin run the same binary and both
# read/write the same WALLPAPERX_TEST_IMAGE_DIR path -- lock them to the same
# resource so ctest never runs them concurrently (e.g. under `ctest -jN`),
# which would otherwise risk one process reading a half-written PNG from
# the other.
set_tests_properties(static_renderer_probe PROPERTIES RESOURCE_LOCK static_renderer_probe_test_image)

if(WALLPAPERX_LAVAPIPE_ICD)
    add_test(NAME device_probe_lavapipe COMMAND wallpaperx-device-probe)
    add_test(NAME offscreen_probe_lavapipe COMMAND wallpaperx-offscreen-probe)
    add_test(NAME static_renderer_probe_lavapipe COMMAND wallpaperx-static-renderer-probe)
    add_test(NAME descriptor_allocator_probe_lavapipe COMMAND wallpaperx-descriptor-allocator-probe)
    set_tests_properties(
        device_probe_lavapipe
        offscreen_probe_lavapipe
        static_renderer_probe_lavapipe
        descriptor_allocator_probe_lavapipe
        PROPERTIES
            # WALLPAPERX_ALLOW_NO_HEAP_TIER tells descriptor_allocator_probe's
            # Heap-tier tests that resolving to a non-Heap tier here is expected
            # (Lavapipe doesn't implement VK_EXT_descriptor_heap) rather than a
            # regression -- see heapTierGuard() in descriptor_allocator_probe.cpp.
            ENVIRONMENT "VK_ICD_FILENAMES=${WALLPAPERX_LAVAPIPE_ICD};WALLPAPERX_ALLOW_NO_HEAP_TIER=1"
            LABELS "lavapipe"
    )
    set_tests_properties(static_renderer_probe_lavapipe PROPERTIES RESOURCE_LOCK static_renderer_probe_test_image)
endif()
