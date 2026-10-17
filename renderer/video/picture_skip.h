// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace wallpaperx {

// Which pictures a VideoDecoder call may leave undecoded, each level leaving out more: those no other
// picture references; every picture but intra ones; every picture but those that reset the decoder
// (H.264 IDR or a recovery point, HEVC IRAP). A picture returned decodes as it would in full, and its
// timestamp still places it. No level drops B pictures as such: in a B pyramid P pictures can reference
// them. VideoDecoder::deepestSkip() says how deep each codec, in software or on the device, is asked
// to go.
// Apart from video_decoder.h so a caller can choose one without ffmpeg's headers.
enum class PictureSkip { Nothing, Unreferenced, NonIntra, NonKey };

}
