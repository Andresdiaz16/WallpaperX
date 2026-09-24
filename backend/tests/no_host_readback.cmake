# cmake -DBINARY=<path to wallpaperx-daemon> -P no_host_readback.cmake
#
# GOAL: S13.9 -- no code shipped in the daemon can copy a decoded picture into system memory: it names
#       none of the calls that do (ffmpeg's hardware-frame download or map, libswscale, a Vulkan
#       image-to-buffer or image-to-host copy).
# WHY-THIS-SHAPE: read from the linked binary, not the sources, so a call added in any file the daemon
#       links -- renderer or daemon -- is seen, and so is a name fetched through vkGetDeviceProcAddr,
#       whose string sits in the binary. A runtime interposer would only see a copy on the path one
#       fixture happens to drive, and a cost measurement would be an assertion on the scheduler. What it
#       cannot see is a read-back ffmpeg performs inside itself unasked; nothing here asks it to.
# CONTROL: renderer/video/video_decoder.cpp DecodedFrame's constructor downloading each device picture
#       with av_hwframe_transfer_data, reverted: "names av_hwframe_transfer_data". The same constructor
#       looking up vkCmdCopyImageToBuffer2 through vkGetDeviceProcAddr, reverted: "names
#       vkCmdCopyImageToBuffer2".
# COVERAGE-ASSERT: the binary must name the calls the hardware decode path does make; /usr/bin/true
#       fails "does not name av_hwframe_ctx_init", a missing path "no daemon binary to read".

if(NOT DEFINED BINARY OR NOT EXISTS "${BINARY}")
    message(FATAL_ERROR "FAIL: no daemon binary to read at '${BINARY}'")
endif()

set(forbidden
    "av_hwframe_transfer_data" "av_hwframe_map" "sws_scale" "sws_scale_frame" "sws_getContext"
    "vkCmdCopyImageToBuffer" "vkCmdCopyImageToBuffer2" "vkCmdCopyImageToBuffer2KHR"
    "vkCopyImageToMemoryEXT")
set(required "av_hwframe_ctx_init" "avcodec_get_hw_frames_parameters" "avcodec_receive_frame")

file(STRINGS "${BINARY}" names REGEX "^[A-Za-z_][A-Za-z0-9_]*$")
list(REMOVE_DUPLICATES names)

set(failed FALSE)
foreach(name IN LISTS required)
    if(NOT name IN_LIST names)
        message(SEND_ERROR "FAIL: ${BINARY} does not name ${name}, which the hardware decode path calls -- "
                           "this is not the daemon, so nothing below was checked")
        set(failed TRUE)
    endif()
endforeach()
foreach(name IN LISTS forbidden)
    if(name IN_LIST names)
        message(SEND_ERROR "FAIL: ${BINARY} names ${name}: something the daemon links can copy a decoded "
                           "picture into system memory")
        set(failed TRUE)
    endif()
endforeach()
if(failed)
    message(FATAL_ERROR "no_host_readback: failed")
endif()
list(LENGTH forbidden checked)
message(STATUS "no_host_readback: ${BINARY} names none of ${checked} read-back calls")
