#pragma once

#include <clean-core/bytes/compression.hh>
#include <clean-core/common/utility.hh> // cc::unit, for the validating entry points below

// The seam every compression algorithm plugs into.
//
// Adding an algorithm means four edits, and the fourth is the one that compiles cleanly when forgotten:
// an enum value, a .cc defining a table, a line in backend_for, its sources in clean-core's CMakeLists — and a line in cc::detect_algorithm, without which framed blobs of it are never sniffed.
//
// Each upstream header stays inside its own backend TU, which is why every entry point speaks in cc types and an untyped context pointer.

namespace cc::impl
{
struct compression_backend
{
    [[nodiscard]] isize (*compress_bound)(isize size, compression_config const& cfg);

    /// The compressing context, created once per cc::compressor and carrying the prepared dictionary where the algorithm has one.
    /// Never null on success; creation failure surfaces from compress_into.
    [[nodiscard]] void* (*create_compressor)(compression_config const& cfg);
    void (*destroy_compressor)(void* state);
    [[nodiscard]] cc::result<isize> (*compress_into)(void* state,
                                                     compression_config const& cfg,
                                                     cc::span<byte const> data,
                                                     cc::span<byte> out);

    [[nodiscard]] void* (*create_decompressor)(decompression_config const& cfg);
    void (*destroy_decompressor)(void* state);
    [[nodiscard]] cc::result<isize> (*decompress_into)(void* state,
                                                       decompression_config const& cfg,
                                                       cc::span<byte const> data,
                                                       cc::span<byte> out);

    /// Decompress without being told the size, growing the output as it goes.
    /// A declared content size only ever seeds the first allocation, which is why this cannot be expressed on top of decompress_into.
    [[nodiscard]] cc::result<cc::vector<byte>> (*decompress_to_vector)(void* state,
                                                                       decompression_config const& cfg,
                                                                       cc::span<byte const> data);

    /// The uncompressed size the frame declares, or nullopt when it declares none.
    [[nodiscard]] cc::optional<isize> (*declared_size)(cc::span<byte const> data);

    /// Whether `data` opens with this algorithm's frame magic.
    [[nodiscard]] bool (*matches_magic)(cc::span<byte const> data);

    [[nodiscard]] cc::result<cc::vector<byte>> (*train_dictionary)(cc::span<cc::span<byte const> const> samples,
                                                                   isize dict_size);
    [[nodiscard]] u32 (*dictionary_id)(cc::span<byte const> raw);
};

[[nodiscard]] compression_backend const& zstd_backend();
[[nodiscard]] compression_backend const& lz4_backend();

[[nodiscard]] compression_backend const& backend_for(compression_algorithm algorithm);

/// The shared precondition every entry point checks before touching a backend: the dictionary, if any, is for this
/// algorithm, and `raw` framing was given an explicit algorithm to work with.
[[nodiscard]] cc::result<cc::unit> validate_compression_config(compression_config const& cfg);
[[nodiscard]] cc::result<cc::unit> validate_decompression_config(decompression_config const& cfg);
} // namespace cc::impl
