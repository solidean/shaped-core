#include <clean-core/algorithm/sort.hh>
#include <clean-core/string/format.hh>
#include <versioned-document-file/impl/payload_codec.hh>
#include <versioned-document-file/impl/snapshot_codec.hh>
#include <versioned-document-file/impl/store_memory.hh>
#include <versioned-document-file/store.hh>
#include <versioned-document/value_builder.hh>


namespace vdoc::file
{
namespace
{
/// The `parts` column: an ordered array of part objects.
///
/// Declaration order is preserved exactly, because it is what disambiguates parts sharing a name.
/// `$main` is written as an ABSENT name field and read back as `$main`, so the ceremony-free single-part asset costs
/// no bytes for a name every such asset would carry.
/// An empty name is written explicitly, since it has to stay distinguishable from that default.
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
        if (part.name != main_part_name)
            object.set("name", part.name);
        array.push(object.build());
    }
    return array.build();
}

/// The `deps` column: an array of asset id strings, exactly as the application declared them.
///
/// Written verbatim rather than deduplicated or sorted: this is the application's declaration, and normalizing it here
/// would make a round-trip lossy for no gain — the flood fill does not care about order or repeats.
vdoc::value encode_deps(cc::span<cc::string const> dependencies)
{
    auto array = vdoc::value_builder::array();
    for (auto const& id : dependencies)
        array.push(cc::string_view(id));
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

/// The DAG as it will be once a verified batch lands, for the checks that have to run before it does.
///
/// A batch can only ever fill a skeleton or add an op, never move an edge, so this is the real graph with a few holes
/// read as filled.
struct pending_graph
{
    vdoc::op_graph const& graph;
    cc::map<vdoc::op_id, vdoc::op const*> incoming;

    [[nodiscard]] bool has_payload(vdoc::op_id const& id) const
    {
        if (incoming.contains(id))
            return true;

        auto const* const o = graph.find(id);
        return o != nullptr && !o->is_skeleton();
    }

    [[nodiscard]] cc::span<vdoc::op_id const> parents_of(vdoc::op_id const& id) const
    {
        if (auto const* const* const staged = incoming.get_ptr(id))
            return (*staged)->parents;

        auto const* const o = graph.find(id);
        return o != nullptr ? cc::span<vdoc::op_id const>(o->parents) : cc::span<vdoc::op_id const>();
    }
};

/// Would every op behind `head` have its payload once the batch lands?
///
/// An ancestor that is MISSING counts as incomplete just as a skeleton does: replaying over a hole is lossy in exactly
/// the same way, so a snapshot standing over one is still load-bearing.
[[nodiscard]] bool ancestry_is_complete(pending_graph const& pending, vdoc::op_id const& head)
{
    auto seen = cc::set<vdoc::op_id>();
    auto stack = cc::vector<vdoc::op_id>();
    for (auto const& parent : pending.parents_of(head))
        stack.push_back(parent);

    while (!stack.empty())
    {
        auto const id = stack.back();
        stack.remove_back();

        if (seen.contains(id))
            continue;
        seen.insert(id);

        if (!pending.has_payload(id))
            return false;

        for (auto const& parent : pending.parents_of(id))
            stack.push_back(parent);
    }

    return true;
}

/// Is `head` reachable from `from` through parent edges?
[[nodiscard]] bool descends_from(pending_graph const& pending, vdoc::op_id const& from, vdoc::op_id const& head)
{
    auto seen = cc::set<vdoc::op_id>();
    auto stack = cc::vector<vdoc::op_id>();
    stack.push_back(from);

    while (!stack.empty())
    {
        auto const id = stack.back();
        stack.remove_back();

        if (id == head)
            return true;

        if (seen.contains(id))
            continue;
        seen.insert(id);

        for (auto const& parent : pending.parents_of(id))
            stack.push_back(parent);
    }

    return false;
}

/// The refusal, in the words a caller can act on.
[[nodiscard]] cc::string describe(vdoc::integration_rejection const& rejection)
{
    if (rejection.reason == vdoc::integration_error::parents_disagree)
        return cc::string("its parents are not the ones this document already holds under that id");

    return rejection.decode_error == vdoc::op_decode_error::hash_mismatch
             ? cc::string("its bytes do not hash to the id it was sent under, which means corruption or tampering")
             : cc::string("its bytes are not a well-formed op");
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

    for (auto const& upload : changes.blobs)
    {
        // Note the asymmetry with the load path, which treats the same unknown encoding as an issue and carries on: a
        // FILE naming a codec this build lacks is someone else's newer writer, while a CALLER asking for one is this
        // build asking itself for something it cannot do.
        if (impl::find_payload_codec(upload.encoding) == nullptr)
            return cc::make_async_from_error<publish_result>(cc::async_error::make_error(cc::any_error(cc::format(
                "a blob was published under the encoding '{}', which this build has no codec for", upload.encoding))));

        // Under raw the decoded size is recoverable from the stored bytes; under anything else only a decode knows it,
        // so an unstated one would be unrecoverable rather than merely absent.
        if (upload.encoding != "raw" && upload.decoded_size <= 0)
            return cc::make_async_from_error<publish_result>(cc::async_error::make_error(
                cc::any_error(cc::string("a blob was published under a non-raw encoding without its decoded size"))));
    }

    auto claimed_blobs = cc::vector<blob_hash>();
    for (auto const& upload : changes.blobs)
    {
        if (!upload.has_data || _durable_blobs.contains(upload.hash))
            continue;

        auto row = blob_row{.size = upload.decoded_size > 0 ? upload.decoded_size : upload.data.size(),
                            .stored_size = upload.data.size(),
                            .chunk_count = (upload.data.size() + impl::payload_chunk_size - 1) / impl::payload_chunk_size,
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
        // An asset with nothing declared writes NULL rather than an empty array, so "declared nothing" and "declared
        // an empty list" cannot drift apart into two encodings of one fact.
        if (!record.dependencies.empty())
            row.deps = cc::vector<byte>::create_copy_of(encode_deps(record.dependencies).bytes());
        job.assets.push_back(cc::move(row));
    }

    for (auto const& asset_id : changes.removed_assets)
        job.removed_assets.push_back(asset_id);

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
    // Removals after the upserts here too, matching the order the writer applies them in.
    for (auto const& asset_id : changes.removed_assets)
        (void)_assets.erase(asset_id);
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

    // Pruned on the way in, because resolve_asset makes this a real growth path rather than a theoretical one: a
    // caller resolving assets every frame would otherwise grow this vector forever.
    auto live = cc::vector<std::weak_ptr<blob_source>>();
    for (auto& weak : _blob_sources)
        if (!weak.expired())
            live.push_back(cc::move(weak));
    live.push_back(source);
    _blob_sources = cc::move(live);
    return source;
}

// reclamation
// -------------------------------------------------------------------------------------------------

cc::shared_async<reclaim_result> store::reclaim(cc::span<cc::string const> roots)
{
    impl_harvest_pending();

    if (_is_closed)
        return cc::make_async_from_error<reclaim_result>(
            cc::async_error::make_error(cc::any_error(cc::string("reclaiming in a closed store"))));

    // The closure, by flood fill over the resident index.
    // `retained` doubles as the visited set, which is what makes a cycle terminate rather than need detecting.
    auto retained = cc::set<cc::string>();
    auto frontier = cc::vector<cc::string>();
    for (auto const& root : roots)
        if (_assets.contains(root) && retained.insert(root))
            frontier.push_back(root);

    while (!frontier.empty())
    {
        auto const id = frontier.back();
        frontier.remove_back();

        auto const* record = _assets.get_ptr(id);
        if (record == nullptr)
            continue;

        for (auto const& dependency : record->dependencies)
        {
            // A dependency naming nothing in this file is skipped in silence: a file is one asset source among many,
            // so an id resolving elsewhere is the expected case rather than a defect to report.
            if (!_assets.contains(dependency))
                continue;
            if (retained.insert(dependency))
                frontier.push_back(dependency);
        }
    }

    auto job = impl::reclaim_job();
    for (auto const& [asset_id, record] : _assets)
        if (!retained.contains(asset_id))
            job.removed_assets.push_back(asset_id);

    // Marked from the RETAINED assets alone — which is the one line that makes a root set mean anything.
    auto marked = cc::set<blob_hash>();
    for (auto const& asset_id : retained)
        if (auto const* record = _assets.get_ptr(asset_id); record != nullptr)
            for (auto const& part : record->parts)
                marked.insert(part.hash);

    for (auto const& hash : _durable_blobs)
        if (!marked.contains(hash))
            job.removed_blobs.push_back(hash);

    for (auto const& asset_id : job.removed_assets)
        (void)_assets.erase(asset_id);
    for (auto const& hash : job.removed_blobs)
        _durable_blobs.erase(hash);

    auto async = on_reclaim(cc::move(job));
    impl_pump_until_idle();
    return async;
}

// snapshots and pruning
// -------------------------------------------------------------------------------------------------

namespace
{
/// Encodes one cached snapshot into the row and chunks a writer takes.
[[nodiscard]] cc::result<impl::snapshot_write> encode_for_storage(vdoc::op_id const& op,
                                                                  vdoc::snapshot_document const& snapshot,
                                                                  bool required)
{
    auto const decoded = impl::encode_snapshot(snapshot.document());

    auto const* const codec = impl::find_payload_codec("raw");
    CC_ASSERT(codec != nullptr, "the raw codec is always registered");

    auto stored = codec->encode(cc::vector<byte>::create_copy_of(decoded));
    CC_RETURN_IF_ERROR(stored);

    auto chunks = impl::split_into_chunks(stored.value());
    auto row = snapshot_row{.required = required ? 1 : 0,
                            .encoding = cc::string("raw"),
                            .decoded_size = decoded.size(),
                            .stored_size = stored.value().size(),
                            .chunk_count = chunks.size()};
    row.op_hash.resize_to_uninitialized(vdoc::op_id::byte_size);
    op.to_bytes(row.op_hash);

    return impl::snapshot_write{.row = cc::move(row), .chunks = cc::move(chunks)};
}
} // namespace

cc::shared_async<snapshot_write_result> store::publish_snapshots(cc::span<vdoc::op_id const> ops)
{
    impl_harvest_pending();

    if (_is_closed)
        return cc::make_async_from_error<snapshot_write_result>(
            cc::async_error::make_error(cc::any_error(cc::string("publishing snapshots in a closed store"))));

    auto job = impl::snapshot_write_job();
    for (auto const& op : ops)
    {
        auto const* const snapshot = _snapshot_cache.find(op);
        if (snapshot == nullptr)
            continue; // the cache is derived and may have evicted it, which is not something to report

        auto encoded = encode_for_storage(op, *snapshot, /*required =*/false);
        if (encoded.has_error())
            continue;

        _snapshots[op] = snapshot_entry{.op = op,
                                        .required = false,
                                        .encoding = encoded.value().row.encoding,
                                        .decoded_size = encoded.value().row.decoded_size,
                                        .decoded = true};

        job.snapshots.push_back(cc::move(encoded.value()));
    }

    auto async = on_write_snapshots(cc::move(job));
    impl_pump_until_idle();
    return async;
}

cc::shared_async<snapshot_write_result> store::prune(vdoc::op_id const& head)
{
    impl_harvest_pending();

    if (_is_closed)
        return cc::make_async_from_error<snapshot_write_result>(
            cc::async_error::make_error(cc::any_error(cc::string("pruning a closed store"))));

    if (!_ops.contains(head))
        return cc::make_async_from_error<snapshot_write_result>(
            cc::async_error::make_error(cc::any_error(cc::string("pruning at an op this document does not have"))));

    // Computed here, on the calling thread, so nothing crosses to storage until the whole answer is known.
    auto const doc = _ops.materialize(head, _snapshot_cache);
    auto snapshot = vdoc::snapshot_document::create_owning_copy(doc);

    auto encoded = encode_for_storage(head, snapshot, /*required =*/true);
    if (encoded.has_error())
        return cc::make_async_from_error<snapshot_write_result>(cc::async_error::make_error(
            cc::any_error(cc::string("pruning at an op whose snapshot could not be encoded"))));

    // EVERY ref must descend from `head`, or this prune is refused.
    //
    // A required snapshot carries no `superseded`, and that is only sound while nothing can present a writer from
    // behind it.
    // A ref that forked BEFORE `head` can: its own branch keeps its ancestors, so it still offers writers that ops
    // behind `head` superseded — and merging the two would fabricate a multi-value nobody authored.
    // Replaying instead is no answer either, since the ops that would have suppressed it are now skeletons.
    //
    // So the boundary a document may prune to is the oldest op every ref still descends from, and asking for more is
    // an error rather than something to silently do half of.
    for (auto const& [name, ref_head] : _refs)
    {
        if (ref_head == head)
            continue;

        auto descends = false;
        op_id const other[] = {ref_head};
        for (auto const& id : _ops.collect_reachable(other))
            descends = descends || id == head;

        if (!descends)
            return cc::make_async_from_error<snapshot_write_result>(cc::async_error::make_error(cc::any_error(
                cc::format("pruning at an op the ref '{}' does not descend from, which would leave the two unable to "
                           "merge correctly",
                           name))));
    }

    op_id const head_span[] = {head};
    auto behind = cc::set<vdoc::op_id>();
    for (auto const& id : _ops.collect_reachable(head_span))
        if (!(id == head))
            behind.insert(id);

    auto job = impl::snapshot_write_job();
    auto const encoding = encoded.value().row.encoding;
    auto const decoded_size = encoded.value().row.decoded_size;
    job.snapshots.push_back(cc::move(encoded.value()));

    for (auto const& id : behind)
    {
        // Already a skeleton means already pruned, and re-emptying it would count a second time.
        auto const* const o = _ops.find(id);
        if (o == nullptr || o->is_skeleton())
            continue;

        auto hash = cc::vector<byte>();
        hash.resize_to_uninitialized(vdoc::op_id::byte_size);
        id.to_bytes(hash);
        job.skeletonized.push_back(cc::move(hash));
    }

    cc::sort(job.skeletonized,
             [](cc::vector<byte> const& a, cc::vector<byte> const& b)
             {
                 for (isize i = 0; i < a.size() && i < b.size(); ++i)
                     if (a[i] != b[i])
                         return a[i] < b[i];
                 return a.size() < b.size();
             });

    // The in-memory graph follows the file, so a prune needs no reopen to take effect.
    for (auto const& id : behind)
        (void)_ops.skeletonize(id);

    _snapshots[head]
        = snapshot_entry{.op = head, .required = true, .encoding = encoding, .decoded_size = decoded_size, .decoded = true};
    _snapshot_cache.install(head, cc::move(snapshot), /*pinned =*/true);

    auto async = on_write_snapshots(cc::move(job));
    impl_pump_until_idle();
    return async;
}

// recovering history from a peer
// -------------------------------------------------------------------------------------------------

cc::shared_async<recovery_result> store::recover(cc::span<vdoc::received_op const> batch)
{
    impl_harvest_pending();

    if (_is_closed)
        return cc::make_async_from_error<recovery_result>(
            cc::async_error::make_error(cc::any_error(cc::string("recovering history in a closed store"))));

    // Verified against today's graph and stored nowhere, so every refusal below leaves this replica exactly as it was
    // with nothing to undo.
    auto verified = vdoc::try_verify_batch(_ops, batch);
    if (verified.has_error())
        return cc::make_async_from_error<recovery_result>(cc::async_error::make_error(
            cc::any_error(cc::format("a peer's op was refused because {}", describe(verified.error())))));

    auto pending = pending_graph{.graph = _ops};
    for (auto const& o : verified.value())
        pending.incoming[o.id] = &o;

    // A required snapshot carries no `superseded`, and that is sound only while nothing can present a writer from
    // behind it.
    // Completing its ancestry retires the question entirely — a replay reproduces what the snapshot holds — so the two
    // outcomes are one rule: complete it and the snapshot is demoted, leave it incomplete and nothing may fork below it.
    auto completed = cc::vector<vdoc::op_id>();
    auto still_required = cc::vector<vdoc::op_id>();
    for (auto const& [id, entry] : _snapshots)
    {
        if (!entry.required)
            continue;

        if (ancestry_is_complete(pending, id))
            completed.push_back(id);
        else
            still_required.push_back(id);
    }

    for (auto const& o : verified.value())
    {
        // Filling a skeleton the prune left behind is the case this whole path exists for, and it moves no edge.
        if (_ops.contains(o.id))
            continue;

        for (auto const& head : still_required)
            if (!descends_from(pending, o.id, head))
                return cc::make_async_from_error<recovery_result>(cc::async_error::make_error(cc::any_error(
                    cc::string("a peer's op forks below a snapshot whose history is still pruned, which would leave a "
                               "writer nothing can suppress; send the rest of that snapshot's ancestry with it"))));
    }

    auto job = impl::recovery_job();
    for (auto const& o : verified.value())
    {
        // An op already held in full is already durable, so offering it again would be a write that changes nothing.
        auto const* const existing = _ops.find(o.id);
        if (existing != nullptr && !existing->is_skeleton())
            continue;

        job.ops.push_back(to_row(o));
    }

    cc::sort(job.ops,
             [](op_row const& a, op_row const& b)
             {
                 for (isize i = 0; i < a.hash.size() && i < b.hash.size(); ++i)
                     if (a.hash[i] != b.hash[i])
                         return a.hash[i] < b.hash[i];
                 return a.hash.size() < b.hash.size();
             });

    auto const applied = vdoc::apply_verified_batch(_ops, cc::move(verified.value()));
    job.ops_added = applied.ops_added;
    job.skeletons_filled = applied.skeletons_filled;

    for (auto const& row : job.ops)
        _durable_ops.insert(vdoc::op_id::from_bytes(row.hash));

    for (auto const& head : completed)
    {
        // The cached materialization is still exactly right — it was computed before the prune, over the very ops that
        // just came back — so nothing is recomputed and only its standing changes.
        _snapshots[head].required = false;
        (void)_snapshot_cache.unpin(head);

        auto hash = cc::vector<byte>();
        hash.resize_to_uninitialized(vdoc::op_id::byte_size);
        head.to_bytes(hash);
        job.demoted_snapshots.push_back(cc::move(hash));
    }

    auto async = on_recover(cc::move(job));
    impl_pump_until_idle();
    return async;
}

// resolving assets
// -------------------------------------------------------------------------------------------------

cc::optional<asset_resolution> store::resolve_asset(cc::string_view asset_id)
{
    auto const* record = _assets.get_ptr(asset_id);
    if (record == nullptr)
        return {};

    // One source shared across every resolution, rather than one per asset: they are interchangeable, and a caller
    // resolving many assets should not accumulate many.
    auto source = _shared_source.lock();
    if (!source)
    {
        source = make_blob_source();
        _shared_source = source;
    }
    return asset_resolution{.record = *record, .blobs = cc::move(source)};
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
    return load_range(hash, 0, -1);
}

cc::shared_async<cc::vector<byte>> blob_source::load_range(blob_hash const& hash, i64 offset, i64 size)
{
    // Enqueue-and-return: this never blocks and never re-enters its caller, whatever state the store is in.
    if (_severed)
        return cc::make_async_from_error<cc::vector<byte>>(
            cc::async_error::make_error(cc::any_error(cc::string("the blob source was severed by close()"))));

    return _owner->impl_fetch_blob(hash, {.offset = offset, .size = size});
}

cc::shared_async<cc::vector<byte>> store::impl_fetch_blob(blob_hash const& hash, impl::blob_fetch_range range)
{
    if (_is_closed)
        return cc::make_async_from_error<cc::vector<byte>>(
            cc::async_error::make_error(cc::any_error(cc::string("fetching a blob from a closed store"))));

    // No harvest and NO PUMP, deliberately: both would run work — and with it a caller's continuation — inside the
    // load() call this is serving.
    // See on_fetch_blob.
    return on_fetch_blob(hash, range);
}
} // namespace vdoc::file
