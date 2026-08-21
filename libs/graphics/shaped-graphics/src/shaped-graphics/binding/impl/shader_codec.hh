#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-graphics/fwd.hh>

/// The byte form of a compiled_shader, for a cache that outlives the process.
///
/// Deliberately independent of cc::byte_stream_builder.
/// That one is a hashing front end whose layout is free to change, and riding it would turn a clean-core tweak into
/// silently corrupt cached shaders.
///
/// Fixed little-endian widths and length prefixes throughout, so the encoding does not depend on the compiler that
/// wrote it — a blob is read back by a different build than made it, which is the whole point.

namespace sg::impl
{
/// The layout revision this build writes.
/// Bump it whenever a field is added, removed or reordered; older blobs then decode to nothing, and a cache reads that as a miss.
inline constexpr u32 k_shader_codec_version = 2;

[[nodiscard]] cc::vector<byte> encode_compiled_shader(compiled_shader const& shader);

/// Nothing when `bytes` is truncated, internally inconsistent, or a version this build does not know.
/// Every doubtful input decodes to nothing rather than to a guess: a cache is allowed to miss, never to lie.
[[nodiscard]] cc::optional<compiled_shader> decode_compiled_shader(cc::span<byte const> bytes);
} // namespace sg::impl
