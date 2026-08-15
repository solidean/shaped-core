#include <clean-core/string/format.hh>
#include <versioned-document-file/impl/store_memory.hh>
#include <versioned-document-file/store.hh>
#include <versioned-document/value_builder.hh>

namespace vdoc::file
{
namespace
{
/// The `parts` column: an ordered array of part objects.
///
/// The order is the contract, so this preserves it exactly; the name is written only when there is one, because an
/// absent debug label and an empty one are the same thing and only one of them costs bytes.
vdoc::value encode_parts(cc::span<asset_part const> parts)
{
    auto array = vdoc::value_builder::array();
    for (auto const& part : parts)
    {
        byte hash_bytes[blob_hash::byte_size] = {};
        part.hash.to_bytes(hash_bytes);

        auto object = vdoc::value_builder::object();
        object.set_bytes("hash", hash_bytes);
        object.set("format", part.format);
        if (!part.name.empty())
            object.set("name", part.name);
        array.push(object.build());
    }
    return array.build();
}

op_row to_row(vdoc::op const& op)
{
    auto row = op_row();
    row.hash.resize_to_uninitialized(vdoc::op_id::byte_size);
    op.id.to_bytes(row.hash);

    row.parents.resize_to_uninitialized(op.parents.size() * vdoc::op_id::byte_size);
    for (isize i = 0; i < op.parents.size(); ++i)
        op.parents[i].to_bytes(
            cc::span<byte>(row.parents).subspan({.offset = i * vdoc::op_id::byte_size, .size = vdoc::op_id::byte_size}));

    // A skeleton writes NULL/NULL, which is what it is: a position in the DAG whose content was pruned away.
    if (!op.is_skeleton())
    {
        row.metadata = cc::vector<byte>::create_copy_of(op.payload.value().metadata_bytes);
        row.assignments = cc::vector<byte>::create_copy_of(op.payload.value().assignment_bytes);
    }
    return row;
}
} // namespace

store::~store()
{
    // Nothing here can call the virtual hooks any more, so close() must already have run.
    // It is idempotent, and a derived destructor calls it before this one runs.
    CC_ASSERT(_is_closed, "a store implementation must call close() in its destructor, before its own state dies");
}

store_handle store::create_in_memory()
{
    return create_in_memory(std::make_shared<memory_image>());
}

store_handle store::create_in_memory(std::shared_ptr<memory_image> image)
{
    return impl::make_memory_store(cc::move(image));
}

// publishing
// -------------------------------------------------------------------------------------------------

cc::shared_async<publish_result> store::publish(publish_changes changes)
{
    impl_harvest_pending();

    if (_is_closed)
        // Not latched: the store is already closed, and the latch exists so a failing AUTOSAVE surfaces early.
        return cc::make_async_from_error<publish_result>(
            cc::async_error::make_error(cc::any_error(cc::string("publishing to a closed store"))));

    auto heads = cc::vector<vdoc::op_id>();
    for (auto const& [name, head] : changes.refs)
        heads.push_back(head);

    // THE safety property: an op no ref can reach is not in `reachable`, so it cannot be published even by a caller who asked for it.
    // An abandoned branch or a discarded drag preview simply is not here.
    auto const reachable = _ops.collect_reachable(heads);

    auto job = impl::publish_job();
    auto claimed = cc::vector<vdoc::op_id>();
    for (auto const& id : reachable) // already sorted by id bytes, so the write order is deterministic
    {
        if (_durable_ops.contains(id))
            continue;
        auto const* op = _ops.find(id);
        if (op == nullptr)
            continue; // collect_reachable tolerates a missing op; so does this
        job.ops.push_back(to_row(*op));
        claimed.push_back(id);
    }

    // A "you already have this" upload naming nothing stored is a publish error, and it is caught HERE — before
    // anything is enqueued, so a bad ask never opens a transaction.
    auto offered = cc::set<blob_hash>();
    for (auto const& upload : changes.blobs)
        if (upload.has_data)
            offered.insert(upload.hash);
    for (auto const& upload : changes.blobs)
        if (!upload.has_data && !_durable_blobs.contains(upload.hash) && !offered.contains(upload.hash))
            return cc::make_async_from_error<publish_result>(cc::async_error::make_error(
                cc::any_error(cc::string("a blob was published with no data, and its hash names nothing stored"))));

    auto claimed_blobs = cc::vector<blob_hash>();
    for (auto const& upload : changes.blobs)
    {
        if (!upload.has_data || _durable_blobs.contains(upload.hash))
            continue;

        auto row = blob_row{.size = upload.size,
                            .stored_size = upload.data.size(),
                            .chunk_count = (upload.data.size() + impl::blob_chunk_size - 1) / impl::blob_chunk_size,
                            .format = upload.format,
                            .encoding = upload.encoding};
        row.hash.resize_to_uninitialized(blob_hash::byte_size);
        upload.hash.to_bytes(row.hash);

        job.blobs.push_back({.row = cc::move(row), .data = cc::vector<byte>::create_copy_of(upload.data)});
        claimed_blobs.push_back(upload.hash);
    }

    for (auto const& record : changes.assets)
    {
        auto const parts = encode_parts(record.parts);
        auto row = asset_row{.asset_id = record.asset_id,
                             .kind = record.kind,
                             .parts = cc::vector<byte>::create_copy_of(parts.bytes())};
        if (!record.meta.is_null())
            row.meta = cc::vector<byte>::create_copy_of(record.meta.bytes());
        job.assets.push_back(cc::move(row));
    }

    for (auto const& [name, head] : changes.refs)
    {
        auto row = ref_row{.name = name};
        row.op_hash.resize_to_uninitialized(vdoc::op_id::byte_size);
        head.to_bytes(row.op_hash);
        job.refs.push_back(cc::move(row));
    }

    // Applied optimistically, which is exactly why is_saved means QUEUED rather than committed.
    for (auto const& [name, head] : changes.refs)
        _refs[name] = head;
    for (auto& record : changes.assets)
        _assets[record.asset_id] = cc::move(record);
    for (auto const& id : claimed)
        _durable_ops.insert(id);
    for (auto const& hash : claimed_blobs)
        _durable_blobs.insert(hash);

    auto async = on_publish(cc::move(job));
    _pending.push_back({.async = async, .claimed = cc::move(claimed), .claimed_blobs = cc::move(claimed_blobs)});
    impl_pump_until_idle();
    impl_harvest_pending();
    return async;
}

bool store::is_saved(vdoc::op_id const& head)
{
    impl_harvest_pending();
    return _durable_ops.contains(head);
}

cc::any_error const* store::sticky_error()
{
    impl_harvest_pending();
    return _sticky_error.has_value() ? &_sticky_error.value() : nullptr;
}

void store::impl_harvest_pending()
{
    // In submission order, which is completion order too: the actor serializes, and the in-memory arm is synchronous.
    auto still_pending = cc::vector<pending_publish>();
    for (auto& entry : _pending)
    {
        if (!entry.async->is_ready())
        {
            still_pending.push_back(cc::move(entry));
            continue;
        }

        if (!entry.async->has_error())
            continue;

        // The FIRST failure is what is kept, so a failing autosave is reported as the failure it was rather than as
        // whatever failed last.
        if (!_sticky_error.has_value())
            _sticky_error = cc::any_error(cc::string(entry.async->try_error()->underlying().to_string()));

        // Un-claim, so a retry writes these again.
        // Without this the durable set would become REQUIRED for correctness rather than an optimization.
        for (auto const& id : entry.claimed)
            _durable_ops.erase(id);
        for (auto const& hash : entry.claimed_blobs)
            _durable_blobs.erase(hash);
    }
    _pending = cc::move(still_pending);
}

void store::impl_pump_until_idle()
{
    // Threaded, the first call already returns false and the asyncs stay pending.
    // Unthreaded, this runs the work on the calling thread, so an async is resolved by the time it is handed back.
    while (on_pump())
    {
    }
}

std::shared_ptr<blob_source> store::make_blob_source()
{
    // The source holds a handle to this store, and this store holds a weak one back so close() can sever it.
    // A strong one either way would be a cycle neither end could break.
    auto source = std::shared_ptr<blob_source>(new blob_source(shared_from_this()));
    if (_is_closed)
        source->impl_sever();
    _blob_sources.push_back(source);
    return source;
}

// the workspace
// -------------------------------------------------------------------------------------------------

void store::set_workspace(cc::string_view key, workspace_value value)
{
    // No op, no ref, no _durable_ops touch, so is_saved cannot move.
    // Moving a camera must not look like an edit.
    _workspace[cc::string(key)] = cc::move(value);
    _workspace_dirty.insert(cc::string(key));
}

cc::optional<vdoc::value_view> store::try_get_workspace(cc::string_view key, i32 version) const
{
    auto const* entry = _workspace.get_ptr(key);
    if (entry == nullptr || entry->version != version)
        return {}; // a version this caller does not know reads as absent, and the row stays where it is
    return entry->value.view();
}

cc::shared_async<cc::unit> store::flush_workspace()
{
    impl_harvest_pending();

    if (_workspace_dirty.empty())
        return cc::make_async_from_value(cc::unit{}); // nothing dirty performs no I/O at all

    // ONLY the dirty keys, which is what keeps a key a newer build wrote and this one never touched unclobbered:
    // it is not in any statement to begin with.
    auto entries = cc::vector<workspace_entry>();
    for (auto const& key : _workspace_dirty)
        if (auto const* value = _workspace.get_ptr(key); value != nullptr)
            entries.push_back({.key = key, .value = {.version = value->version, .value = value->value}});
    _workspace_dirty.clear();

    auto async = on_flush_workspace(cc::move(entries));
    _pending_workspace.push_back(async);
    impl_pump_until_idle();
    return async;
}

// closing
// -------------------------------------------------------------------------------------------------

void store::close()
{
    if (_is_closed)
        return;

    // The flush happens HERE, in the non-virtual close, so no implementation can forget it.
    (void)flush_workspace();

    // Drain: pump until nothing is outstanding.
    // Unthreaded this runs the work; threaded it waits on the actor.
    while (true)
    {
        impl_harvest_pending();

        auto workspace_pending = cc::vector<cc::shared_async<cc::unit>>();
        for (auto& async : _pending_workspace)
            if (!async->is_ready())
                workspace_pending.push_back(cc::move(async));
        _pending_workspace = cc::move(workspace_pending);

        if (_pending.empty() && _pending_workspace.empty())
            break;
        if (!on_pump())
            break; // nothing left to run here; the actor's own shutdown drains the rest
    }

    _is_closed = true;

    // After this a blob load completes with an error rather than hanging on a dead handle.
    for (auto const& weak : _blob_sources)
        if (auto const source = weak.lock())
            source->impl_sever();
    _blob_sources.clear();

    on_close();

    impl_harvest_pending();
}

// blob_source
// -------------------------------------------------------------------------------------------------

void blob_source::impl_sever()
{
    _severed = true;
    _owner.reset();
}

cc::shared_async<cc::vector<byte>> blob_source::load(blob_hash const& hash)
{
    (void)hash;

    // Enqueue-and-return: this never blocks and never re-enters its caller, whatever state the store is in.
    if (_severed)
        return cc::make_async_from_error<cc::vector<byte>>(
            cc::async_error::make_error(cc::any_error(cc::string("the blob source was severed by close()"))));

    // The live fetch path is milestone 5. Reporting is what an unbuilt path owes a caller; hanging is not.
    return cc::make_async_from_error<cc::vector<byte>>(cc::async_error::make_error(
        cc::any_error(cc::string("loading blob bytes arrives with milestone 5 (assets and blobs)"))));
}
} // namespace vdoc::file
