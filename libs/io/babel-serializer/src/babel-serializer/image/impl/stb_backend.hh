#pragma once

#include <babel-serializer/fwd.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>

// Backend seam for the image codecs — the ONLY place stb is reached.
//
// These declarations are backend-neutral: no stb type appears here, so png / jpg / image never see stb_image.h.
// stb_backend.cc is the single TU that includes the stb headers and links the vendored `stb` target (PRIVATE).
// A future hand-rolled per-format decoder replaces the body of one of these — the signatures stay put.
//
// Internal to babel — this header is NOT part of the public FILE_SET.

namespace babel::impl
{
/// Raw pixels from a whole-buffer decode: tightly packed, 8-bit samples, row-major, top-left origin.
struct stb_image
{
    int width = 0;
    int height = 0;
    int channels = 0; // samples per pixel actually produced (1 grey / 2 GA / 3 rgb / 4 rgba)
    cc::vector<cc::byte> pixels;
};

/// Decode a whole in-memory image (any stb-supported format) to 8-bit pixels.
/// req_channels forces the output channel count (1..4); 0 keeps the file's own channel count.
/// stb owns its buffer only transiently — the pixels are copied out and stb's buffer freed before returning.
[[nodiscard]] cc::result<stb_image> stb_decode(cc::span<cc::byte const> bytes, int req_channels = 0);

/// Encode tightly-packed 8-bit pixels to the format's file bytes. width / height / channels describe `pixels`.
[[nodiscard]] cc::result<cc::vector<cc::byte>> stb_encode_png(cc::span<cc::byte const> pixels,
                                                              int width,
                                                              int height,
                                                              int channels);
[[nodiscard]] cc::result<cc::vector<cc::byte>> stb_encode_jpg(cc::span<cc::byte const> pixels,
                                                              int width,
                                                              int height,
                                                              int channels,
                                                              int quality);
} // namespace babel::impl
