#include "compression.hh"

#include <clean-core/bytes/compression_dictionary.hh>
#include <clean-core/bytes/impl/compression_backend.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/string/format.hh>

using namespace cc::primitive_defines;

namespace
{
[[nodiscard]] cc::result<cc::compression_algorithm> resolve_algorithm(cc::span<byte const> data,
                                                                      cc::decompression_config const& cfg)
{
    if (cfg.algorithm.has_value())
        return cfg.algorithm.value();

    auto const detected = cc::detect_algorithm(data);
    if (detected.has_value())
        return detected.value();

    return cc::error("decompression: no known frame magic, and no algorithm was given - a format storing compressed "
                     "blobs should record which algorithm it used rather than rely on detection");
}
} // namespace

cc::impl::compression_backend const& cc::impl::backend_for(compression_algorithm algorithm)
{
    switch (algorithm)
    {
    case compression_algorithm::zstd:
        return zstd_backend();
    case compression_algorithm::lz4:
        return lz4_backend();
    case compression_algorithm::deflate:
        return deflate_backend();
    }

    CC_UNREACHABLE("unknown compression algorithm");
}

cc::result<cc::unit> cc::impl::validate_compression_config(compression_config const& cfg)
{
    if (cfg.dictionary != nullptr && cfg.dictionary->algorithm() != cfg.algorithm)
        return cc::error("compression: the dictionary was built for a different algorithm");

    if (cfg.framing == compression_framing::zlib && cfg.algorithm != compression_algorithm::deflate)
        return cc::error("compression: `zlib` framing is the RFC 1950 wrapper around deflate, and no other algorithm "
                         "has one");

    return cc::unit{};
}

cc::result<cc::unit> cc::impl::validate_decompression_config(decompression_config const& cfg)
{
    if (cfg.framing != compression_framing::frame && !cfg.algorithm.has_value())
        return cc::error("decompression: only `frame` framing carries a magic, so any other framing needs the "
                         "algorithm given explicitly");

    if (cfg.framing == compression_framing::zlib && cfg.algorithm.value() != compression_algorithm::deflate)
        return cc::error("decompression: `zlib` framing is the RFC 1950 wrapper around deflate, and no other algorithm "
                         "has one");

    if (cfg.dictionary != nullptr && cfg.algorithm.has_value() && cfg.dictionary->algorithm() != cfg.algorithm.value())
        return cc::error("decompression: the dictionary was built for a different algorithm");

    return cc::unit{};
}

// --- one-shot ---------------------------------------------------------------------------------------------

isize cc::compress_bound(isize size, compression_config cfg)
{
    CC_ASSERT(size >= 0, "size must be >= 0");

    auto const bound = impl::backend_for(cfg.algorithm).compress_bound(size, cfg);

    // A backend reports 0 for an input it cannot describe a bound for at all, which today means lz4's raw block API
    // and its ~2 GB ceiling.
    // Catching it here beats letting cc::compress allocate nothing and fail an assert further on.
    CC_ASSERT(bound > 0 || size == 0, "input too large for this algorithm and framing (lz4 raw caps at ~2 GB per "
                                      "call)");

    return bound;
}

cc::result<isize> cc::compress_into(cc::span<byte const> data, cc::span<byte> out, compression_config cfg)
{
    CC_RETURN_IF_ERROR(impl::validate_compression_config(cfg));

    auto const& backend = impl::backend_for(cfg.algorithm);
    auto* const state = backend.create_compressor(cfg);
    auto written = backend.compress_into(state, cfg, data, out);
    backend.destroy_compressor(state);
    return written;
}

cc::vector<byte> cc::compress(cc::span<byte const> data, compression_config cfg)
{
    auto out = cc::vector<byte>::create_uninitialized(compress_bound(data.size(), cfg));

    auto const written = compress_into(data, out, cfg);

    // A compress_bound-sized buffer cannot be too small, so a failure here is a violated precondition rather than
    // anything a caller could handle — see cc::compress's own doc for which ones.
    CC_ASSERT(written.has_value(), "compression failed into a compress_bound-sized buffer");

    out.resize_down_to(written.value());
    return out;
}

cc::result<isize> cc::decompress_into(cc::span<byte const> data, cc::span<byte> out, decompression_config cfg)
{
    CC_RETURN_IF_ERROR(impl::validate_decompression_config(cfg));

    auto algorithm = resolve_algorithm(data, cfg);
    CC_RETURN_IF_ERROR(algorithm);

    auto const& backend = impl::backend_for(algorithm.value());
    auto* const state = backend.create_decompressor(cfg);
    auto written = backend.decompress_into(state, cfg, data, out);
    backend.destroy_decompressor(state);
    return written;
}

cc::result<cc::vector<byte>> cc::decompress(cc::span<byte const> data, decompression_config cfg)
{
    CC_RETURN_IF_ERROR(impl::validate_decompression_config(cfg));

    auto algorithm = resolve_algorithm(data, cfg);
    CC_RETURN_IF_ERROR(algorithm);

    auto const& backend = impl::backend_for(algorithm.value());
    auto* const state = backend.create_decompressor(cfg);
    auto result = backend.decompress_to_vector(state, cfg, data);
    backend.destroy_decompressor(state);
    return result;
}

// --- inspection -------------------------------------------------------------------------------------------

cc::optional<cc::compression_algorithm> cc::detect_algorithm(cc::span<byte const> data)
{
    if (impl::zstd_backend().matches_magic(data))
        return compression_algorithm::zstd;
    if (impl::lz4_backend().matches_magic(data))
        return compression_algorithm::lz4;
    // Only deflate's gzip framing is sniffable; its zlib wrapper is a checksum constraint rather than a magic.
    if (impl::deflate_backend().matches_magic(data))
        return compression_algorithm::deflate;

    return {};
}

cc::optional<isize> cc::decompressed_size(cc::span<byte const> data, decompression_config cfg)
{
    // Only `frame` framing declares anything, and a sniff on the others would read payload bytes as a header.
    if (cfg.framing != compression_framing::frame)
        return {};

    auto const algorithm = resolve_algorithm(data, cfg);
    if (algorithm.has_error())
        return {};

    return impl::backend_for(algorithm.value()).declared_size(data);
}

// --- reusable contexts ------------------------------------------------------------------------------------

cc::compressor::compressor(compression_config cfg) : _config(cfg)
{
    CC_ASSERT(impl::validate_compression_config(cfg).has_value(), "the dictionary was built for a different algorithm");
    _state = impl::backend_for(_config.algorithm).create_compressor(_config);
}

cc::compressor::~compressor()
{
    if (_state != nullptr)
        impl::backend_for(_config.algorithm).destroy_compressor(_state);
}

cc::compressor::compressor(compressor&& rhs) noexcept : _config(rhs._config), _state(rhs._state)
{
    rhs._state = nullptr;
}

cc::compressor& cc::compressor::operator=(compressor&& rhs) noexcept
{
    if (this == &rhs)
        return *this;

    if (_state != nullptr)
        impl::backend_for(_config.algorithm).destroy_compressor(_state);

    _config = rhs._config;
    _state = rhs._state;
    rhs._state = nullptr;
    return *this;
}

cc::result<isize> cc::compressor::compress_into(cc::span<byte const> data, cc::span<byte> out)
{
    return impl::backend_for(_config.algorithm).compress_into(_state, _config, data, out);
}

cc::vector<byte> cc::compressor::compress(cc::span<byte const> data)
{
    auto out = cc::vector<byte>::create_uninitialized(cc::compress_bound(data.size(), _config));

    auto const written = compress_into(data, out);
    CC_ASSERT(written.has_value(), "compression failed into a compress_bound-sized buffer");

    out.resize_down_to(written.value());
    return out;
}

cc::decompressor::decompressor(decompression_config cfg) : _config(cfg)
{
    // The context is per-algorithm, so it can only be built up front when the config names one.
    if (_config.algorithm.has_value())
    {
        _state_algorithm = _config.algorithm.value();
        _state = impl::backend_for(_state_algorithm).create_decompressor(_config);
    }
}

cc::decompressor::~decompressor()
{
    if (_state != nullptr)
        impl::backend_for(_state_algorithm).destroy_decompressor(_state);
}

cc::decompressor::decompressor(decompressor&& rhs) noexcept
  : _config(cc::move(rhs._config)), _state_algorithm(rhs._state_algorithm), _state(rhs._state)
{
    rhs._state = nullptr;
}

cc::decompressor& cc::decompressor::operator=(decompressor&& rhs) noexcept
{
    if (this == &rhs)
        return *this;

    if (_state != nullptr)
        impl::backend_for(_state_algorithm).destroy_decompressor(_state);

    _config = cc::move(rhs._config);
    _state_algorithm = rhs._state_algorithm;
    _state = rhs._state;
    rhs._state = nullptr;
    return *this;
}

cc::result<isize> cc::decompressor::decompress_into(cc::span<byte const> data, cc::span<byte> out)
{
    if (_state != nullptr)
        return impl::backend_for(_state_algorithm).decompress_into(_state, _config, data, out);

    return cc::decompress_into(data, out, _config);
}

cc::result<cc::vector<byte>> cc::decompressor::decompress(cc::span<byte const> data)
{
    if (_state != nullptr)
        return impl::backend_for(_state_algorithm).decompress_to_vector(_state, _config, data);

    return cc::decompress(data, _config);
}

// --- dictionaries -----------------------------------------------------------------------------------------

cc::compression_dictionary cc::compression_dictionary::from_bytes(compression_algorithm algorithm,
                                                                  cc::span<byte const> raw)
{
    auto dict = compression_dictionary();
    dict._algorithm = algorithm;
    dict._bytes = cc::vector<byte>::create_copy_of(raw);
    dict._id = impl::backend_for(algorithm).dictionary_id(dict._bytes);
    return dict;
}

cc::result<cc::compression_dictionary> cc::compression_dictionary::train(compression_algorithm algorithm,
                                                                         cc::span<cc::span<byte const> const> samples,
                                                                         isize dict_size)
{
    CC_ASSERT(dict_size > 0, "dict_size must be > 0");

    auto content = impl::backend_for(algorithm).train_dictionary(samples, dict_size);
    CC_RETURN_IF_ERROR(content);

    auto dict = compression_dictionary();
    dict._algorithm = algorithm;
    dict._bytes = cc::move(content).value();
    dict._id = impl::backend_for(algorithm).dictionary_id(dict._bytes);
    return dict;
}
