#include "store_fixture.hh"

#include <clean-core/string/format.hh>
#include <versioned-document/op_builder.hh>
#include <versioned-document/value_debug.hh>

namespace vdoc::file::test
{
namespace
{
/// An in-memory medium: the image outlives every store opened over it, which is what makes reopen mean the same
/// thing here as it does on a file.
class memory_medium final : public store_medium
{
public:
    memory_medium() : _image(std::make_shared<memory_image>()) {}

    cc::optional<store_handle> open() override
    {
        // A future format version is refused on both arms, and refusing is the whole behaviour being tested.
        if (_image->user_version > memory_image::current_user_version)
            return {};
        if (_blocked)
            _image->writes_fail = true;

        // Null is this arm's failed open — a required snapshot that will not decode is what produces one.
        auto handle = store::create_in_memory(_image);
        if (handle == nullptr)
            return {};
        return handle;
    }

    cc::vector<byte> snapshot_bytes() override
    {
        // Not a format, just a total ordering of everything stored — enough to say "this did not change".
        auto text = cc::string();
        text += cc::format("v{} a{}\n", _image->user_version, _image->application_id);
        for (auto const& row : _image->ops)
            text += cc::format("op {} {} {} {}\n", hex(row.hash), hex(row.parents),
                               row.metadata.has_value() ? hex(row.metadata.value()) : cc::string("-"),
                               row.assignments.has_value() ? hex(row.assignments.value()) : cc::string("-"));
        for (auto const& row : _image->refs)
            text += cc::format("ref {} {}\n", row.name, hex(row.op_hash));
        for (auto const& row : _image->snapshots)
            text += cc::format("snap {} {} {} {} {} {}\n", hex(row.op_hash), row.required, row.encoding,
                               row.decoded_size, row.stored_size, row.chunk_count);
        for (auto const& row : _image->snapshot_chunks)
            text += cc::format("snap-chunk {} {} {}\n", hex(row.op_hash), row.chunk_index, hex(row.data));
        for (auto const& row : _image->assets)
            text += cc::format("asset {} {} {} {} {}\n", row.asset_id, row.kind, hex(row.parts),
                               row.meta.has_value() ? hex(row.meta.value()) : cc::string("-"),
                               row.deps.has_value() ? hex(row.deps.value()) : cc::string("-"));
        for (auto const& row : _image->blobs)
            text += cc::format("blob {} {} {} {} {}\n", hex(row.hash), row.size, row.stored_size, row.chunk_count,
                               row.encoding);
        for (auto const& row : _image->blob_chunks)
            text += cc::format("chunk {} {} {}\n", row.blob_id, row.chunk_index, hex(row.data));
        for (auto const& row : _image->workspace)
            text += cc::format("ws {} {} {}\n", row.key, row.version, hex(row.value));
        for (auto const& row : _image->meta)
            text += cc::format("meta {}\n", row.key);
        for (auto const& table : _image->unknown_tables)
        {
            text += cc::format("unknown-table {}", table.name);
            for (auto const& column : table.columns)
                text += cc::format(" {}", column);
            text += "\n";
        }
        for (auto const& column : _image->unknown_columns)
            text += cc::format("unknown-column {}\n", column);

        return cc::vector<byte>::create_copy_of(
            cc::span<byte const>(reinterpret_cast<byte const*>(text.data()), text.size()));
    }

    void set_user_version(i32 version) override { _image->user_version = version; }

    void add_unknown_table(cc::string_view name) override
    {
        _image->unknown_tables.push_back({.name = cc::string(name), .columns = {cc::string("id")}});
    }

    void add_unknown_column(cc::string_view table, cc::string_view column) override
    {
        _image->unknown_columns.push_back(cc::format("{}.{}", table, column));
    }

    bool corrupt_first_op_payload() override
    {
        for (auto& row : _image->ops)
            if (row.assignments.has_value() && !row.assignments.value().empty())
            {
                // One byte, inside the payload the id commits to: the row still decodes, and no longer hashes to its id.
                auto& payload = row.assignments.value();
                payload[payload.size() - 1] = byte(u8(payload[payload.size() - 1]) ^ 0xFF);
                return true;
            }
        return false;
    }

    bool corrupt_first_op_structurally() override
    {
        for (auto& row : _image->ops)
            if (row.assignments.has_value())
            {
                // An assignment blob has to start with an encoding tag this build knows; 0xFF never is one.
                row.assignments = cc::vector<byte>::create_defaulted(3);
                row.assignments.value()[0] = byte(0xFF);
                return true;
            }
        return false;
    }

    void block_writes() override
    {
        _blocked = true;
        _image->writes_fail = true;
    }

    void unblock_writes() override
    {
        _blocked = false;
        _image->writes_fail = false;
    }

    bool delete_first_blob_chunk() override
    {
        auto const id = first_blob_id();
        if (!id.has_value())
            return false;

        // The LAST chunk of that blob, so a torn blob is short at its end rather than holed at its start — which is
        // the shape a write interrupted partway through actually leaves.
        auto best = isize(-1);
        auto best_index = i64(-1);
        for (isize i = 0; i < _image->blob_chunks.size(); ++i)
            if (_image->blob_chunks[i].blob_id == id.value() && _image->blob_chunks[i].chunk_index > best_index)
            {
                best = i;
                best_index = _image->blob_chunks[i].chunk_index;
            }
        if (best < 0)
            return false;

        auto kept = cc::vector<chunk_row>();
        for (isize i = 0; i < _image->blob_chunks.size(); ++i)
            if (i != best)
                kept.push_back(cc::move(_image->blob_chunks[i]));
        _image->blob_chunks = cc::move(kept);
        return true;
    }

    bool set_first_blob_encoding(cc::string_view encoding) override
    {
        auto const id = first_blob_id();
        if (!id.has_value())
            return false;

        for (auto& row : _image->blobs)
            if (row.id == id.value())
            {
                row.encoding = cc::string(encoding);
                return true;
            }
        return false;
    }

    bool corrupt_first_asset_deps() override
    {
        for (auto& row : _image->assets)
            if (row.deps.has_value())
            {
                // A vdoc value has to start with an encoding tag this build knows; 0xFF never is one.
                row.deps = cc::vector<byte>::create_defaulted(3);
                row.deps.value()[0] = byte(0xFF);
                return true;
            }
        return false;
    }

    isize count_blobs() override { return _image->blobs.size(); }

    isize count_snapshots() override { return _image->snapshots.size(); }

    bool delete_first_snapshot() override
    {
        auto const key = first_snapshot_key();
        if (!key.has_value())
            return false;

        auto kept = cc::vector<snapshot_row>();
        for (auto& row : _image->snapshots)
            if (!same_hash(row.op_hash, key.value()))
                kept.push_back(cc::move(row));
        _image->snapshots = cc::move(kept);

        // The file arm gets this from ON DELETE CASCADE; here it is spelled out so both arms leave the same state.
        auto kept_chunks = cc::vector<snapshot_chunk_row>();
        for (auto& row : _image->snapshot_chunks)
            if (!same_hash(row.op_hash, key.value()))
                kept_chunks.push_back(cc::move(row));
        _image->snapshot_chunks = cc::move(kept_chunks);
        return true;
    }

    bool corrupt_first_snapshot_payload() override
    {
        auto const key = first_snapshot_key();
        if (!key.has_value())
            return false;

        // The row keeps saying what it stores, so this is a payload that will not decode rather than a short one.
        for (auto& row : _image->snapshot_chunks)
            if (same_hash(row.op_hash, key.value()) && row.chunk_index == 0 && !row.data.empty())
            {
                for (auto& b : row.data)
                    b = byte(0xFF);
                return true;
            }
        return false;
    }

    bool set_first_snapshot_required(bool required) override
    {
        auto const key = first_snapshot_key();
        if (!key.has_value())
            return false;

        for (auto& row : _image->snapshots)
            if (same_hash(row.op_hash, key.value()))
            {
                row.required = required ? 1 : 0;
                return true;
            }
        return false;
    }

    bool set_first_snapshot_encoding(cc::string_view encoding) override
    {
        auto const key = first_snapshot_key();
        if (!key.has_value())
            return false;

        for (auto& row : _image->snapshots)
            if (same_hash(row.op_hash, key.value()))
            {
                row.encoding = cc::string(encoding);
                return true;
            }
        return false;
    }

    bool first_snapshot_is_required() override
    {
        auto const key = first_snapshot_key();
        if (!key.has_value())
            return false;

        for (auto const& row : _image->snapshots)
            if (same_hash(row.op_hash, key.value()))
                return row.required != 0;
        return false;
    }

private:
    static bool same_hash(cc::span<byte const> a, cc::span<byte const> b)
    {
        if (a.size() != b.size())
            return false;
        for (isize i = 0; i < a.size(); ++i)
            if (a[i] != b[i])
                return false;
        return true;
    }

    /// Lowest key by bytes, so both arms damage the same snapshot.
    cc::optional<cc::vector<byte>> first_snapshot_key() const
    {
        auto lowest = cc::optional<cc::vector<byte>>();
        for (auto const& row : _image->snapshots)
            if (!lowest.has_value() || hex(row.op_hash) < hex(lowest.value()))
                lowest = cc::vector<byte>::create_copy_of(cc::span<byte const>(row.op_hash));
        return lowest;
    }

    /// Lowest id, which is the first blob written — the same choice the file arm makes.
    cc::optional<i64> first_blob_id() const
    {
        auto lowest = cc::optional<i64>();
        for (auto const& row : _image->blobs)
            if (!lowest.has_value() || row.id < lowest.value())
                lowest = row.id;
        return lowest;
    }

    static cc::string hex(cc::span<byte const> bytes)
    {
        auto out = cc::string();
        for (auto const b : bytes)
            out += cc::format("{:02x}", u8(b));
        return out;
    }

    std::shared_ptr<memory_image> _image;
    bool _blocked = false;
};
} // namespace

store_impl in_memory_impl()
{
    return {.name = "in-memory",
            .make_medium = []() -> std::unique_ptr<store_medium> { return std::make_unique<memory_medium>(); },
            .is_available = [] { return true; }};
}

// ---- helpers ------------------------------------------------------------------------------------

sample_history make_sample_history()
{
    auto history = sample_history();

    auto const wall = vdoc::entity_id::of("wall-17");
    auto const transform = vdoc::component_type_id::of("transform");

    auto first = vdoc::op_builder();
    first.set_raw(wall, transform, vdoc::property_id::of("x"), vdoc::value::of(1.0));
    first.set_raw(wall, transform, vdoc::property_id::of("y"), vdoc::value::of(2.0));
    history.ops.push_back(history.graph.add(first.build(history.graph)));

    auto second = vdoc::op_builder();
    second.set_parents(cc::span<vdoc::op_id const>(history.ops).last_n(1));
    second.set_raw(wall, transform, vdoc::property_id::of("x"), vdoc::value::of(10.0));
    history.ops.push_back(history.graph.add(second.build(history.graph)));

    auto third = vdoc::op_builder();
    third.set_parents(cc::span<vdoc::op_id const>(history.ops).last_n(1));
    third.set_raw(wall, transform, vdoc::property_id::of("label"), vdoc::value::of("north"));
    history.ops.push_back(history.graph.add(third.build(history.graph)));

    return history;
}

sample_history make_linear_history(isize length)
{
    auto history = sample_history();

    auto const wall = vdoc::entity_id::of("wall-17");
    auto const transform = vdoc::component_type_id::of("transform");

    for (isize i = 0; i < length; ++i)
    {
        auto op = vdoc::op_builder();
        if (!history.ops.empty())
            op.set_parents(cc::span<vdoc::op_id const>(history.ops).last_n(1));

        // Every op overwrites x, so all but the last writer is superseded — which is what makes pruning observable.
        op.set_raw(wall, transform, vdoc::property_id::of("x"), vdoc::value::of(f64(i)));
        op.set_raw(wall, transform, vdoc::property_id::of(cc::format("p{}", i)), vdoc::value::of(i64(i)));
        history.ops.push_back(history.graph.add(op.build(history.graph)));
    }

    return history;
}

vdoc::op_id add_branch(vdoc::op_graph& graph, vdoc::op_id const& after, cc::string_view marker)
{
    auto branch = vdoc::op_builder();
    branch.set_parents(cc::span<vdoc::op_id const>(&after, 1));
    branch.set_raw(vdoc::entity_id::of("wall-17"), vdoc::component_type_id::of("transform"),
                   vdoc::property_id::of("branch"), vdoc::value::of(marker));
    return graph.add(branch.build(graph));
}

void copy_ops_into(store& target, vdoc::op_graph const& source, cc::span<vdoc::op_id const> ids)
{
    for (auto const& id : ids)
        if (auto const* op = source.find(id))
            target.add_op(*op);
}

cc::string materialize_to_text(vdoc::op_graph const& graph, vdoc::op_id const& head)
{
    auto const raw = graph.materialize(head);

    auto text = cc::string();
    for (auto const& entity : raw.entities)
        for (auto const& component : entity.value.components)
            for (auto const& property : component.value.properties)
                for (auto const& writer : property.value.writers)
                    text += cc::format("{}/{}/{} = {}\n", entity.entity.as_string_view(),
                                       component.component.as_string_view(), property.property.as_string_view(),
                                       vdoc::to_debug_string(writer.value));
    return text;
}
} // namespace vdoc::file::test
