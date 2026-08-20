#include "compression_backend.hh"

#include <clean-core/bytes/compression_dictionary.hh>
#include <clean-core/common/assert.hh>

#define ZSTD_STATIC_LINKING_ONLY // ZSTD_c_format / ZSTD_d_format, which is how `raw` framing drops the magic
#include <zdict.h>
#include <zstd.h>

using namespace cc::primitive_defines;

namespace
{
/// The first four bytes of a framed zstd payload, little-endian.
constexpr isize zstd_magic_size = 4;

[[nodiscard]] cc::result<isize> zstd_failure(cc::string_view what, size_t code)
{
    return cc::error(cc::format("{}: {}", what, ZSTD_getErrorName(code)));
}

/// A level outside the build's supported window is an error inside zstd rather than a saturating choice.
[[nodiscard]] int clamped_level(int level)
{
    auto const lo = ZSTD_minCLevel();
    auto const hi = ZSTD_maxCLevel();
    return level < lo ? lo : (level > hi ? hi : level);
}

/// The frame parameters `raw` framing turns off.
///
/// Magicless drops the 4-byte signature, and the three flags drop the content size, the checksum and the dictionary id, leaving a ~2-byte header.
/// All four are exactly what a caller choosing `raw` has undertaken to record itself, which is why they are one switch rather than four knobs.
[[nodiscard]] bool apply_framing(ZSTD_CCtx* cctx, cc::compression_framing framing)
{
    if (framing != cc::compression_framing::raw)
        return true;

    return !ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_format, ZSTD_f_zstd1_magicless))
        && !ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_contentSizeFlag, 0))
        && !ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 0))
        && !ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_dictIDFlag, 0));
}

// --- compression ------------------------------------------------------------------------------------------

[[nodiscard]] void* create_compressor(cc::compression_config const& cfg)
{
    auto* const cctx = ZSTD_createCCtx();
    if (cctx == nullptr)
        return nullptr;

    auto ok = !ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, clamped_level(cfg.level)));
    ok = ok && apply_framing(cctx, cfg.framing);

    // Order matters: zstd prepares the dictionary for whatever level the cctx carries at the moment it is loaded.
    // Loading before setting the level would silently prepare it for the wrong one.
    if (ok && cfg.dictionary != nullptr && !cfg.dictionary->is_empty())
    {
        auto const raw = cfg.dictionary->bytes();
        ok = !ZSTD_isError(ZSTD_CCtx_loadDictionary(cctx, raw.data(), size_t(raw.size())));
    }

    if (!ok)
    {
        ZSTD_freeCCtx(cctx);
        return nullptr;
    }
    return cctx;
}

void destroy_compressor(void* state)
{
    ZSTD_freeCCtx(static_cast<ZSTD_CCtx*>(state));
}

[[nodiscard]] isize compress_bound(isize size, cc::compression_config const&)
{
    return isize(ZSTD_compressBound(size_t(size)));
}

[[nodiscard]] cc::result<isize> compress_into(void* state,
                                              cc::compression_config const&,
                                              cc::span<byte const> data,
                                              cc::span<byte> out)
{
    auto* const cctx = static_cast<ZSTD_CCtx*>(state);
    if (cctx == nullptr)
        return cc::error("zstd: failed to create the compression context");

    auto const written = ZSTD_compress2(cctx, out.data(), size_t(out.size()), data.data(), size_t(data.size()));
    if (ZSTD_isError(written))
        return zstd_failure("zstd compression failed", written);

    return isize(written);
}

// --- decompression ----------------------------------------------------------------------------------------

[[nodiscard]] void* create_decompressor(cc::decompression_config const& cfg)
{
    auto* const dctx = ZSTD_createDCtx();
    if (dctx == nullptr)
        return nullptr;

    auto ok = true;
    if (cfg.framing == cc::compression_framing::raw)
        ok = !ZSTD_isError(ZSTD_DCtx_setParameter(dctx, ZSTD_d_format, ZSTD_f_zstd1_magicless));

    if (ok && cfg.dictionary != nullptr && !cfg.dictionary->is_empty())
    {
        auto const raw = cfg.dictionary->bytes();
        ok = !ZSTD_isError(ZSTD_DCtx_loadDictionary(dctx, raw.data(), size_t(raw.size())));
    }

    if (!ok)
    {
        ZSTD_freeDCtx(dctx);
        return nullptr;
    }
    return dctx;
}

void destroy_decompressor(void* state)
{
    ZSTD_freeDCtx(static_cast<ZSTD_DCtx*>(state));
}

[[nodiscard]] cc::optional<isize> declared_size(cc::span<byte const> data)
{
    auto const size = ZSTD_getFrameContentSize(data.data(), size_t(data.size()));
    if (size == ZSTD_CONTENTSIZE_UNKNOWN || size == ZSTD_CONTENTSIZE_ERROR)
        return {};

    return isize(size);
}

[[nodiscard]] bool matches_magic(cc::span<byte const> data)
{
    if (data.size() < zstd_magic_size)
        return false;

    auto const magic = u32(u8(data[0])) | (u32(u8(data[1])) << 8) | (u32(u8(data[2])) << 16) | (u32(u8(data[3])) << 24);
    return magic == ZSTD_MAGICNUMBER;
}

[[nodiscard]] cc::result<isize> decompress_into(void* state,
                                                cc::decompression_config const& cfg,
                                                cc::span<byte const> data,
                                                cc::span<byte> out)
{
    auto* const dctx = static_cast<ZSTD_DCtx*>(state);
    if (dctx == nullptr)
        return cc::error("zstd: failed to create the decompression context");

    if (cfg.max_output_size >= 0 && out.size() > cfg.max_output_size)
        out = out.first_n(cfg.max_output_size);

    auto const written = ZSTD_decompressDCtx(dctx, out.data(), size_t(out.size()), data.data(), size_t(data.size()));
    if (ZSTD_isError(written))
        return zstd_failure("zstd decompression failed", written);

    return isize(written);
}

/// Decompress a frame whose size is not known up front, growing the output as it goes.
///
/// The declared size is a hint and never a bound: it is attacker-controlled on untrusted input.
/// So it seeds the first allocation and nothing more, and cfg.max_output_size is what actually stops the loop.
[[nodiscard]] cc::result<cc::vector<byte>> decompress_to_vector(void* state,
                                                                cc::decompression_config const& cfg,
                                                                cc::span<byte const> data)
{
    auto* const dctx = static_cast<ZSTD_DCtx*>(state);
    if (dctx == nullptr)
        return cc::error("zstd: failed to create the decompression context");

    auto const hint = declared_size(data);
    if (cfg.max_output_size >= 0 && hint.has_value() && hint.value() > cfg.max_output_size)
        return cc::error(
            cc::format("zstd: frame declares {} bytes, over the {} byte limit", hint.value(), cfg.max_output_size));

    // The floor keeps a tiny seed from turning the growth loop into a per-byte realloc, but it must never lift the
    // capacity back over a limit the caller set — max_output_size = 0 has to mean zero.
    auto capacity = cc::max(isize(64), hint.value_or(cc::max(isize(4096), data.size() * 4)));
    if (cfg.max_output_size >= 0)
        capacity = cc::min(capacity, cfg.max_output_size);

    auto result = cc::vector<byte>::create_uninitialized(capacity);

    ZSTD_DCtx_reset(dctx, ZSTD_reset_session_only);

    auto in = ZSTD_inBuffer{data.data(), size_t(data.size()), 0};
    auto written = isize(0);

    while (true)
    {
        auto out = ZSTD_outBuffer{result.data(), size_t(result.size()), size_t(written)};
        auto const status = ZSTD_decompressStream(dctx, &out, &in);
        if (ZSTD_isError(status))
            return cc::error(cc::format("zstd decompression failed: {}", ZSTD_getErrorName(status)));

        written = isize(out.pos);

        if (status == 0) // the frame is complete
            break;

        if (cfg.max_output_size >= 0 && written >= cfg.max_output_size)
            return cc::error(cc::format("zstd: output exceeds the {} byte limit", cfg.max_output_size));

        // Only grow once the codec actually filled what it was given; otherwise it is waiting on more input, and there
        // is none left, which means the frame was truncated.
        if (out.pos < out.size)
            return cc::error("zstd: truncated frame");

        auto next = result.size() * 2;
        if (cfg.max_output_size >= 0)
            next = cc::min(next, cfg.max_output_size);
        if (next <= result.size())
            return cc::error(cc::format("zstd: output exceeds the {} byte limit", cfg.max_output_size));

        result.resize_to_uninitialized(next);
    }

    result.resize_down_to(written);
    return result;
}

// --- dictionaries -----------------------------------------------------------------------------------------

[[nodiscard]] cc::result<cc::vector<byte>> train_dictionary(cc::span<cc::span<byte const> const> samples, isize dict_size)
{
    // ZDICT wants one flat buffer plus the sample lengths, so the samples are concatenated rather than indexed.
    auto total = isize(0);
    for (auto const& s : samples)
        total += s.size();

    auto flat = cc::vector<byte>::create_uninitialized(total);
    auto sizes = cc::vector<size_t>::create_with_capacity(samples.size());

    auto offset = isize(0);
    for (auto const& s : samples)
    {
        cc::memcpy(flat.data() + offset, s.data(), size_t(s.size()));
        offset += s.size();
        sizes.push_back(size_t(s.size()));
    }

    auto dict = cc::vector<byte>::create_uninitialized(dict_size);

    auto const written
        = ZDICT_trainFromBuffer(dict.data(), size_t(dict_size), flat.data(), sizes.data(), unsigned(sizes.size()));
    if (ZDICT_isError(written))
        return cc::error(cc::format("zstd dictionary training failed: {}", ZDICT_getErrorName(written)));

    dict.resize_down_to(isize(written));
    return dict;
}

[[nodiscard]] u32 dictionary_id(cc::span<byte const> raw)
{
    return u32(ZSTD_getDictID_fromDict(raw.data(), size_t(raw.size())));
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
    .matches_magic = &matches_magic,
    .train_dictionary = &train_dictionary,
    .dictionary_id = &dictionary_id,
};
} // namespace

cc::impl::compression_backend const& cc::impl::zstd_backend()
{
    return backend;
}
