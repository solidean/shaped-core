#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <versioned-document-file/impl/store_memory.hh>

namespace vdoc::file::impl
{
namespace
{
cc::vector<byte> copy_of(cc::span<byte const> bytes)
{
    return cc::vector<byte>::create_copy_of(bytes);
}

cc::optional<cc::vector<byte>> copy_of_optional(cc::optional<cc::vector<byte>> const& bytes)
{
    if (!bytes.has_value())
        return {};
    return copy_of(cc::span<byte const>(bytes.value()));
}

bool same_bytes(cc::span<byte const> a, cc::span<byte const> b)
{
    if (a.size() != b.size())
        return false;
    for (isize i = 0; i < a.size(); ++i)
        if (a[i] != b[i])
            return false;
    return true;
}

/// Reads rows straight out of an image.
///
/// Everything is copied out rather than viewed, because the loader's contract is the same on both arms: what it gets
/// back is its own, and the image may be mutated afterwards.
class memory_reader final : public store_reader
{
public:
    explicit memory_reader(memory_image const& image) : _image(image) {}

    cc::result<cc::vector<blob_row>> read_blobs() override
    {
        auto out = cc::vector<blob_row>();
        for (auto const& row : _image.blobs)
            out.push_back({.id = row.id,
                           .hash = copy_of(row.hash),
                           .size = row.size,
                           .stored_size = row.stored_size,
                           .chunk_count = row.chunk_count,
                           .format = row.format,
                           .encoding = row.encoding});
        return out;
    }

    cc::result<cc::vector<chunk_summary>> read_chunk_summaries() override
    {
        // The file arm answers this with COUNT and SUM(LENGTH(data)), which reads row headers and never a payload.
        // Summarizing rather than handing back the chunks is what keeps the two arms honest about that.
        auto totals = cc::map<i64, chunk_summary>();
        for (auto const& chunk : _image.blob_chunks)
        {
            auto& summary = totals[chunk.blob_id];
            summary.blob_id = chunk.blob_id;
            summary.count += 1;
            summary.total_bytes += chunk.data.size();
        }

        auto out = cc::vector<chunk_summary>();
        for (auto const& [id, summary] : totals)
            out.push_back(summary);
        return out;
    }

    cc::result<cc::vector<asset_row>> read_assets() override
    {
        auto out = cc::vector<asset_row>();
        for (auto const& row : _image.assets)
            out.push_back({.asset_id = row.asset_id,
                           .kind = row.kind,
                           .parts = copy_of(cc::span<byte const>(row.parts)),
                           .meta = copy_of_optional(row.meta),
                           .deps = copy_of_optional(row.deps)});
        return out;
    }

    cc::result<cc::vector<op_row>> read_ops() override
    {
        auto out = cc::vector<op_row>();
        for (auto const& row : _image.ops)
            out.push_back({.hash = copy_of(cc::span<byte const>(row.hash)),
                           .parents = copy_of(cc::span<byte const>(row.parents)),
                           .metadata = copy_of_optional(row.metadata),
                           .assignments = copy_of_optional(row.assignments)});
        return out;
    }

    cc::result<cc::vector<ref_row>> read_refs() override
    {
        auto out = cc::vector<ref_row>();
        for (auto const& row : _image.refs)
            out.push_back({.name = row.name, .op_hash = copy_of(cc::span<byte const>(row.op_hash))});
        return out;
    }

    cc::result<cc::vector<snapshot_row>> read_snapshots() override
    {
        auto out = cc::vector<snapshot_row>();
        for (auto const& row : _image.snapshots)
            out.push_back({.op_hash = copy_of(cc::span<byte const>(row.op_hash)),
                           .required = row.required,
                           .encoding = row.encoding,
                           .data = copy_of(cc::span<byte const>(row.data))});
        return out;
    }

    cc::result<cc::vector<workspace_row>> read_workspace() override
    {
        auto out = cc::vector<workspace_row>();
        for (auto const& row : _image.workspace)
            out.push_back({.key = row.key, .version = row.version, .value = copy_of(cc::span<byte const>(row.value))});
        return out;
    }

    cc::result<cc::vector<meta_row>> read_meta() override
    {
        auto out = cc::vector<meta_row>();
        for (auto const& row : _image.meta)
            out.push_back({.key = row.key, .value = copy_of_optional(row.value)});
        return out;
    }

    cc::span<cc::string const> unknown_tables() const override { return _unknown_tables; }
    cc::span<cc::string const> unknown_columns() const override { return _image.unknown_columns; }

private:
    memory_image const& _image;
    /// Materialized once, because unknown_tables() hands back a span of names and the image stores whole entries.
    cc::vector<cc::string> _unknown_tables = [this]
    {
        auto names = cc::vector<cc::string>();
        for (auto const& table : _image.unknown_tables)
            names.push_back(table.name);
        return names;
    }();
};

/// Reads blob payloads out of an image.
///
/// The same gap and short-read checks the file arm makes, so a torn blob lands on one code path rather than on two that
/// happen to agree today.
class memory_blob_payload_reader final : public blob_payload_reader
{
public:
    explicit memory_blob_payload_reader(memory_image const& image) : _image(image) {}

    cc::result<cc::optional<blob_header>> read_blob_header(blob_hash const& hash) override
    {
        byte hash_bytes[blob_hash::byte_size] = {};
        hash.to_bytes(hash_bytes);

        for (auto const& row : _image.blobs)
            if (same_bytes(row.hash, hash_bytes))
                return cc::optional<blob_header>(blob_header{.id = row.id,
                                                             .decoded_size = row.size,
                                                             .stored_size = row.stored_size,
                                                             .chunk_count = row.chunk_count,
                                                             .encoding = row.encoding});
        return cc::optional<blob_header>();
    }

    cc::result<cc::unit> read_stored_range(blob_header const& blob, i64 offset, cc::span<byte> out) override
    {
        if (out.empty())
            return cc::unit{};

        // Placed by index rather than sorted, so a missing chunk stays a hole instead of closing up silently — which is
        // exactly the torn blob this has to report.
        auto chunks = cc::vector<chunk_row const*>();
        chunks.resize_to_filled(blob.chunk_count, nullptr);
        for (auto const& chunk : _image.blob_chunks)
            if (chunk.blob_id == blob.id && chunk.chunk_index >= 0 && chunk.chunk_index < blob.chunk_count)
                chunks[chunk.chunk_index] = &chunk;

        auto at = i64(0);
        auto filled = i64(0);
        for (auto const* chunk : chunks)
        {
            if (filled == out.size())
                break;
            if (chunk == nullptr)
                break; // a hole: everything past it is unreachable, and the short fill below reports it
            auto const chunk_end = at + chunk->data.size();
            if (chunk_end <= offset)
            {
                at = chunk_end;
                continue;
            }

            auto const from = cc::max(i64(0), offset - at);
            auto const take = cc::min(i64(chunk->data.size()) - from, i64(out.size()) - filled);
            for (isize i = 0; i < take; ++i)
                out[filled + i] = chunk->data[from + i];
            filled += take;
            at = chunk_end;
        }

        if (filled != out.size())
            return cc::error(
                cc::any_error(cc::format("a blob's chunks cover {} of the {} bytes asked for", filled, out.size())));
        return cc::unit{};
    }

private:
    memory_image const& _image;
};

/// Writes rows into an image.
///
/// The transaction is a staged copy: `begin` snapshots the image, every write lands on the copy, and `commit` swaps it in.
/// Anything else — an error, or this writer dying — throws the copy away.
/// That is the same all-or-nothing guarantee the file arm gets from SQLite, reached the only way an in-memory arm can.
class memory_writer final : public store_writer
{
public:
    explicit memory_writer(memory_image& image) : _image(image) {}

    cc::result<cc::unit> begin() override
    {
        if (_image.writes_fail)
            return cc::error(cc::any_error(cc::string("the in-memory store was told to refuse writes")));
        _staged = _image;
        _is_open = true;
        return cc::unit{};
    }

    cc::result<cc::unit> insert_op(op_row const& row) override
    {
        // Content-addressed: a key already present means the identical row, so nothing is rewritten.
        for (auto const& existing : _staged.ops)
            if (same_bytes(existing.hash, row.hash))
                return cc::unit{};
        _staged.ops.push_back({.hash = copy_of(cc::span<byte const>(row.hash)),
                               .parents = copy_of(cc::span<byte const>(row.parents)),
                               .metadata = copy_of_optional(row.metadata),
                               .assignments = copy_of_optional(row.assignments)});
        return cc::unit{};
    }

    cc::result<cc::optional<i64>> insert_blob(blob_row const& row) override
    {
        for (auto const& existing : _staged.blobs)
            if (same_bytes(existing.hash, row.hash))
                return cc::optional<i64>();

        auto stored = row;
        stored.id = _staged.next_blob_id++;
        stored.hash = copy_of(cc::span<byte const>(row.hash));
        auto const id = stored.id;
        _staged.blobs.push_back(cc::move(stored));
        return cc::optional<i64>(id);
    }

    cc::result<cc::unit> insert_chunk(chunk_row const& row) override
    {
        _staged.blob_chunks.push_back(
            {.blob_id = row.blob_id, .chunk_index = row.chunk_index, .data = copy_of(cc::span<byte const>(row.data))});
        return cc::unit{};
    }

    cc::result<cc::unit> upsert_asset(asset_row const& row) override
    {
        // The one mutable mapping in the format, so this one really does overwrite.
        for (auto& existing : _staged.assets)
            if (existing.asset_id == row.asset_id)
            {
                existing.kind = row.kind;
                existing.parts = copy_of(cc::span<byte const>(row.parts));
                existing.meta = copy_of_optional(row.meta);
                existing.deps = copy_of_optional(row.deps);
                return cc::unit{};
            }
        _staged.assets.push_back({.asset_id = row.asset_id,
                                  .kind = row.kind,
                                  .parts = copy_of(cc::span<byte const>(row.parts)),
                                  .meta = copy_of_optional(row.meta),
                                  .deps = copy_of_optional(row.deps)});
        return cc::unit{};
    }

    cc::result<cc::unit> delete_asset(cc::string_view asset_id) override
    {
        auto kept = cc::vector<asset_row>();
        for (auto& existing : _staged.assets)
            if (existing.asset_id != asset_id)
                kept.push_back(cc::move(existing));
        _staged.assets = cc::move(kept);
        return cc::unit{};
    }

    cc::result<cc::unit> delete_blob(blob_hash const& hash) override
    {
        byte hash_bytes[blob_hash::byte_size] = {};
        hash.to_bytes(hash_bytes);

        // The chunks go with the blob HERE, by hand: the file arm gets this from ON DELETE CASCADE, and an in-memory
        // image that left them behind would disagree with it about what a torn file even looks like.
        auto removed_ids = cc::vector<i64>();
        auto kept = cc::vector<blob_row>();
        for (auto& existing : _staged.blobs)
        {
            if (same_bytes(existing.hash, hash_bytes))
                removed_ids.push_back(existing.id);
            else
                kept.push_back(cc::move(existing));
        }
        _staged.blobs = cc::move(kept);

        auto kept_chunks = cc::vector<chunk_row>();
        for (auto& chunk : _staged.blob_chunks)
        {
            auto is_orphan = false;
            for (auto const id : removed_ids)
                if (chunk.blob_id == id)
                    is_orphan = true;
            if (!is_orphan)
                kept_chunks.push_back(cc::move(chunk));
        }
        _staged.blob_chunks = cc::move(kept_chunks);
        return cc::unit{};
    }

    cc::result<cc::unit> upsert_ref(ref_row const& row) override
    {
        for (auto& existing : _staged.refs)
            if (existing.name == row.name)
            {
                existing.op_hash = copy_of(cc::span<byte const>(row.op_hash));
                return cc::unit{};
            }
        _staged.refs.push_back({.name = row.name, .op_hash = copy_of(cc::span<byte const>(row.op_hash))});
        return cc::unit{};
    }

    cc::result<cc::unit> upsert_workspace(workspace_row const& row) override
    {
        for (auto& existing : _staged.workspace)
            if (existing.key == row.key)
            {
                existing.version = row.version;
                existing.value = copy_of(cc::span<byte const>(row.value));
                return cc::unit{};
            }
        _staged.workspace.push_back(
            {.key = row.key, .version = row.version, .value = copy_of(cc::span<byte const>(row.value))});
        return cc::unit{};
    }

    cc::result<cc::unit> commit() override
    {
        if (!_is_open)
            return cc::error(cc::any_error(cc::string("committing a transaction that was never begun")));
        _image = cc::move(_staged);
        _is_open = false;
        return cc::unit{};
    }

private:
    memory_image& _image;
    memory_image _staged;
    bool _is_open = false;
};

/// The store over a memory_image.
///
/// Every hook completes inline, so an async handed back from one is already resolved.
class memory_store final : public store
{
public:
    explicit memory_store(std::shared_ptr<memory_image> image) : _image(cc::move(image)) {}

    ~memory_store() override { close(); }

protected:
    cc::shared_async<publish_result> on_publish(publish_job job) override
    {
        auto writer = memory_writer(*_image);
        auto applied = apply_publish(writer, job);
        if (applied.has_error())
            return cc::make_async_from_error<publish_result>(cc::async_error::make_error(cc::move(applied).error()));
        return cc::make_async_from_value(applied.value());
    }

    cc::shared_async<reclaim_result> on_reclaim(reclaim_job job) override
    {
        auto writer = memory_writer(*_image);
        auto applied = apply_reclaim(writer, job);
        if (applied.has_error())
            return cc::make_async_from_error<reclaim_result>(cc::async_error::make_error(cc::move(applied).error()));
        return cc::make_async_from_value(applied.value());
    }

    cc::shared_async<cc::unit> on_flush_workspace(cc::vector<workspace_entry> entries) override
    {
        auto writer = memory_writer(*_image);
        auto applied = apply_workspace(writer, entries);
        if (applied.has_error())
            return cc::make_async_from_error<cc::unit>(cc::async_error::make_error(cc::move(applied).error()));
        return cc::make_async_from_value(cc::unit{});
    }

    cc::shared_async<cc::vector<byte>> on_fetch_blob(blob_hash const& hash, blob_fetch_range range) override
    {
        // This arm could answer right here, and that is exactly the danger: resolving now would wake the caller's
        // continuation inside its own load() call, under whatever lock it holds.
        // So the bytes wait for a pump even though they are already in hand.
        auto promise = cc::make_async_manual<cc::vector<byte>>();
        _pending_fetches.push_back({.hash = hash, .range = range, .promise = promise});
        return promise;
    }

    bool on_pump() override
    {
        if (_pending_fetches.empty())
            return false;

        // Drained into a local FIRST: resolving a promise wakes a continuation that may enqueue another fetch, and
        // appending to the vector being walked would invalidate the walk.
        auto const batch = cc::move(_pending_fetches);
        _pending_fetches.clear();

        for (auto const& entry : batch)
        {
            auto reader = memory_blob_payload_reader(*_image);
            auto fetched = fetch_blob(reader, entry.hash, entry.range);
            if (fetched.has_error())
                entry.promise->push_error(cc::async_error::make_error(cc::move(fetched).error()));
            else
                entry.promise->push_value(cc::move(fetched.value()));
        }

        // One batch per call, so close()'s drain loop still terminates while a continuation that enqueued a follow-up
        // fetch is still served.
        return true;
    }

    void on_close() override
    {
        // Nothing to release: an image is owned by whoever made it, and outliving this store is the point.
    }

private:
    struct pending_fetch
    {
        blob_hash hash;
        blob_fetch_range range;
        cc::shared_async<cc::vector<byte>> promise;
    };

    std::shared_ptr<memory_image> _image;
    cc::vector<pending_fetch> _pending_fetches;
};
} // namespace

store_handle make_memory_store(std::shared_ptr<memory_image> const& image)
{
    // A fresh image is stamped here, so the identity and version checks look the same on both arms.
    if (image->application_id == 0 && image->user_version == 0)
    {
        image->application_id = memory_image::vdoc_application_id;
        image->user_version = memory_image::current_user_version;
    }

    auto handle = std::make_shared<memory_store>(image);
    auto reader = memory_reader(*image);
    auto const loaded = load(reader, *handle);
    CC_ASSERT(loaded.has_value(), "an in-memory load has no hard failure to report");
    return handle;
}
} // namespace vdoc::file::impl
