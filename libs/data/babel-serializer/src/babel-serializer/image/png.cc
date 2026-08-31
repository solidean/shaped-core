#include <babel-serializer/image/impl/spng_backend.hh>
#include <babel-serializer/image/png.hh>
#include <clean-core/common/profiling.hh>

namespace babel::png
{
cc::result<data> read(cc::span<byte const> bytes)
{
    CC_RECORD_SCOPE("png.read");
    return babel::impl::spng_decode_png(bytes);
}

cc::result<data> read(cc::read_stream& in)
{
    auto bytes = in.read_all();
    CC_RETURN_IF_ERROR(bytes);
    return read(bytes.value());
}

cc::result<cc::vector<byte>> encode(data const& img, write_options opts)
{
    CC_RECORD_SCOPE("png.encode");

    if (img.is_empty())
        return cc::error("png encode: empty image");
    return babel::impl::spng_encode_png(img, opts.compression_level);
}

cc::result<cc::unit> write(cc::write_stream& out, data const& img, write_options opts)
{
    auto encoded = encode(img, opts);
    CC_RETURN_IF_ERROR(encoded);
    CC_RETURN_IF_ERROR(out.write(encoded.value()));
    return cc::unit{};
}
} // namespace babel::png
