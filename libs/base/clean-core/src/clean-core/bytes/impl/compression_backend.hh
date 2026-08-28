#pragma once

#include <clean-core/bytes/compression.hh>
#include <clean-core/common/utility.hh> // cc::unit, for the validating entry points below

// The seam every compression algorithm plugs into.
//
// Adding an algorithm means six edits, and the ones at the end are those that compile cleanly when forgotten:
// an enum value, a .cc defining a table, a line in backend_for, its sources and its link in clean-core's CMakeLists, an allow-include entry in .shaped-lint.yml for the upstream header
// — and a line in cc::detect_algorithm, without which framed blobs of it are never sniffed.
//
// Each upstream header stays inside its own backend TU, which is why every entry point speaks in cc types and an untyped context pointer.

namespace cc::impl
{
/// What one pass of a streaming decompressor moved.
/// `consumed` and `produced` may both be 0 while the codec waits for more input, so neither is an end signal.
struct stream_decompress_step
{
    isize consumed = 0;
    isize produced = 0;
    bool finished = false;
};

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
    /// `data` must be the WHOLE blob rather than a prefix of it — deflate reads gzip's trailer, not its header.
    [[nodiscard]] cc::optional<isize> (*declared_size)(cc::span<byte const> data);

    /// Whether declared_size can be answered from the first bytes of a frame.
    /// A streaming reader only ever holds a prefix, so it may probe for a size hint only when this is true.
    /// zstd and lz4 declare theirs in the frame header; deflate declares it in gzip's trailer, and a prefix of a gzip
    /// stream has compressed payload where that trailer would be.
    bool declares_size_in_header = false;

    /// Whether `data` opens with this algorithm's frame magic.
    [[nodiscard]] bool (*matches_magic)(cc::span<byte const> data);

    [[nodiscard]] cc::result<cc::vector<byte>> (*train_dictionary)(cc::span<cc::span<byte const> const> samples,
                                                                   isize dict_size);
    [[nodiscard]] u32 (*dictionary_id)(cc::span<byte const> raw);

    // --- streaming (bytes/compression_stream.hh) ----------------------------------------------------------
    //
    // A context of its own, because all three codecs keep frame state across calls here and none can resume a frame
    // from a context that has served a whole-buffer call.
    // Not `raw`: a raw lz4 blob is one block with no continuation, and a raw zstd blob has no header to resume from.
    // Deflate's `zlib` framing streams like `frame` does, its wrapper being a header and a trailing checksum.

    [[nodiscard]] void* (*create_stream_compressor)(compression_config const& cfg);
    void (*destroy_stream_compressor)(void* state);

    /// Output capacity a stream_compress of `in_size` bytes may need, header and epilogue included.
    [[nodiscard]] isize (*stream_compress_bound)(void* state, isize in_size);

    /// Compress all of `in` into `out`, returning bytes written; `finish` seals the frame.
    /// `out` must be at least stream_compress_bound(in.size()), which is what lets this never partially consume.
    [[nodiscard]] cc::result<isize> (*stream_compress)(void* state,
                                                       cc::span<byte const> in,
                                                       cc::span<byte> out,
                                                       bool finish);

    [[nodiscard]] void* (*create_stream_decompressor)(decompression_config const& cfg);
    void (*destroy_stream_decompressor)(void* state);

    /// Decompress from `in` into `out`, moving as much as each side allows.
    /// Partial progress is normal, and `finished` is the only signal that the frame is complete.
    /// `out` is the caller's whole allowance for this call: decompression_config::max_output_size is enforced by
    /// narrowing it, since only the caller knows what earlier calls already produced.
    [[nodiscard]] cc::result<stream_decompress_step> (*stream_decompress)(void* state,
                                                                          cc::span<byte const> in,
                                                                          cc::span<byte> out);
};

[[nodiscard]] compression_backend const& zstd_backend();
[[nodiscard]] compression_backend const& lz4_backend();
[[nodiscard]] compression_backend const& deflate_backend();

[[nodiscard]] compression_backend const& backend_for(compression_algorithm algorithm);

/// The shared precondition every entry point checks before touching a backend: the dictionary, if any, is for this
/// algorithm, and `raw` framing was given an explicit algorithm to work with.
[[nodiscard]] cc::result<cc::unit> validate_compression_config(compression_config const& cfg);
[[nodiscard]] cc::result<cc::unit> validate_decompression_config(decompression_config const& cfg);
} // namespace cc::impl
