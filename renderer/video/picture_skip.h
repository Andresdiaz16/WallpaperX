// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace wallpaperx {

// Which pictures a VideoDecoder call may leave undecoded. Unreferenced: those the stream marks as no
// other picture's reference, so each picture returned decodes exactly as it would in full and its
// timestamp still places it. How many there are is the encoder's choice. ffmpeg's H.264 and HEVC
// decoders honour it, in software and on the device, software's frame threads a few packets late.
// AV1 skips nothing: the device's decoder ignores it and libdav1d reads it only when opened. Apart
// from video_decoder.h so a caller can choose one without ffmpeg's headers.
enum class PictureSkip { Nothing, Unreferenced };

}
