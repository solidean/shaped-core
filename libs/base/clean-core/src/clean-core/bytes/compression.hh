#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/fwd.hh>

// General-purpose compression (bytes/).
//
// The algorithm is a VALUE, not a type.
// Every format that stores compressed bytes has to write down how it compressed them and dispatch on that when reading them back.
//
//   auto const blob = cc::compress(payload);                                  // zstd, default level, framed
//   auto const back = cc::decompress(blob).value();                           // sniffs the frame
//   auto const fast = cc::compress(payload, {.algorithm = cc::compression_algorithm::lz4, .level = -4});
//
// Dictionaries are bytes/compression_dictionary.hh.
//
// Every entry point takes the whole input and produces the whole output, both in memory.
// There is no streaming form yet, and nothing here starts a worker pool behind a caller's back.

/// Which algorithm.
/// Stored in file formats, so the values are stable and only ever appended to.
///
/// zstd is the default, and the right answer unless measured otherwise: far better ratio than lz4 at comparable speed, with low levels fast enough for most hot paths.
/// lz4 is for when decompression speed is the whole requirement — GB/s, at a noticeably worse ratio.
enum class cc::compression_algorithm : cc::u8
{
    zstd = 0,
    lz4 = 1,
};

/// Whether the output describes itself.
///
/// `frame` carries a magic number, usually the uncompressed size, and a checksum.
/// It is what cc::detect_algorithm recognizes, and the only thing cc::decompress can read without being told which algorithm to use.
///
/// `raw` is bare codec output with that header stripped, which stops being noise once the blobs are small and numerous.
/// It can never be sniffed, so a format using it must record the algorithm itself.
/// What is left after that differs by backend, and it is not a uniform "frame minus a few bytes".
/// A raw zstd blob is still a frame with its magic and flags removed, so cc::decompress can grow into it as usual.
/// A raw lz4 blob leaves the frame API entirely for the block API, and carries no length, checksum or terminator at all.
/// So a raw lz4 blob additionally requires the caller to know its uncompressed size, and cc::decompress_into is the only way to read one.
enum class cc::compression_framing : cc::u8
{
    frame = 0,
    raw = 1,
};

struct cc::compression_config
{
    compression_algorithm algorithm = compression_algorithm::zstd;

    /// The algorithm's OWN level scale, never a normalized one.
    /// 0 selects the algorithm's default, which is what both C APIs already mean by it.
    /// A level outside the algorithm's supported window is clamped rather than rejected.
    ///
    /// zstd: 1..22 upward, higher being smaller and slower, default 3.
    ///       Negative levels are the fast modes and run far past -22; the real floor is the build's own ZSTD_minCLevel.
    /// lz4:  positive selects the high-compression codec, up to 12, where 12 is far slower for a modest gain.
    ///       Negative is "acceleration", roughly N times faster to compress at a worse ratio.
    ///       `frame` and `raw` read this scale slightly differently — a frame starts high-compression at 2, not 1, and reads -N as acceleration N+1.
    int level = 0;

    compression_framing framing = compression_framing::frame;

    /// Must outlive every call using this config, and its algorithm must match the one above.
    /// Decompression must be given the same dictionary, or it fails.
    /// A mismatched algorithm is an ordinary error from cc::compress_into, but an assert from the cc::compressor constructor, which has no way to report it.
    compression_dictionary const* dictionary = nullptr;
};

struct cc::decompression_config
{
    /// nullopt sniffs the frame header for the algorithm.
    ///
    /// Set it explicitly for anything a format already recorded, which every format storing blobs should be doing.
    /// Sniffing reads a frame magic, so it cannot work on `raw` output and is not guaranteed to keep working as algorithms are added.
    cc::optional<compression_algorithm> algorithm = {};

    /// Must match how the data was produced.
    /// `raw` additionally requires an explicit `algorithm`, there being no header to read one from.
    compression_framing framing = compression_framing::frame;

    /// Must outlive the call, and must be the same dictionary the data was compressed with.
    /// Its algorithm is checked against `algorithm` above only when that is set; under sniffing a mismatch surfaces as a decode failure instead.
    compression_dictionary const* dictionary = nullptr;

    /// Refuse to produce more than this many bytes; negative means no limit.
    ///
    /// UNTRUSTED INPUT MUST SET THIS.
    /// A hostile frame declares a gigantic uncompressed size out of a few bytes, and without a cap the allocation that follows is the attack.
    /// The default is no limit because truncating a legitimate large payload at a number nobody chose is the worse failure.
    isize max_output_size = -1;
};

namespace cc
{
// --- one-shot ---------------------------------------------------------------------------------------------

/// Compress `data` into a fresh buffer.
/// Empty input is valid.
///
/// This form has no failure channel, so what would be an error elsewhere is a PRECONDITION here, and violating one asserts.
/// The dictionary must match the algorithm, and with lz4 `raw` framing `data` must be under lz4's ~2 GB block limit.
/// cc::compress_into is the fallible form.
[[nodiscard]] cc::vector<byte> compress(cc::span<byte const> data, compression_config cfg = {});

/// Compress into caller-provided storage, returning the number of bytes written.
/// `out` must hold at least compress_bound(data.size(), cfg) bytes; too small is an ordinary error, not an assert.
[[nodiscard]] cc::result<isize> compress_into(cc::span<byte const> data, cc::span<byte> out, compression_config cfg = {});

/// The largest output `size` bytes can compress to: the worst case, where the data is incompressible and the codec falls back to storing it.
/// Never an estimate, so a buffer this large always suffices.
/// `size` must be under lz4's ~2 GB block limit when using lz4 with `raw` framing, which is the one input no bound can be given for.
[[nodiscard]] isize compress_bound(isize size, compression_config cfg = {});

/// Decompress `data` into a fresh buffer.
///
/// Fails on corrupt or truncated input, on a dictionary that does not match the one used to compress, and on output exceeding cfg.max_output_size.
[[nodiscard]] cc::result<cc::vector<byte>> decompress(cc::span<byte const> data, decompression_config cfg = {});

/// Decompress into caller-provided storage, returning the number of bytes written.
/// Fails rather than truncating when the content does not fit in `out`.
/// It is also the only form that can read an lz4 `raw` blob, which carries no length at all.
[[nodiscard]] cc::result<isize> decompress_into(cc::span<byte const> data,
                                                cc::span<byte> out,
                                                decompression_config cfg = {});

// --- inspection -------------------------------------------------------------------------------------------

/// Which algorithm produced `data`, read from its frame magic.
/// nullopt when nothing matches, which includes every `raw`-framed blob.
[[nodiscard]] cc::optional<compression_algorithm> detect_algorithm(cc::span<byte const> data);

/// The uncompressed size the frame declares, without decompressing it.
///
/// nullopt for `raw` framing, which declares nothing, and when the algorithm can be neither read from the config nor sniffed.
/// Also nullopt for an lz4 frame whose content is empty, because lz4 spells "no declared size" and "zero bytes" the same way; zstd reports 0 for that same payload.
[[nodiscard]] cc::optional<isize> decompressed_size(cc::span<byte const> data, decompression_config cfg = {});
} // namespace cc

// --- reusable contexts ------------------------------------------------------------------------------------

/// A compressor that keeps its backend context between calls.
///
/// The free functions above build one of these per call.
/// That is the right trade for a handful of large payloads and the wrong one for thousands of small ones, where the per-call cost is an allocation plus, with a dictionary, re-preparing it every time.
/// Holding one across the loop removes both.
///
/// NOT thread-safe — one per thread.
/// Results are identical to the free functions either way.
struct cc::compressor
{
    explicit compressor(compression_config cfg);
    ~compressor();

    compressor(compressor&& rhs) noexcept;
    compressor& operator=(compressor&& rhs) noexcept;

    [[nodiscard]] cc::vector<byte> compress(cc::span<byte const> data);
    [[nodiscard]] cc::result<isize> compress_into(cc::span<byte const> data, cc::span<byte> out);

    [[nodiscard]] compression_config const& config() const { return _config; }

private:
    compression_config _config;

    /// The backend's own context, plus the prepared dictionary where the algorithm has one.
    /// Untyped because the type is upstream's (ZSTD_CCtx, LZ4F_cctx) and those headers stay inside the backend TUs.
    /// _config.algorithm is what says which destructor applies.
    void* _state = nullptr;
};

/// The decompressing counterpart.
/// Same reuse argument, same threading rule.
///
/// Its config's `algorithm` is resolved per call when left nullopt, so one decompressor still reads whatever it is handed.
/// That mode keeps no context between calls at all, though — leaving the algorithm open trades the reuse this type exists for against not having to know it.
struct cc::decompressor
{
    explicit decompressor(decompression_config cfg);
    ~decompressor();

    decompressor(decompressor&& rhs) noexcept;
    decompressor& operator=(decompressor&& rhs) noexcept;

    [[nodiscard]] cc::result<cc::vector<byte>> decompress(cc::span<byte const> data);
    [[nodiscard]] cc::result<isize> decompress_into(cc::span<byte const> data, cc::span<byte> out);

    [[nodiscard]] decompression_config const& config() const { return _config; }

private:
    decompression_config _config;
    compression_algorithm _state_algorithm = {};
    void* _state = nullptr;
};
