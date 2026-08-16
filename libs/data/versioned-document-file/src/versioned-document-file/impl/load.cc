#include <clean-core/string/format.hh>
#include <versioned-document-file/impl/payload_codec.hh>
#include <versioned-document-file/impl/snapshot_codec.hh>
#include <versioned-document-file/impl/store_io.hh>
#include <versioned-document-file/store.hh>
#include <versioned-document/op.hh>
#include <versioned-document/value.hh>

/// The one loader, over a store_reader.
///
/// In the order [the format](../../../docs/format.md#loading) gives, and metadata before payloads.
/// Every op goes through try_decode_op, which verifies as it goes, and there is no other route.
///
/// **Soft failures never block a load.** A corrupt op is dropped and reported, landing on exactly the same downstream
/// path as a pruned one — which is what makes that path get exercised rather than rot until the day it is needed.

namespace vdoc::file::impl
{
namespace
{
/// What a load learned about one blob without reading a payload.
struct blob_facts
{
    bool is_usable = false; // false where the encoding is unknown or the chunks do not add up
};

void report_issue(load_report& report, load_issue issue)
{
    report.issues.push_back(cc::move(issue));
}

/// Reads a 32-byte id out of a stored blob column, or nothing if the column is not that shape.
cc::optional<vdoc::op_id> try_read_op_id(cc::span<byte const> bytes)
{
    if (bytes.size() != vdoc::op_id::byte_size)
        return {};
    return vdoc::op_id::from_bytes(bytes);
}

cc::optional<blob_hash> try_read_blob_hash(cc::span<byte const> bytes)
{
    if (bytes.size() != blob_hash::byte_size)
        return {};
    return blob_hash::from_bytes(bytes);
}

/// Splits the concatenated parent ids.
/// The column carries no count, so a length that is not a whole number of ids is a decode error.
cc::optional<cc::vector<vdoc::op_id>> try_read_parents(cc::span<byte const> bytes)
{
    if (bytes.size() % vdoc::op_id::byte_size != 0)
        return {};

    auto parents = cc::vector<vdoc::op_id>();
    for (isize at = 0; at < bytes.size(); at += vdoc::op_id::byte_size)
        parents.push_back(vdoc::op_id::from_bytes(bytes.subspan({.offset = at, .size = vdoc::op_id::byte_size})));
    return parents;
}

/// Reads one part object out of an asset's `parts` array.
/// The name is optional and debug-only; the hash and the format are what the entry means.
cc::optional<asset_part> try_read_part(vdoc::value_view entry)
{
    if (entry.kind() != vdoc::value_kind::object)
        return {};

    auto const hash_field = entry.try_find("hash");
    auto const format_field = entry.try_find("format");
    if (!hash_field.has_value() || hash_field.value().kind() != vdoc::value_kind::bytes)
        return {};
    if (!format_field.has_value() || format_field.value().kind() != vdoc::value_kind::string)
        return {};

    auto const hash = try_read_blob_hash(hash_field.value().as_bytes());
    if (!hash.has_value())
        return {};

    auto part = asset_part{.hash = hash.value(), .format = cc::string(format_field.value().as_string())};
    if (auto const name_field = entry.try_find("name");
        name_field.has_value() && name_field.value().kind() == vdoc::value_kind::string)
        part.name = cc::string(name_field.value().as_string());
    return part;
}
} // namespace

cc::result<cc::unit> load(store_reader& reader, store& target)
{
    // load is the store's friend, so the report is reached here and passed on as a plain reference.
    auto& report = target._report;

    // 1. Whatever the reader had to do to get here is done; the unknown shapes it found are reported first, so an
    //    issue list read top-down starts with what this build did not understand at all.
    for (auto const& name : reader.unknown_tables())
        report_issue(report, {.kind = load_issue_kind::unknown_table, .name = cc::string(name)});
    for (auto const& name : reader.unknown_columns())
        report_issue(report, {.kind = load_issue_kind::unknown_column, .name = cc::string(name)});

    // 2. Blobs, METADATA ONLY — never a payload, which is what keeps opening a multi-gigabyte file cheap.
    auto blobs = reader.read_blobs();
    CC_RETURN_IF_ERROR(blobs);
    auto summaries = reader.read_chunk_summaries();
    CC_RETURN_IF_ERROR(summaries);

    auto chunk_counts = cc::map<i64, chunk_summary>();
    for (auto const& s : summaries.value())
        chunk_counts[s.blob_id] = s;

    auto blob_state = cc::map<blob_hash, blob_facts>();
    for (auto const& row : blobs.value())
    {
        auto const hash = try_read_blob_hash(row.hash);
        if (!hash.has_value())
            continue; // a hash column that is not 32 bytes names no blob any asset can reach

        // An encoding this build has no codec for is a load issue and the blob is skipped, never a failed open.
        // Note what the `continue` skips: the blob does NOT join _durable_blobs, so a republish rewrites it rather
        // than trusting a row this build cannot read.
        if (find_payload_codec(row.encoding) == nullptr)
        {
            report_issue(report, {.kind = load_issue_kind::unknown_encoding, .blob = hash.value(), .name = row.encoding});
            blob_state[hash.value()] = {.is_usable = false};
            continue;
        }

        // A torn write is visible rather than silent: the row says what must be there, and the chunks either add up or they do not.
        // There is no nullable payload column that could read as "present".
        auto const* summary = chunk_counts.get_ptr(row.id);
        auto const count = summary != nullptr ? summary->count : 0;
        auto const bytes = summary != nullptr ? summary->total_bytes : 0;
        blob_state[hash.value()] = {.is_usable = count == row.chunk_count && bytes == row.stored_size};

        target._durable_blobs.insert(hash.value());
    }

    // 3. Assets, filling in the blob-side facts from step 2.
    auto assets = reader.read_assets();
    CC_RETURN_IF_ERROR(assets);
    for (auto const& row : assets.value())
    {
        auto const decoded = vdoc::try_decode(row.parts);
        if (decoded.has_error() || decoded.value().kind() != vdoc::value_kind::array)
        {
            report_issue(report, {.kind = load_issue_kind::asset_decode_failed, .name = row.asset_id});
            continue;
        }

        auto record = asset_record{.asset_id = row.asset_id, .kind = row.kind};
        auto const parts = decoded.value();
        auto parts_are_sound = true;
        for (isize i = 0; i < parts.size(); ++i)
        {
            auto part = try_read_part(parts.element_at(i));
            if (!part.has_value())
            {
                parts_are_sound = false;
                break;
            }
            record.parts.push_back(cc::move(part.value()));
        }
        if (!parts_are_sound)
        {
            report_issue(report, {.kind = load_issue_kind::asset_decode_failed, .name = row.asset_id});
            continue;
        }

        if (row.meta.has_value())
        {
            auto const meta = vdoc::try_decode(row.meta.value());
            if (meta.has_error())
            {
                report_issue(report, {.kind = load_issue_kind::asset_decode_failed, .name = row.asset_id});
                continue;
            }
            record.meta = vdoc::value::from_validated_bytes(row.meta.value());
        }

        if (row.deps.has_value())
        {
            // A dependency list is advisory, so NOTHING here drops the asset — an unreadable list is reported and
            // leaves the asset with no declared dependencies, exactly as an absent one would.
            // The cost of that is a reclamation that collects more than it had to, never a wrong idea of what the
            // asset IS.
            // Dropping the asset instead would turn an advisory field into a load-bearing one.
            auto const deps = vdoc::try_decode(row.deps.value());
            if (deps.has_error() || deps.value().kind() != vdoc::value_kind::array)
            {
                report_issue(report, {.kind = load_issue_kind::asset_decode_failed, .name = row.asset_id});
            }
            else
            {
                // One entry that is not a string is dropped, for the same reason and at the same cost.
                auto const declared = deps.value();
                for (isize i = 0; i < declared.size(); ++i)
                    if (auto const entry = declared.element_at(i); entry.kind() == vdoc::value_kind::string)
                        record.dependencies.push_back(cc::string(entry.as_string()));
            }
        }

        // Both of these KEEP every part: the report names them at open and the lookup names them at use, and dropping
        // one here would make asset_record::try_find_part's `ambiguous` unreachable.
        auto has_unnamed = false;
        auto has_duplicate = false;
        for (isize i = 0; i < record.parts.size(); ++i)
        {
            if (record.parts[i].name.empty())
                has_unnamed = true;
            for (isize j = i + 1; j < record.parts.size(); ++j)
                if (record.parts[i].name == record.parts[j].name)
                    has_duplicate = true;
        }
        // Once per asset rather than once per part: a naive publish leaves every part on the default name, and one
        // issue per part would bury the rest of the report.
        if (has_unnamed)
            report_issue(report, {.kind = load_issue_kind::asset_part_unnamed, .name = row.asset_id});
        if (has_duplicate)
            report_issue(report, {.kind = load_issue_kind::asset_duplicate_part_name, .name = row.asset_id});

        for (auto const& part : record.parts)
        {
            auto const* facts = blob_state.get_ptr(part.hash);
            if (facts == nullptr)
            {
                report_issue(report,
                             {.kind = load_issue_kind::asset_blob_missing, .blob = part.hash, .name = row.asset_id});
                record.is_resolvable = false;
            }
            else if (!facts->is_usable)
            {
                report_issue(report,
                             {.kind = load_issue_kind::asset_blob_incomplete, .blob = part.hash, .name = row.asset_id});
                record.is_resolvable = false;
            }
        }

        target._assets[record.asset_id] = cc::move(record);
    }

    // 4. Ops, decoding and VERIFYING every one.
    auto ops = reader.read_ops();
    CC_RETURN_IF_ERROR(ops);
    for (auto const& row : ops.value())
    {
        auto const id = try_read_op_id(row.hash);
        if (!id.has_value())
            continue; // a key that is not an op id names no op, so there is nothing to report it against

        auto const parents = try_read_parents(row.parents);
        if (!parents.has_value())
        {
            report_issue(report, {.kind = load_issue_kind::op_decode_failed, .op = id.value()});
            continue;
        }

        // Both payload columns NULL is a skeleton; exactly one is a row that was never a valid op.
        auto const has_metadata = row.metadata.has_value();
        auto const has_assignments = row.assignments.has_value();
        if (has_metadata != has_assignments)
        {
            report_issue(report, {.kind = load_issue_kind::op_decode_failed, .op = id.value()});
            continue;
        }

        auto decoded = has_metadata
                         ? vdoc::try_decode_op(id.value(), parents.value(), row.metadata.value(), row.assignments.value())
                         : vdoc::try_decode_skeleton_op(id.value(), parents.value());
        if (decoded.has_error())
        {
            // A mismatch is corruption or tampering; everything else is bytes that never were an op.
            // The two are never merged, because only one of them is a claim about integrity.
            auto const kind = decoded.error() == vdoc::op_decode_error::hash_mismatch
                                ? load_issue_kind::op_hash_mismatch
                                : load_issue_kind::op_decode_failed;
            report_issue(report, {.kind = kind, .op = id.value()});
            continue;
        }

        target._durable_ops.insert(id.value());
        target._ops.add(cc::move(decoded.value()));
    }

    // 5. Missing parents, once every op is in.
    //    Informational, and normal after pruning.
    for (auto const& row : ops.value())
    {
        auto const id = try_read_op_id(row.hash);
        if (!id.has_value())
            continue;
        auto const* op = target._ops.find(id.value());
        if (op == nullptr)
            continue;

        for (auto const& parent : op->parents)
            if (!target._ops.contains(parent))
                report_issue(report, {.kind = load_issue_kind::missing_parent, .op = id.value(), .parent = parent});
    }

    // 6. Refs, kept verbatim — including one whose op this load dropped.
    //    Discarding somebody's ref is not a loader's decision.
    auto refs = reader.read_refs();
    CC_RETURN_IF_ERROR(refs);
    for (auto const& row : refs.value())
    {
        auto const id = try_read_op_id(row.op_hash);
        if (!id.has_value())
        {
            report_issue(report, {.kind = load_issue_kind::dangling_ref, .name = row.name});
            continue;
        }

        target._refs[row.name] = id.value();
        if (!target._ops.contains(id.value()))
            report_issue(report, {.kind = load_issue_kind::dangling_ref, .op = id.value(), .name = row.name});
    }

    // 7. Snapshots, decoded into the materialization cache.
    //
    //    This is the ONE place where severity is decided by the `required` flag rather than by the kind of failure.
    //    A droppable snapshot that will not decode costs speed and nothing else, so it is an issue.
    //    A required one stood in for history that has been pruned away, so it is a hard failure and the open does not
    //    complete — there is no correct document to hand back.
    auto snapshots = reader.read_snapshots();
    CC_RETURN_IF_ERROR(snapshots);
    for (auto const& row : snapshots.value())
    {
        auto const id = try_read_op_id(row.op_hash);
        if (!id.has_value())
        {
            if (row.required != 0)
                return cc::error(cc::any_error(cc::string("a required snapshot is keyed on something that is not an "
                                                          "op id, and the history behind it has been pruned")));

            // No op to name, because the key itself is not an id.
            report_issue(report, {.kind = load_issue_kind::missing_snapshot});
            continue;
        }

        auto entry = snapshot_entry{.op = id.value(),
                                    .required = row.required != 0,
                                    .encoding = row.encoding,
                                    .decoded_size = row.decoded_size};

        auto const fail_or_report = [&](load_issue_kind kind, cc::string_view what) -> cc::result<cc::unit>
        {
            if (row.required != 0)
                return cc::error(cc::any_error(
                    cc::format("a required snapshot {}, and the history behind it has been pruned", what)));

            report_issue(report, {.kind = kind, .op = id.value()});
            return cc::unit{};
        };

        auto const* const codec = find_payload_codec(row.encoding);
        if (codec == nullptr)
        {
            // The row stays on disk untouched, because publishing only ever inserts — so a snapshot a newer build
            // wrote survives this one opening and saving the file.
            CC_RETURN_IF_ERROR(fail_or_report(load_issue_kind::unknown_encoding, "names an encoding this build has no "
                                                                                 "codec for"));
            target._snapshots[id.value()] = cc::move(entry);
            continue;
        }

        auto chunks = reader.read_snapshot_chunks(row.op_hash);
        CC_RETURN_IF_ERROR(chunks);

        auto stored = cc::vector<byte>();
        for (auto const& chunk : chunks.value())
            for (auto const b : chunk.data)
                stored.push_back(b);

        if (isize(chunks.value().size()) != row.chunk_count || stored.size() != row.stored_size)
        {
            CC_RETURN_IF_ERROR(fail_or_report(load_issue_kind::missing_snapshot, "is missing chunks"));
            target._snapshots[id.value()] = cc::move(entry);
            continue;
        }

        auto decoded_bytes = codec->decode(cc::move(stored), row.decoded_size);
        if (decoded_bytes.has_error())
        {
            CC_RETURN_IF_ERROR(fail_or_report(load_issue_kind::missing_snapshot, "will not decode"));
            target._snapshots[id.value()] = cc::move(entry);
            continue;
        }

        auto snapshot = try_decode_snapshot(snapshot_encoding_v1, decoded_bytes.value());
        if (snapshot.has_error())
        {
            CC_RETURN_IF_ERROR(fail_or_report(load_issue_kind::missing_snapshot, "will not decode"));
            target._snapshots[id.value()] = cc::move(entry);
            continue;
        }

        // A required snapshot is PINNED: it is not a cache, and shedding memory must not be able to destroy the only
        // record of the history it replaced.
        entry.decoded = true;
        target._snapshot_cache.install(id.value(), cc::move(snapshot.value()), entry.required);
        target._snapshots[id.value()] = cc::move(entry);
    }

    // 8. Workspace.
    //    A row that will not decode is not exposed and is LEFT IN PLACE, which costs nothing because only dirty keys are ever written.
    auto workspace = reader.read_workspace();
    CC_RETURN_IF_ERROR(workspace);
    for (auto const& row : workspace.value())
    {
        if (vdoc::try_decode(row.value).has_error())
        {
            report_issue(report, {.kind = load_issue_kind::workspace_decode_failed, .name = row.key});
            continue;
        }
        target._workspace[row.key]
            = workspace_value{.version = i32(row.version), .value = vdoc::value::from_validated_bytes(row.value)};
    }

    // 9. Meta.
    //    Unknown keys survive by never being rewritten.
    auto meta = reader.read_meta();
    CC_RETURN_IF_ERROR(meta);
    for (auto const& row : meta.value())
    {
        if (!row.value.has_value() || vdoc::try_decode(row.value.value()).has_error())
            continue;
        target._meta[row.key] = vdoc::value::from_validated_bytes(row.value.value());
    }

    return cc::unit{};
}
} // namespace vdoc::file::impl
