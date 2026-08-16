#include <clean-core/string/format.hh>
#include <versioned-document-file/impl/blob_codec.hh>
#include <versioned-document-file/impl/store_io.hh>

/// The one route from a blob hash to bytes, over a blob_payload_reader.
///
/// Shared by both store implementations, which is what makes a torn blob, an unknown encoding and a range past the end
/// behave identically on each rather than similarly.

namespace vdoc::file::impl
{
cc::result<cc::vector<byte>> fetch_blob(blob_payload_reader& reader, blob_hash const& hash, blob_fetch_range range)
{
    auto header = reader.read_blob_header(hash);
    CC_RETURN_IF_ERROR(header);
    if (!header.value().has_value())
        return cc::error(cc::any_error(cc::string("no blob is stored under this hash")));

    auto const& blob = header.value().value();

    // Unlike the load, which reports an unknown encoding and carries on, a FETCH of such a blob can only fail: there is
    // no partial answer to give.
    auto const* codec = find_blob_codec(blob.encoding);
    if (codec == nullptr)
        return cc::error(cc::any_error(cc::format("this build has no codec for the blob encoding '{}'", blob.encoding)));

    if (range.offset < 0 || range.offset > blob.decoded_size)
        return cc::error(cc::any_error(
            cc::format("a blob range starts at {} but the blob decodes to {} bytes", range.offset, blob.decoded_size)));

    auto const available = blob.decoded_size - range.offset;
    auto const wanted = range.size < 0 ? available : cc::min(range.size, available);

    // The only path that avoids materializing a multi-gigabyte blob to answer for 64 bytes of it — and it exists only
    // where a stored offset IS a decoded offset.
    //
    // A WHOLE-blob fetch deliberately does not take it, even under a byte-addressable codec: routing every full read
    // through decode is what keeps that half of the seam exercised rather than dead until the first real codec.
    // Under `raw` the decode is a move, so the fast path buys nothing here anyway.
    auto const is_partial = range.offset != 0 || wanted != blob.decoded_size;
    if (codec->is_byte_addressable && is_partial)
    {
        auto out = cc::vector<byte>();
        out.resize_to_uninitialized(wanted);
        CC_RETURN_IF_ERROR(reader.read_stored_range(blob, range.offset, out));
        return out;
    }

    auto stored = cc::vector<byte>();
    stored.resize_to_uninitialized(blob.stored_size);
    CC_RETURN_IF_ERROR(reader.read_stored_range(blob, 0, stored));

    // The seam a real codec moves at.
    // `raw` decodes by identity, so today this runs on the thread that did the read; a codec that actually works would
    // be handed off instead, which is why the read and the decode are two statements rather than one expression.
    auto decoded = codec->decode(cc::move(stored), blob.decoded_size);
    CC_RETURN_IF_ERROR(decoded);
    if (decoded.value().size() != blob.decoded_size)
        return cc::error(cc::any_error(cc::format("decoding a blob produced {} bytes but its row promises {}",
                                                  decoded.value().size(), blob.decoded_size)));

    if (range.offset == 0 && wanted == blob.decoded_size)
        return cc::move(decoded.value());

    return cc::vector<byte>::create_copy_of(
        cc::span<byte const>(decoded.value()).subspan({.offset = range.offset, .size = wanted}));
}
} // namespace vdoc::file::impl
