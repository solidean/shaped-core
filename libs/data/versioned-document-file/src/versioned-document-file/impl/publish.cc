#include <versioned-document-file/impl/store_io.hh>
#include <versioned-document-file/store.hh>

/// The one publisher, over a store_writer.
///
/// Everything is written in ONE transaction, and only then are the refs considered moved.
/// Both implementations run this same function, so the write order — and its atomicity — is identical on each.

namespace vdoc::file::impl
{
cc::result<publish_result> apply_publish(store_writer& writer, publish_job const& job)
{
    CC_RETURN_IF_ERROR(writer.begin());

    for (auto const& row : job.ops)
        CC_RETURN_IF_ERROR(writer.insert_op(row));

    isize blobs_written = 0;
    for (auto const& blob : job.blobs)
    {
        auto inserted = writer.insert_blob(blob.row);
        CC_RETURN_IF_ERROR(inserted);
        if (!inserted.value().has_value())
            continue; // already stored, and content-addressed, so the bytes are the same bytes

        ++blobs_written;
        auto const id = inserted.value().value();
        auto index = i64(0);
        for (isize at = 0; at < blob.data.size(); at += blob_chunk_size)
        {
            auto const size = cc::min(blob_chunk_size, blob.data.size() - at);
            CC_RETURN_IF_ERROR(
                writer.insert_chunk({.blob_id = id,
                                     .chunk_index = index,
                                     .data = cc::vector<byte>::create_copy_of(
                                         cc::span<byte const>(blob.data).subspan({.offset = at, .size = size}))}));
            ++index;
        }
    }

    for (auto const& row : job.assets)
        CC_RETURN_IF_ERROR(writer.upsert_asset(row));

    // AFTER the upserts, so publishing an asset and removing it in one call leaves it removed.
    for (auto const& asset_id : job.removed_assets)
        CC_RETURN_IF_ERROR(writer.delete_asset(asset_id));

    // Refs LAST, so they move only once everything they reach is in the same transaction.
    for (auto const& row : job.refs)
        CC_RETURN_IF_ERROR(writer.upsert_ref(row));

    CC_RETURN_IF_ERROR(writer.commit());
    return publish_result{.ops_written = job.ops.size(), .blobs_written = blobs_written};
}

cc::result<cc::unit> apply_workspace(store_writer& writer, cc::span<workspace_entry const> entries)
{
    if (entries.empty())
        return cc::unit{};

    CC_RETURN_IF_ERROR(writer.begin());
    for (auto const& entry : entries)
        CC_RETURN_IF_ERROR(
            writer.upsert_workspace({.key = entry.key,
                                     .version = i64(entry.value.version),
                                     .value = cc::vector<byte>::create_copy_of(entry.value.value.bytes())}));
    CC_RETURN_IF_ERROR(writer.commit());
    return cc::unit{};
}
} // namespace vdoc::file::impl
