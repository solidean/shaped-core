#include "compression_backend.hh"

#include <clean-core/bytes/compression_dictionary.hh>
#include <clean-core/string/format.hh>

#define LZ4F_STATIC_LINKING_ONLY // LZ4F_CDict, the prepared dictionary a compressor holds
#include <lz4.h>
#include <lz4frame.h>
#include <lz4hc.h>

using namespace cc::primitive_defines;

namespace
{
constexpr isize lz4_magic_size = 4;

/// LZ4's block API is int-sized throughout, so anything larger has to be refused here rather than truncated there.
[[nodiscard]] bool fits_block_api(isize size)
{
    return size >= 0 && size <= isize(LZ4_MAX_INPUT_SIZE);
}

/// cc's level scale onto LZ4's two codecs.
/// Positive is the high-compression one, negative is "acceleration" on the fast one, and 0 is the fast one at its default.
/// The frame API takes the same scale directly.
[[nodiscard]] LZ4F_preferences_t frame_preferences(cc::compression_config const& cfg, isize content_size)
{
    auto prefs = LZ4F_preferences_t{};
    prefs.compressionLevel = cfg.level;
    prefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;

    // Declaring the size is what lets cc::decompressed_size answer for an lz4 frame, and cc::decompress size its buffer in one allocation.
    // It costs 8 bytes in the header, which `raw` framing is the way to opt out of.
    prefs.frameInfo.contentSize = u64(content_size);
    return prefs;
}

struct compressor_state
{
    LZ4F_cctx* cctx = nullptr;
    LZ4F_CDict* cdict = nullptr;

    /// The block codec's own contexts, built on first use.
    /// LZ4F_CDict serves the frame path only, so without these a dictionary-using `raw` compressor would allocate and
    /// free a stream per call — which is the exact workload a held compressor exists to make cheap.
    LZ4_stream_t* block_stream = nullptr;
    LZ4_streamHC_t* block_stream_hc = nullptr;
};

struct decompressor_state
{
    LZ4F_dctx* dctx = nullptr;
};

// --- compression ------------------------------------------------------------------------------------------

[[nodiscard]] void* create_compressor(cc::compression_config const& cfg)
{
    auto* const state = new compressor_state();

    if (LZ4F_isError(LZ4F_createCompressionContext(&state->cctx, LZ4F_VERSION)))
    {
        delete state;
        return nullptr;
    }

    if (cfg.dictionary != nullptr && !cfg.dictionary->is_empty())
    {
        auto const raw = cfg.dictionary->bytes();
        state->cdict = LZ4F_createCDict(raw.data(), size_t(raw.size()));
        if (state->cdict == nullptr)
        {
            LZ4F_freeCompressionContext(state->cctx);
            delete state;
            return nullptr;
        }
    }

    return state;
}

void destroy_compressor(void* state)
{
    auto* const s = static_cast<compressor_state*>(state);
    if (s == nullptr)
        return;

    if (s->cdict != nullptr)
        LZ4F_freeCDict(s->cdict);
    if (s->cctx != nullptr)
        LZ4F_freeCompressionContext(s->cctx);
    if (s->block_stream != nullptr)
        LZ4_freeStream(s->block_stream);
    if (s->block_stream_hc != nullptr)
        LZ4_freeStreamHC(s->block_stream_hc);
    delete s;
}

[[nodiscard]] isize compress_bound(isize size, cc::compression_config const& cfg)
{
    if (cfg.framing == cc::compression_framing::raw)
        return fits_block_api(size) ? isize(LZ4_compressBound(int(size))) : isize(0);

    auto const prefs = frame_preferences(cfg, size);
    return isize(LZ4F_compressFrameBound(size_t(size), &prefs));
}

/// The block codec, with the stream variants where a dictionary is in play — LZ4 has no single call that takes both
/// a dictionary and a compression level, so the four combinations are spelled out.
[[nodiscard]] cc::result<isize> compress_block(compressor_state* s,
                                               cc::compression_config const& cfg,
                                               cc::span<byte const> data,
                                               cc::span<byte> out)
{
    if (!fits_block_api(data.size()))
        return cc::error(
            cc::format("lz4: {} bytes is over the block API's {} byte limit", data.size(), isize(LZ4_MAX_INPUT_SIZE)));

    auto const* const src = reinterpret_cast<char const*>(data.data());
    auto* const dst = reinterpret_cast<char*>(out.data());
    auto const src_size = int(data.size());
    auto const dst_cap = int(cc::min(out.size(), isize(LZ4_MAX_INPUT_SIZE)));

    auto const dict = cfg.dictionary != nullptr ? cfg.dictionary->bytes() : cc::span<byte const>();
    auto written = 0;

    if (dict.empty())
    {
        if (cfg.level > 0)
            written = LZ4_compress_HC(src, dst, src_size, dst_cap, cfg.level);
        else
            written = LZ4_compress_fast(src, dst, src_size, dst_cap, cfg.level < 0 ? -cfg.level : 1);
    }
    else if (cfg.level > 0)
    {
        if (s->block_stream_hc == nullptr)
            s->block_stream_hc = LZ4_createStreamHC();
        if (s->block_stream_hc == nullptr)
            return cc::error("lz4: failed to create the high-compression block context");

        // Loading the dictionary is also what resets the stream, so each call starts from the dictionary and nothing
        // carries over from the previous one — these are independent blocks, not a continued stream.
        LZ4_resetStreamHC_fast(s->block_stream_hc, cfg.level);
        LZ4_loadDictHC(s->block_stream_hc, reinterpret_cast<char const*>(dict.data()), int(dict.size()));
        written = LZ4_compress_HC_continue(s->block_stream_hc, src, dst, src_size, dst_cap);
    }
    else
    {
        if (s->block_stream == nullptr)
            s->block_stream = LZ4_createStream();
        if (s->block_stream == nullptr)
            return cc::error("lz4: failed to create the block context");

        LZ4_loadDict(s->block_stream, reinterpret_cast<char const*>(dict.data()), int(dict.size()));
        written
            = LZ4_compress_fast_continue(s->block_stream, src, dst, src_size, dst_cap, cfg.level < 0 ? -cfg.level : 1);
    }

    if (written <= 0 && !data.empty())
        return cc::error("lz4 compression failed: the output buffer is too small");

    return isize(written);
}

[[nodiscard]] cc::result<isize> compress_into(void* state,
                                              cc::compression_config const& cfg,
                                              cc::span<byte const> data,
                                              cc::span<byte> out)
{
    auto* const s = static_cast<compressor_state*>(state);
    if (s == nullptr)
        return cc::error("lz4: failed to create the compression context");

    if (cfg.framing == cc::compression_framing::raw)
        return compress_block(s, cfg, data, out);

    if (s->cctx == nullptr)
        return cc::error("lz4: failed to create the compression context");

    auto const prefs = frame_preferences(cfg, data.size());
    auto const written = LZ4F_compressFrame_usingCDict(s->cctx, out.data(), size_t(out.size()), data.data(),
                                                       size_t(data.size()), s->cdict, &prefs);
    if (LZ4F_isError(written))
        return cc::error(cc::format("lz4 compression failed: {}", LZ4F_getErrorName(written)));

    return isize(written);
}

// --- decompression ----------------------------------------------------------------------------------------

[[nodiscard]] void* create_decompressor(cc::decompression_config const&)
{
    auto* const state = new decompressor_state();
    if (LZ4F_isError(LZ4F_createDecompressionContext(&state->dctx, LZ4F_VERSION)))
    {
        delete state;
        return nullptr;
    }
    return state;
}

void destroy_decompressor(void* state)
{
    auto* const s = static_cast<decompressor_state*>(state);
    if (s == nullptr)
        return;

    if (s->dctx != nullptr)
        LZ4F_freeDecompressionContext(s->dctx);
    delete s;
}

[[nodiscard]] cc::optional<isize> declared_size(cc::span<byte const> data)
{
    LZ4F_dctx* dctx = nullptr;
    if (LZ4F_isError(LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION)))
        return {};

    auto info = LZ4F_frameInfo_t{};
    auto consumed = size_t(data.size());
    auto const status = LZ4F_getFrameInfo(dctx, &info, data.data(), &consumed);
    LZ4F_freeDecompressionContext(dctx);

    // contentSize is 0 both when the frame omits it and when the content really is empty; the two are
    // indistinguishable here, and treating both as "unknown" costs only a streaming pass over nothing.
    if (LZ4F_isError(status) || info.contentSize == 0)
        return {};

    return isize(info.contentSize);
}

[[nodiscard]] bool matches_magic(cc::span<byte const> data)
{
    if (data.size() < lz4_magic_size)
        return false;

    auto const magic = u32(u8(data[0])) | (u32(u8(data[1])) << 8) | (u32(u8(data[2])) << 16) | (u32(u8(data[3])) << 24);
    return magic == LZ4F_MAGICNUMBER;
}

[[nodiscard]] cc::result<isize> decompress_block(cc::decompression_config const& cfg,
                                                 cc::span<byte const> data,
                                                 cc::span<byte> out)
{
    if (cfg.max_output_size >= 0 && out.size() > cfg.max_output_size)
        out = out.first_n(cfg.max_output_size);

    if (data.empty())
        return isize(0);

    if (!fits_block_api(data.size()) || !fits_block_api(out.size()))
        return cc::error("lz4: block decompression is limited to 2 GB per call");

    auto const* const src = reinterpret_cast<char const*>(data.data());
    auto* const dst = reinterpret_cast<char*>(out.data());
    auto const dict = cfg.dictionary != nullptr ? cfg.dictionary->bytes() : cc::span<byte const>();

    auto const written = dict.empty()
                           ? LZ4_decompress_safe(src, dst, int(data.size()), int(out.size()))
                           : LZ4_decompress_safe_usingDict(src, dst, int(data.size()), int(out.size()),
                                                           reinterpret_cast<char const*>(dict.data()), int(dict.size()));
    if (written < 0)
        return cc::error("lz4 decompression failed: malformed block, or the output buffer is too small");

    return isize(written);
}

/// Run a frame through LZ4F_decompress until it ends, handing each output window to `sink`.
///
/// LZ4F_decompress reports how much input it consumed and how much output it produced per call and must be driven to
/// a zero return, which is the only signal that the frame is complete rather than merely out of buffer.
///
/// The dictionary has to be handed to every call rather than loaded into the context once: LZ4F has no decompression
/// counterpart to LZ4F_CDict, so the plain entry point silently ignores a dictionary instead of failing.
template <class Sink>
[[nodiscard]] cc::result<isize> run_frame(LZ4F_dctx* dctx, cc::span<byte const> data, cc::span<byte const> dict, Sink&& sink)
{
    LZ4F_resetDecompressionContext(dctx);

    auto consumed = isize(0);
    auto produced = isize(0);

    while (true)
    {
        auto window = sink(produced);
        if (window.has_error())
            return cc::error(cc::move(window).error());

        auto out_span = window.value();
        auto out_size = size_t(out_span.size());
        auto in_size = size_t(data.size() - consumed);

        auto const status
            = dict.empty() ? LZ4F_decompress(dctx, out_span.data(), &out_size, data.data() + consumed, &in_size, nullptr)
                           : LZ4F_decompress_usingDict(dctx, out_span.data(), &out_size, data.data() + consumed,
                                                       &in_size, dict.data(), size_t(dict.size()), nullptr);
        if (LZ4F_isError(status))
            return cc::error(cc::format("lz4 decompression failed: {}", LZ4F_getErrorName(status)));

        consumed += isize(in_size);
        produced += isize(out_size);

        if (status == 0) // the frame is complete
            return produced;

        // Neither side moved, so the loop cannot make progress.
        // Two causes, and from here they are indistinguishable: the input ran out mid-frame, or the sink has no room left to write the rest into.
        if (in_size == 0 && out_size == 0)
            return cc::error("lz4: the frame is truncated, or the output buffer is too small for it");
    }
}

[[nodiscard]] cc::result<isize> decompress_into(void* state,
                                                cc::decompression_config const& cfg,
                                                cc::span<byte const> data,
                                                cc::span<byte> out)
{
    if (cfg.framing == cc::compression_framing::raw)
        return decompress_block(cfg, data, out);

    auto* const s = static_cast<decompressor_state*>(state);
    if (s == nullptr || s->dctx == nullptr)
        return cc::error("lz4: failed to create the decompression context");

    auto const limit = cfg.max_output_size >= 0 ? cc::min(out.size(), cfg.max_output_size) : out.size();
    auto const dict = cfg.dictionary != nullptr ? cfg.dictionary->bytes() : cc::span<byte const>();

    return run_frame(s->dctx, data, dict,
                     [&](isize produced) -> cc::result<cc::span<byte>>
                     {
                         // An empty window is allowed rather than an error: LZ4F still has header bytes to consume at
                         // that point, and run_frame's no-progress check is what actually catches "this does not fit".
                         return out.subspan({.offset = produced, .size = limit - produced});
                     });
}

[[nodiscard]] cc::result<cc::vector<byte>> decompress_to_vector(void* state,
                                                                cc::decompression_config const& cfg,
                                                                cc::span<byte const> data)
{
    // A raw lz4 blob is a bare block: no length, no header, no terminator.
    // So there is nothing here to decompress *into* without being told how big the result is, and decompress_into is the only way to read one.
    if (cfg.framing == cc::compression_framing::raw)
        return cc::error("lz4: raw framing carries no size, so it can only be read through cc::decompress_into");

    auto* const s = static_cast<decompressor_state*>(state);
    if (s == nullptr || s->dctx == nullptr)
        return cc::error("lz4: failed to create the decompression context");

    auto const hint = declared_size(data);
    if (cfg.max_output_size >= 0 && hint.has_value() && hint.value() > cfg.max_output_size)
        return cc::error(
            cc::format("lz4: frame declares {} bytes, over the {} byte limit", hint.value(), cfg.max_output_size));

    // The floor keeps a tiny seed from turning the growth loop into a per-byte realloc, but it must never lift the
    // capacity back over a limit the caller set — max_output_size = 0 has to mean zero.
    auto capacity = cc::max(isize(64), hint.value_or(cc::max(isize(4096), data.size() * 4)));
    if (cfg.max_output_size >= 0)
        capacity = cc::min(capacity, cfg.max_output_size);

    auto result = cc::vector<byte>::create_uninitialized(capacity);

    auto const dict = cfg.dictionary != nullptr ? cfg.dictionary->bytes() : cc::span<byte const>();

    auto written
        = run_frame(s->dctx, data, dict,
                    [&](isize produced) -> cc::result<cc::span<byte>>
                    {
                        if (produced < result.size())
                            return cc::span<byte>(result.data() + produced, result.size() - produced);

                        auto next = result.size() * 2;
                        if (cfg.max_output_size >= 0)
                            next = cc::min(next, cfg.max_output_size);
                        if (next <= result.size())
                            return cc::error(cc::format("lz4: output exceeds the {} byte limit", cfg.max_output_size));

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
    return cc::error("lz4 ships no dictionary trainer - build one from representative bytes with from_bytes, or train "
                     "a zstd dictionary instead");
}

[[nodiscard]] u32 dictionary_id(cc::span<byte const>)
{
    return 0;
}

// --- streaming --------------------------------------------------------------------------------------------

/// A streaming compressor is a frame in progress, so it carries the header state the one-shot path does not have.
struct stream_compressor_state
{
    LZ4F_cctx* cctx = nullptr;
    LZ4F_CDict* cdict = nullptr;
    LZ4F_preferences_t prefs = {};
    bool begun = false;
};

[[nodiscard]] void* create_stream_compressor(cc::compression_config const& cfg)
{
    auto* const state = new stream_compressor_state();

    if (LZ4F_isError(LZ4F_createCompressionContext(&state->cctx, LZ4F_VERSION)))
    {
        delete state;
        return nullptr;
    }

    // A streaming frame does not know its total length, so contentSize stays 0 — which is why cc::decompressed_size
    // reports nothing for a streamed lz4 frame, unlike a one-shot one.
    state->prefs = frame_preferences(cfg, 0);
    state->prefs.frameInfo.contentSize = 0;

    if (cfg.dictionary != nullptr && !cfg.dictionary->is_empty())
    {
        auto const raw = cfg.dictionary->bytes();
        state->cdict = LZ4F_createCDict(raw.data(), size_t(raw.size()));
        if (state->cdict == nullptr)
        {
            LZ4F_freeCompressionContext(state->cctx);
            delete state;
            return nullptr;
        }
    }

    return state;
}

void destroy_stream_compressor(void* state)
{
    auto* const s = static_cast<stream_compressor_state*>(state);
    if (s == nullptr)
        return;

    if (s->cdict != nullptr)
        LZ4F_freeCDict(s->cdict);
    if (s->cctx != nullptr)
        LZ4F_freeCompressionContext(s->cctx);
    delete s;
}

[[nodiscard]] isize stream_compress_bound(void* state, isize in_size)
{
    auto* const s = static_cast<stream_compressor_state*>(state);
    if (s == nullptr)
        return isize(0);

    // The header is only emitted once, but budgeting for it every time costs a few bytes and removes a special case.
    return isize(LZ4F_HEADER_SIZE_MAX) + isize(LZ4F_compressBound(size_t(in_size), &s->prefs));
}

[[nodiscard]] cc::result<isize> stream_compress(void* state, cc::span<byte const> in, cc::span<byte> out, bool finish)
{
    auto* const s = static_cast<stream_compressor_state*>(state);
    if (s == nullptr || s->cctx == nullptr)
        return cc::error("lz4: failed to create the streaming compression context");

    auto written = isize(0);

    if (!s->begun)
    {
        auto const n = LZ4F_compressBegin_usingCDict(s->cctx, out.data(), size_t(out.size()), s->cdict, &s->prefs);
        if (LZ4F_isError(n))
            return cc::error(cc::format("lz4: failed to write the frame header: {}", LZ4F_getErrorName(n)));

        written += isize(n);
        s->begun = true;
    }

    if (!in.empty())
    {
        auto const n = LZ4F_compressUpdate(s->cctx, out.data() + written, size_t(out.size() - written), in.data(),
                                           size_t(in.size()), nullptr);
        if (LZ4F_isError(n))
            return cc::error(cc::format("lz4 streaming compression failed: {}", LZ4F_getErrorName(n)));

        written += isize(n);
    }

    if (finish)
    {
        auto const n = LZ4F_compressEnd(s->cctx, out.data() + written, size_t(out.size() - written), nullptr);
        if (LZ4F_isError(n))
            return cc::error(cc::format("lz4: failed to seal the frame: {}", LZ4F_getErrorName(n)));

        written += isize(n);
    }

    return written;
}

[[nodiscard]] cc::result<cc::impl::stream_decompress_step> stream_decompress(void* state,
                                                                             cc::span<byte const> in,
                                                                             cc::span<byte> out)
{
    auto* const s = static_cast<decompressor_state*>(state);
    if (s == nullptr || s->dctx == nullptr)
        return cc::error("lz4: failed to create the streaming decompression context");

    auto out_size = size_t(out.size());
    auto in_size = size_t(in.size());

    auto const status = LZ4F_decompress(s->dctx, out.data(), &out_size, in.data(), &in_size, nullptr);
    if (LZ4F_isError(status))
        return cc::error(cc::format("lz4 streaming decompression failed: {}", LZ4F_getErrorName(status)));

    return cc::impl::stream_decompress_step{.consumed = isize(in_size),
                                            .produced = isize(out_size),
                                            .finished = status == 0};
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
    // lz4 writes its content size into the frame header when the encoder knew it, so a prefix is enough to read it.
    .declares_size_in_header = true,
    .matches_magic = &matches_magic,
    .train_dictionary = &train_dictionary,
    .dictionary_id = &dictionary_id,
    .create_stream_compressor = &create_stream_compressor,
    .destroy_stream_compressor = &destroy_stream_compressor,
    .stream_compress_bound = &stream_compress_bound,
    .stream_compress = &stream_compress,
    // Decompression needs no extra state beyond the LZ4F_dctx the one-shot path already carries.
    .create_stream_decompressor = &create_decompressor,
    .destroy_stream_decompressor = &destroy_decompressor,
    .stream_decompress = &stream_decompress,
};
} // namespace

cc::impl::compression_backend const& cc::impl::lz4_backend()
{
    return backend;
}
