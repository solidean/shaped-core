#pragma once

#include <babel-data/fwd.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>

// base64 codec (data/).
//
// Tolerant on input, canonical on output.
// Decoding accepts BOTH RFC 4648 alphabets — standard ('+' / '/') and URL-safe ('-' / '_'), even mixed
// within one input — treats the '=' padding as optional, and skips ASCII whitespace between characters.
// Encoding always emits the standard alphabet with padding.
//
// There is no streaming interface, and deliberately so: base64 payloads in practice are data URIs and
// blobs embedded in a text format, which the caller already holds whole in memory.
//
//   auto const bytes = babel::base64::decode("Zm9vYmFy").value(); // "foobar"

namespace babel::base64
{
/// Number of bytes `decode` will produce for `text`, or nullopt when `text` is not valid base64.
[[nodiscard]] cc::optional<isize> decoded_size(cc::string_view text);

/// Decode `text` into a fresh buffer.
/// Fails on a character outside both alphabets, on a data character after padding,
/// and on a final quantum of a single character (which encodes no byte at all).
[[nodiscard]] cc::result<cc::vector<byte>> decode(cc::string_view text);

/// Decode `text` into caller-provided storage and return the number of bytes written.
/// `out` must hold at least `decoded_size(text)` bytes; too small is an ordinary error, not an assert.
[[nodiscard]] cc::result<isize> decode_into(cc::string_view text, cc::span<byte> out);

/// Encode `bytes` with the standard alphabet ('+' / '/'), padded to a multiple of 4 characters.
[[nodiscard]] cc::string encode(cc::span<byte const> bytes);
} // namespace babel::base64
