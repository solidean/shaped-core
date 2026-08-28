#include "compression_backend.hh"

#include <clean-core/bytes/compression_dictionary.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // cc::min, cc::max
#include <clean-core/string/format.hh>
#include <zlib.h>

using namespace cc::primitive_defines;

namespace
{
/// zlib's avail_in / avail_out are uInt, so a span larger than that has to be fed in pieces.
/// 1 GiB keeps every intermediate comfortably inside 32 bits, uInt being 32-bit on every target we build.
constexpr isize k_chunk = isize(1) << 30;

/// gzip's magic, then the compression method, which must be 8 (deflate) for anything we can read.
constexpr isize k_gzip_magic_size = 3;

/// The smallest possible gzip member: a 10-byte header, an empty deflate stream, and the 8-byte CRC-32 + ISIZE trailer.
constexpr isize k_gzip_min_size = 18;

/// The most DEFLATE can expand: a 258-byte match encoded in two bits, which is the format's own ceiling.
/// It is what makes a declared size above it provably a lie rather than merely a large number.
constexpr isize k_max_expansion = 1032;

/// deflate holds unflushed output in its pending buffer between calls and flushes it at the top of the next deflate(),
/// so one stream_compress can emit a previous call's pending bytes on top of its own and the bound must cover both.
/// pending_buf_size is lit_bufsize * 4, with lit_bufsize = 1 << (memLevel + 6) — 64 KiB at the default memLevel 8 we init with.
constexpr isize k_pending_bytes = 4 * (isize(1) << (8 + 6));

/// zlib's z_stream declares next_in mutable unless the consumer defines ZLIB_CONST, which would leave zlib's own TUs
/// declaring a different member type than this one.
/// The cast is safe: deflate() and inflate() only ever read through next_in.
[[nodiscard]] Bytef* as_next_in(byte const* p)
{
    return const_cast<Bytef*>(reinterpret_cast<Bytef const*>(p));
}

[[nodiscard]] Bytef* as_next_out(byte* p)
{
    return reinterpret_cast<Bytef*>(p);
}

/// The three framings are one windowBits argument, which is why deflate needs no per-wrapper code path.
///
/// Decoding is deliberately strict rather than using zlib's 15 + 32 auto-detect: framing states how the data was
/// produced, and a mismatch should be loud rather than quietly accepted.
[[nodiscard]] int window_bits_for(cc::compression_framing framing)
{
    switch (framing)
    {
    case cc::compression_framing::frame:
        return 15 + 16; // gzip
    case cc::compression_framing::zlib:
        return 15; // the zlib wrapper
    case cc::compression_framing::raw:
        return -15; // raw deflate, no wrapper at all
    }

    CC_UNREACHABLE("unknown compression framing");
}

/// cc's level scale onto zlib's 0..9.
///
/// 0 means "the algorithm's default" here, while zlib spells its own default -1 and reads 0 as "store, do not compress".
/// So zlib's level 0 is not reachable through this scale, which costs nothing: deflate already falls back to stored
/// blocks per block when the data does not compress.
[[nodiscard]] int zlib_level(int level)
{
    if (level == 0)
        return Z_DEFAULT_COMPRESSION;
    if (level < 0)
        return 1;

    return int(cc::min(isize(level), isize(9)));
}

[[nodiscard]] cc::string zlib_message(z_stream const& strm, int ret)
{
    if (strm.msg != nullptr)
        return cc::format("{} ({})", strm.msg, zError(ret));

    return cc::format("{}", zError(ret));
}

/// A dictionary is not expressible under gzip framing: deflateSetDictionary refuses a gzip-wrapped stream outright,
/// and the gzip header has no field to record which dictionary applies.
/// zlib framing records its Adler-32 as DICTID, and raw framing carries it out of band.
[[nodiscard]] cc::result<cc::unit> check_dictionary_framing(cc::compression_dictionary const* dict,
                                                            cc::compression_framing framing)
{
    if (dict != nullptr && !dict->is_empty() && framing == cc::compression_framing::frame)
        return cc::error("deflate: gzip framing cannot carry a dictionary - use `zlib` or `raw` framing for one");

    return cc::unit{};
}

[[nodiscard]] cc::span<byte const> dictionary_bytes(cc::compression_dictionary const* dict)
{
    return dict != nullptr ? dict->bytes() : cc::span<byte const>();
}

/// deflateSetDictionary takes a uInt length, and only the last window's worth ever reaches the compressor anyway.
/// So an oversized dictionary is truncated to its tail rather than refused, which is what zlib does internally too.
[[nodiscard]] cc::span<byte const> dictionary_tail(cc::span<byte const> dict)
{
    return dict.size() <= k_chunk ? dict : dict.subspan({.offset = dict.size() - k_chunk, .size = k_chunk});
}

/// adler32 over a span of any size, chunked past uInt.
/// This is exactly what a zlib header stores as DICTID, which is why it doubles as the dictionary id.
[[nodiscard]] u32 adler_of(cc::span<byte const> data)
{
    auto sum = adler32(0uL, Z_NULL, 0);

    auto offset = isize(0);
    while (offset < data.size())
    {
        auto const n = cc::min(data.size() - offset, k_chunk);
        sum = adler32(sum, reinterpret_cast<Bytef const*>(data.data() + offset), uInt(n));
        offset += n;
    }

    return u32(sum);
}

// --- compression ------------------------------------------------------------------------------------------

struct compressor_state
{
    z_stream strm = {};

    /// What deflateInit2 was actually called with.
    /// compress_into is handed a config per call, so one that no longer matches has to re-init rather than reset.
    int window_bits = 0;
    int level = 0;

    bool initialized = false;
};

[[nodiscard]] cc::result<cc::unit> ensure_deflate_init(compressor_state* s, cc::compression_config const& cfg)
{
    auto const bits = window_bits_for(cfg.framing);
    auto const level = zlib_level(cfg.level);

    if (s->initialized && s->window_bits == bits && s->level == level)
    {
        if (deflateReset(&s->strm) != Z_OK)
            return cc::error("deflate: failed to reset the compression context");

        return cc::unit{};
    }

    if (s->initialized)
        deflateEnd(&s->strm);

    s->strm = z_stream{};
    s->initialized = false;

    // memLevel 8 and Z_DEFAULT_STRATEGY are zlib's own defaults, and compress_bound's formula assumes both.
    auto const ret = deflateInit2(&s->strm, level, Z_DEFLATED, bits, 8, Z_DEFAULT_STRATEGY);
    if (ret != Z_OK)
        return cc::error(cc::format("deflate: failed to create the compression context: {}", zError(ret)));

    s->window_bits = bits;
    s->level = level;
    s->initialized = true;
    return cc::unit{};
}

[[nodiscard]] void* create_compressor(cc::compression_config const& cfg)
{
    auto* const state = new compressor_state();

    // create_compressor has no error channel, so a failure here leaves the state uninitialized and compress_into
    // retries the init to report why.
    (void)ensure_deflate_init(state, cfg);

    return state;
}

void destroy_compressor(void* state)
{
    auto* const s = static_cast<compressor_state*>(state);
    if (s == nullptr)
        return;

    if (s->initialized)
        deflateEnd(&s->strm);
    delete s;
}

[[nodiscard]] isize compress_bound(isize size, cc::compression_config const&)
{
    // Upstream's compressBound formula, evaluated in isize because uLong is 32-bit on Windows and this has to hold past 4 GB.
    // It is the stored-block worst case: 5 bytes of block header per 16383 bytes of input, plus slack for the wrapper.
    auto const bound = size + (size >> 12) + (size >> 14) + (size >> 25) + 13;

    // That slack covers zlib's 6-byte wrapper.
    // gzip's header and trailer come to 18, so the difference is added for every framing rather than branching on one.
    return bound + 18;
}

[[nodiscard]] cc::result<isize> compress_into(void* state,
                                              cc::compression_config const& cfg,
                                              cc::span<byte const> data,
                                              cc::span<byte> out)
{
    auto* const s = static_cast<compressor_state*>(state);
    if (s == nullptr)
        return cc::error("deflate: failed to create the compression context");

    CC_RETURN_IF_ERROR(check_dictionary_framing(cfg.dictionary, cfg.framing));
    CC_RETURN_IF_ERROR(ensure_deflate_init(s, cfg));

    auto const dict = dictionary_bytes(cfg.dictionary);
    if (!dict.empty())
    {
        auto const tail = dictionary_tail(dict);
        auto const ret = deflateSetDictionary(&s->strm, reinterpret_cast<Bytef const*>(tail.data()), uInt(tail.size()));
        if (ret != Z_OK)
            return cc::error(cc::format("deflate: failed to set the dictionary: {}", zlib_message(s->strm, ret)));
    }

    s->strm.next_in = as_next_in(data.data());
    s->strm.next_out = as_next_out(out.data());

    auto consumed = isize(0);
    auto produced = isize(0);

    while (true)
    {
        auto const in_left = data.size() - consumed;
        auto const out_left = out.size() - produced;

        if (out_left == 0)
            return cc::error("deflate compression failed: the output buffer is too small");

        s->strm.avail_in = uInt(cc::min(in_left, k_chunk));
        s->strm.avail_out = uInt(cc::min(out_left, k_chunk));

        auto const in_before = s->strm.avail_in;
        auto const out_before = s->strm.avail_out;

        // Z_FINISH only once the whole input is in the window; a chunk boundary must not seal the stream early.
        // Chunking with Z_NO_FLUSH introduces no flush points, which is what keeps compress_bound's figure valid.
        auto const last = isize(in_before) == in_left;
        auto const ret = deflate(&s->strm, last ? Z_FINISH : Z_NO_FLUSH);

        consumed += isize(in_before - s->strm.avail_in);
        produced += isize(out_before - s->strm.avail_out);

        if (ret == Z_STREAM_END)
            return produced;

        if (ret != Z_OK && ret != Z_BUF_ERROR)
            return cc::error(cc::format("deflate compression failed: {}", zlib_message(s->strm, ret)));

        // Neither side moved, so the loop cannot make progress; with input still left that means the output is too small.
        if (in_before == s->strm.avail_in && out_before == s->strm.avail_out)
            return cc::error("deflate compression failed: the output buffer is too small");
    }
}

// --- decompression ----------------------------------------------------------------------------------------

struct decompressor_state
{
    z_stream strm = {};
    int window_bits = 0;
    bool initialized = false;

    /// Whether the stream is gzip-framed, which is the only framing whose members concatenate.
    /// The streaming path has no config to consult, so this records what the context was built for.
    bool gzip = false;
};

[[nodiscard]] cc::result<cc::unit> ensure_inflate_init(decompressor_state* s, cc::decompression_config const& cfg)
{
    auto const bits = window_bits_for(cfg.framing);

    if (s->initialized && s->window_bits == bits)
    {
        if (inflateReset(&s->strm) != Z_OK)
            return cc::error("deflate: failed to reset the decompression context");

        s->gzip = cfg.framing == cc::compression_framing::frame;
        return cc::unit{};
    }

    if (s->initialized)
        inflateEnd(&s->strm);

    s->strm = z_stream{};
    s->initialized = false;

    auto const ret = inflateInit2(&s->strm, bits);
    if (ret != Z_OK)
        return cc::error(cc::format("deflate: failed to create the decompression context: {}", zError(ret)));

    s->window_bits = bits;
    s->gzip = cfg.framing == cc::compression_framing::frame;
    s->initialized = true;
    return cc::unit{};
}

[[nodiscard]] void* create_decompressor(cc::decompression_config const& cfg)
{
    auto* const state = new decompressor_state();

    (void)ensure_inflate_init(state, cfg);

    return state;
}

void destroy_decompressor(void* state)
{
    auto* const s = static_cast<decompressor_state*>(state);
    if (s == nullptr)
        return;

    if (s->initialized)
        inflateEnd(&s->strm);
    delete s;
}

/// Whether `rest` opens with as much of gzip's magic as it holds, for a caller that may be looking at a partial window.
[[nodiscard]] bool starts_gzip_member_prefix(bool gzip, cc::span<byte const> rest)
{
    if (!gzip || rest.empty())
        return false;

    constexpr u8 magic[k_gzip_magic_size] = {0x1f, 0x8b, 0x08};
    for (isize i = 0; i < cc::min(rest.size(), k_gzip_magic_size); ++i)
        if (u8(rest[i]) != magic[i])
            return false;

    return true;
}

/// Whether `data` opens a gzip member.
///
/// Only gzip is sniffable.
/// The zlib wrapper opens with a checksum constraint rather than a magic, so accepting it here would claim payload
/// bytes as deflate roughly once in every 31 blobs.
[[nodiscard]] bool matches_magic(cc::span<byte const> data)
{
    return data.size() >= k_gzip_magic_size && starts_gzip_member_prefix(/*gzip*/ true, data);
}

[[nodiscard]] cc::optional<isize> declared_size(cc::span<byte const> data)
{
    // gzip is the only framing that declares anything, and it declares it in the trailer rather than the header.
    if (!matches_magic(data) || data.size() < k_gzip_min_size)
        return {};

    // ISIZE: the uncompressed length modulo 2^32, little-endian, in the last four bytes.
    // So it is a HINT rather than a fact — it wraps past 4 GB, and on a multi-member stream it describes only the last member.
    // Everything here treats it as a seed for one allocation and never as a bound.
    auto const n = data.size();
    auto const size = u32(u8(data[n - 4])) | (u32(u8(data[n - 3])) << 8) | (u32(u8(data[n - 2])) << 16)
                    | (u32(u8(data[n - 1])) << 24);

    return isize(size);
}

/// Whether `rest` opens another gzip member, which is the one case a finished inflate stream continues into.
/// Only gzip framing concatenates: a zlib or raw stream has no magic to recognize a successor by, so trailing bytes
/// there are not ours to interpret.
[[nodiscard]] bool starts_gzip_member(cc::compression_framing framing, cc::span<byte const> rest)
{
    return framing == cc::compression_framing::frame && rest.size() >= k_gzip_min_size && matches_magic(rest);
}

/// Run `data` through inflate until the stream ends, taking each output window from `sink`.
///
/// The sink is asked for a window per pass, which is what lets one loop serve both a fixed caller buffer and a vector
/// that grows as it goes.
template <class Sink>
[[nodiscard]] cc::result<isize> run_inflate(decompressor_state* s,
                                            cc::decompression_config const& cfg,
                                            cc::span<byte const> data,
                                            Sink&& sink)
{
    CC_RETURN_IF_ERROR(ensure_inflate_init(s, cfg));

    auto const dict = dictionary_bytes(cfg.dictionary);

    // Raw inflate has no header in which to ask for a dictionary, so it has to be loaded up front.
    // The zlib wrapper asks instead, which is the Z_NEED_DICT branch below.
    if (!dict.empty() && cfg.framing == cc::compression_framing::raw)
    {
        auto const tail = dictionary_tail(dict);
        auto const ret = inflateSetDictionary(&s->strm, reinterpret_cast<Bytef const*>(tail.data()), uInt(tail.size()));
        if (ret != Z_OK)
            return cc::error(cc::format("deflate: failed to set the dictionary: {}", zlib_message(s->strm, ret)));
    }

    auto consumed = isize(0);
    auto produced = isize(0);

    while (true)
    {
        auto window = sink(produced);
        if (window.has_error())
            return cc::error(cc::move(window).error());

        auto const out_span = window.value();

        s->strm.next_in = as_next_in(data.data() + consumed);
        s->strm.avail_in = uInt(cc::min(data.size() - consumed, k_chunk));
        s->strm.next_out = as_next_out(out_span.data());
        s->strm.avail_out = uInt(cc::min(out_span.size(), k_chunk));

        auto const in_before = s->strm.avail_in;
        auto const out_before = s->strm.avail_out;

        auto const ret = inflate(&s->strm, Z_NO_FLUSH);

        consumed += isize(in_before - s->strm.avail_in);
        produced += isize(out_before - s->strm.avail_out);

        if (ret == Z_STREAM_END)
        {
            // A .gz file is a SEQUENCE of members - `cat a.gz b.gz`, pigz and bgzip all write several - and inflate
            // stops at the end of one without crossing into the next (zlib.h says so, and gzread is the layer that
            // does it, which we do not vendor).
            // So the stream ends here only once the input is spent or what follows is not another member; anything
            // else would decode a legal file down to its first member and report success.
            auto const rest = data.subspan({.offset = consumed, .size = data.size() - consumed});
            if (!starts_gzip_member(cfg.framing, rest))
                return produced;

            auto const reset = inflateReset(&s->strm);
            if (reset != Z_OK)
                return cc::error(
                    cc::format("deflate: failed to reset between gzip members: {}", zlib_message(s->strm, reset)));
            continue;
        }

        if (ret == Z_NEED_DICT)
        {
            if (dict.empty())
                return cc::error("deflate decompression failed: the stream needs the dictionary it was compressed "
                                 "with");

            auto const tail = dictionary_tail(dict);
            auto const set
                = inflateSetDictionary(&s->strm, reinterpret_cast<Bytef const*>(tail.data()), uInt(tail.size()));
            if (set != Z_OK)
                return cc::error(cc::format("deflate: the dictionary does not match the one the stream was compressed "
                                            "with: {}",
                                            zlib_message(s->strm, set)));
            continue;
        }

        if (ret != Z_OK && ret != Z_BUF_ERROR)
            return cc::error(cc::format("deflate decompression failed: {}", zlib_message(s->strm, ret)));

        // Neither side moved, so the loop cannot make progress.
        // Two causes, indistinguishable from here: the input ran out mid-stream, or the sink has no room for the rest.
        if (in_before == s->strm.avail_in && out_before == s->strm.avail_out)
            return cc::error("deflate: the stream is truncated, or the output buffer is too small for it");
    }
}

[[nodiscard]] cc::result<isize> decompress_into(void* state,
                                                cc::decompression_config const& cfg,
                                                cc::span<byte const> data,
                                                cc::span<byte> out)
{
    auto* const s = static_cast<decompressor_state*>(state);
    if (s == nullptr)
        return cc::error("deflate: failed to create the decompression context");

    auto const limit = cfg.max_output_size >= 0 ? cc::min(out.size(), cfg.max_output_size) : out.size();

    return run_inflate(s, cfg, data,
                       [&](isize produced) -> cc::result<cc::span<byte>>
                       {
                           // An empty window is allowed rather than an error: inflate may still have header bytes to
                           // consume, and run_inflate's no-progress check is what catches "this does not fit".
                           return out.subspan({.offset = produced, .size = limit - produced});
                       });
}

[[nodiscard]] cc::result<cc::vector<byte>> decompress_to_vector(void* state,
                                                                cc::decompression_config const& cfg,
                                                                cc::span<byte const> data)
{
    auto* const s = static_cast<decompressor_state*>(state);
    if (s == nullptr)
        return cc::error("deflate: failed to create the decompression context");

    // gzip's ISIZE only ever seeds the first allocation: it wraps past 4 GB and a hostile stream can put any value
    // there, so the growth loop below stays in charge and cfg.max_output_size stays the only cap.
    auto const hint = declared_size(data);
    if (cfg.max_output_size >= 0 && hint.has_value() && hint.value() > cfg.max_output_size)
        return cc::error(cc::format("deflate: the stream declares {} bytes, over the {} byte limit", hint.value(),
                                    cfg.max_output_size));

    // The floor keeps a tiny seed from turning the growth loop into a per-byte realloc, but it must never lift the
    // capacity back over a limit the caller set — max_output_size = 0 has to mean zero.
    auto capacity = cc::max(isize(64), hint.value_or(cc::max(isize(4096), data.size() * 4)));

    // A hint above what DEFLATE could possibly expand this input to is provably wrong, and four bytes of trailing
    // garbage read as ISIZE ask for gigabytes.
    // Without a max_output_size that is an allocation any caller can be handed, and a 32-bit target refuses it outright.
    capacity = cc::min(capacity, cc::max(isize(4096), data.size() * k_max_expansion));

    if (cfg.max_output_size >= 0)
        capacity = cc::min(capacity, cfg.max_output_size);

    auto result = cc::vector<byte>::create_uninitialized(capacity);

    auto written = run_inflate(
        s, cfg, data,
        [&](isize produced) -> cc::result<cc::span<byte>>
        {
            if (produced < result.size())
                return cc::span<byte>(result.data() + produced, result.size() - produced);

            auto next = result.size() * 2;
            if (cfg.max_output_size >= 0)
                next = cc::min(next, cfg.max_output_size);
            if (next <= result.size())
                return cc::error(cc::format("deflate: output exceeds the {} byte limit", cfg.max_output_size));

            result.resize_to_uninitialized(next);
            return cc::span<byte>(result.data() + produced, result.size() - produced);
        });
    CC_RETURN_IF_ERROR(written);

    result.resize_down_to(written.value());
    return result;
}

// --- dictionaries -----------------------------------------------------------------------------------------

[[nodiscard]] cc::result<cc::vector<byte>> train_dictionary(cc::span<cc::span<byte const> const>, isize)
{
    return cc::error("zlib ships no dictionary trainer - build one from representative bytes with from_bytes, or train "
                     "a zstd dictionary instead");
}

[[nodiscard]] u32 dictionary_id(cc::span<byte const> raw)
{
    if (raw.empty())
        return 0;

    return adler_of(raw);
}

// --- streaming --------------------------------------------------------------------------------------------

[[nodiscard]] void* create_stream_compressor(cc::compression_config const& cfg)
{
    if (check_dictionary_framing(cfg.dictionary, cfg.framing).has_error())
        return nullptr;

    auto* const state = new compressor_state();
    if (ensure_deflate_init(state, cfg).has_error())
    {
        delete state;
        return nullptr;
    }

    auto const dict = dictionary_bytes(cfg.dictionary);
    if (!dict.empty())
    {
        auto const tail = dictionary_tail(dict);
        if (deflateSetDictionary(&state->strm, reinterpret_cast<Bytef const*>(tail.data()), uInt(tail.size())) != Z_OK)
        {
            deflateEnd(&state->strm);
            delete state;
            return nullptr;
        }
    }

    return state;
}

[[nodiscard]] isize stream_compress_bound(void* state, isize in_size)
{
    auto* const s = static_cast<compressor_state*>(state);
    if (s == nullptr || !s->initialized)
        return isize(0);

    // deflateBound covers the wrapper and the worst case for `in_size` fresh bytes.
    // The pending allowance on top is what an earlier call left unflushed, which this call emits ahead of its own output.
    return isize(deflateBound(&s->strm, uLong(cc::min(in_size, k_chunk)))) + k_pending_bytes + 64;
}

[[nodiscard]] cc::result<isize> stream_compress(void* state, cc::span<byte const> in, cc::span<byte> out, bool finish)
{
    auto* const s = static_cast<compressor_state*>(state);
    if (s == nullptr || !s->initialized)
        return cc::error("deflate: failed to create the streaming compression context");

    s->strm.next_in = as_next_in(in.data());
    s->strm.next_out = as_next_out(out.data());

    auto consumed = isize(0);
    auto produced = isize(0);

    while (true)
    {
        auto const in_left = in.size() - consumed;
        auto const out_left = out.size() - produced;

        if (out_left == 0)
            return cc::error("deflate streaming compression failed: the output buffer is too small");

        s->strm.avail_in = uInt(cc::min(in_left, k_chunk));
        s->strm.avail_out = uInt(cc::min(out_left, k_chunk));

        auto const in_before = s->strm.avail_in;
        auto const out_before = s->strm.avail_out;

        // Z_FINISH seals the stream, and only once the caller says this is the last of the input.
        // Otherwise Z_NO_FLUSH, which keeps the ratio a flush point would cost.
        auto const last = finish && isize(in_before) == in_left;
        auto const ret = deflate(&s->strm, last ? Z_FINISH : Z_NO_FLUSH);

        consumed += isize(in_before - s->strm.avail_in);
        produced += isize(out_before - s->strm.avail_out);

        if (ret == Z_STREAM_END)
            return produced;

        if (ret != Z_OK && ret != Z_BUF_ERROR)
            return cc::error(cc::format("deflate streaming compression failed: {}", zlib_message(s->strm, ret)));

        // All the input is in and the stream is not being sealed, so this pass is done.
        if (!finish && consumed == in.size())
            return produced;

        if (in_before == s->strm.avail_in && out_before == s->strm.avail_out)
            return cc::error("deflate streaming compression failed: the output buffer is too small");
    }
}

[[nodiscard]] cc::result<cc::impl::stream_decompress_step> stream_decompress(void* state,
                                                                             cc::span<byte const> in,
                                                                             cc::span<byte> out)
{
    auto* const s = static_cast<decompressor_state*>(state);
    if (s == nullptr || !s->initialized)
        return cc::error("deflate: failed to create the streaming decompression context");

    s->strm.next_in = as_next_in(in.data());
    s->strm.avail_in = uInt(cc::min(in.size(), k_chunk));
    s->strm.next_out = as_next_out(out.data());
    s->strm.avail_out = uInt(cc::min(out.size(), k_chunk));

    auto const in_before = s->strm.avail_in;
    auto const out_before = s->strm.avail_out;

    auto const ret = inflate(&s->strm, Z_NO_FLUSH);

    // A decompressing stream is never handed a dictionary, so a stream asking for one cannot be answered here.
    if (ret == Z_NEED_DICT)
        return cc::error("deflate: the stream needs a dictionary, which a decompressing stream cannot be given");

    if (ret != Z_OK && ret != Z_BUF_ERROR && ret != Z_STREAM_END)
        return cc::error(cc::format("deflate streaming decompression failed: {}", zlib_message(s->strm, ret)));

    auto const consumed = isize(in_before - s->strm.avail_in);
    auto finished = ret == Z_STREAM_END;

    // As on the one-shot path: a member ending is not the file ending, so reset and keep going while another member
    // follows.
    // Only the bytes already in this window can be looked at, so a boundary that lands inside gzip's three-byte magic
    // ends the stream — which is why the check is on the prefix rather than on a whole member.
    if (finished && starts_gzip_member_prefix(s->gzip, in.subspan({.offset = consumed, .size = isize(s->strm.avail_in)})))
    {
        auto const reset = inflateReset(&s->strm);
        if (reset != Z_OK)
            return cc::error(
                cc::format("deflate: failed to reset between gzip members: {}", zlib_message(s->strm, reset)));

        finished = false;
    }

    return cc::impl::stream_decompress_step{.consumed = consumed,
                                            .produced = isize(out_before - s->strm.avail_out),
                                            .finished = finished};
}

constexpr cc::impl::compression_backend backend = {
    .compress_bound = &compress_bound,
    .create_compressor = &create_compressor,
    .destroy_compressor = &destroy_compressor,
    .compress_into = &compress_into,
    .create_decompressor = &create_decompressor,
    .destroy_decompressor = &destroy_decompressor,
    .decompress_into = &decompress_into,
    .decompress_to_vector = &decompress_to_vector,
    .declared_size = &declared_size,
    // gzip keeps ISIZE in the trailer, so only a whole blob can be asked - a prefix has payload bytes there.
    .declares_size_in_header = false,
    .matches_magic = &matches_magic,
    .train_dictionary = &train_dictionary,
    .dictionary_id = &dictionary_id,
    // A streaming compressor is the same z_stream, driven with Z_NO_FLUSH until finish, so it shares the destructor.
    .create_stream_compressor = &create_stream_compressor,
    .destroy_stream_compressor = &destroy_compressor,
    .stream_compress_bound = &stream_compress_bound,
    .stream_compress = &stream_compress,
    // Decompression needs no extra state beyond the z_stream the one-shot path already carries.
    .create_stream_decompressor = &create_decompressor,
    .destroy_stream_decompressor = &destroy_decompressor,
    .stream_decompress = &stream_decompress,
};
} // namespace

cc::impl::compression_backend const& cc::impl::deflate_backend()
{
    return backend;
}
