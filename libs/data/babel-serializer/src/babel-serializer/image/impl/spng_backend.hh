#pragma once

#include <babel-serializer/image/png.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>

// Backend seam for the PNG codec — the ONLY place libspng is reached.
//
// These declarations name no spng type, so png / image never see spng.h.
// spng_backend.cc is the single TU that includes it and links the vendored `spng` target (PRIVATE).
//
// Unlike the stb seam next door, this one speaks babel::png::data rather than a neutral pixel struct.
// Mapping a PNG's chunks onto those fields IS the decode, so a second struct in between would only be png::data spelled twice.
//
// Internal to babel — this header is NOT part of the public FILE_SET.

namespace babel::impl
{
/// Decode a whole PNG buffer: the IHDR fields, the pixels, and every ancillary chunk png::data models.
/// Pixels come back tightly packed, at the channel count the file's color type and tRNS imply, and at the sample
/// width `decoded` reports — u16 for a 16-bit file, u8 for every other depth.
/// The input is untrusted, so the decode caps image dimensions and ancillary-chunk memory; both are errors.
[[nodiscard]] cc::result<babel::png::data> spng_decode_png(cc::span<byte const> bytes);

/// Encode `img`'s pixels and metadata to PNG file bytes.
/// `img.pixels` must be exactly the size `width`, `height`, `channels` and `decoded` imply.
/// `compression_level` is zlib's: 0..9, or -1 for zlib's own default.
[[nodiscard]] cc::result<cc::vector<byte>> spng_encode_png(babel::png::data const& img, int compression_level);
} // namespace babel::impl
