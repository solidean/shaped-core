#include <clean-core/common/profiling.hh>
#include <versioned-document-file/impl/store_io.hh>
#include <versioned-document-file/store.hh>

/// The one publisher, over a store_writer.
///
/// Everything is written in ONE transaction, and only then are the refs considered moved.
/// Both implementations run this same function, so the write order — and its atomicity — is identical on each.

namespace vdoc::file::impl
{
cc::vector<cc::vector<byte>> split_into_chunks(cc::span<byte const> payload)
{
    auto out = cc::vector<cc::vector<byte>>();
    for (isize at = 0; at < payload.size(); at += payload_chunk_size)
    {
        auto const size = cc::min(payload_chunk_size, payload.size() - at);
        out.push_back(cc::vector<byte>::create_copy_of(payload.subspan({.offset = at, .size = size})));
    }

    return out;
}

cc::result<publish_result> apply_publish(store_writer& writer, publish_job const& job)
{
    // On the WORK rather than on store::publish, which only posts the job and hands back a handle.
    // A span around the enqueue would report microseconds and mean nothing; this one runs on the actor thread and
    // covers the transaction that actually writes.
    CC_RECORD_SCOPE("vdoc.file.publish");

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
        auto chunks = split_into_chunks(blob.data);
        for (isize index = 0; index < chunks.size(); ++index)
            CC_RETURN_IF_ERROR(
                writer.insert_chunk({.blob_id = id, .chunk_index = i64(index), .data = cc::move(chunks[index])}));
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
