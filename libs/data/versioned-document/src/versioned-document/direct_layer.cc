#include "direct_layer.hh"

#include <clean-core/algorithm/search.hh>
#include <clean-core/algorithm/sort.hh>
#include <clean-core/bytes/blake3.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/common/endian.hh>

using namespace cc::primitive_defines;

namespace
{
// A different preimage space from an op's, so a synthetic writer id cannot collide with a real one.
// The same reasoning "vdoc::op/v1" itself rests on.
constexpr cc::string_view layer_domain = "vdoc::layer/v1";

void hash_length_prefixed(cc::blake3& hasher, cc::span<byte const> bytes)
{
    byte length_field[8] = {};
    cc::store_bytes_le<u64>(length_field, 0, u64(bytes.size()));
    hasher.update(length_field);
    hasher.update(bytes);
}

[[nodiscard]] cc::span<byte const> as_bytes(cc::string_view chars)
{
    return cc::span<byte const>(reinterpret_cast<byte const*>(chars.data()), chars.size());
}
} // namespace

vdoc::op_id vdoc::synthetic_writer_id(cc::string_view layer_name)
{
    auto hasher = cc::blake3();
    hash_length_prefixed(hasher, as_bytes(layer_domain));
    hash_length_prefixed(hasher, as_bytes(layer_name));
    return op_id(hasher.finalize());
}

vdoc::direct_layer::direct_layer(cc::string_view name) : _name(name), _writer(synthetic_writer_id(name))
{
}

void vdoc::direct_layer::set(property_path const& path, value_view v)
{
    impl_note_written(path);

    // One walk that compares and writes, rather than a lookup followed by a write.
    auto inserted = false;
    if (!_doc.set_single_writer_if_changed(path, _writer, v.bytes(), &inserted))
        return;

    _inserted_during_rebuild = _inserted_during_rebuild || inserted;
    _changes.add(path);
    ++_version;
}

void vdoc::direct_layer::abstain(property_path const& path)
{
    impl_note_written(path);
    impl_withdraw(path);
}

void vdoc::direct_layer::mark_dirty(property_path const& path)
{
    _changes.add(path);
    ++_version;
}

void vdoc::direct_layer::begin_rebuild()
{
    CC_ASSERT(!_rebuilding, "a rebuild is already open - begin_rebuild does not nest");
    _rebuilding = true;
    _inserted_during_rebuild = false;
    _written_this_rebuild.clear();
}

void vdoc::direct_layer::finish_rebuild()
{
    CC_ASSERT(_rebuilding, "no rebuild is open");
    _rebuilding = false;

    // The steady-state frame, and the only one that has to be fast: nothing was inserted, and the pass wrote exactly as
    // many paths as the layer holds — so every stored path was written once and none can be stale.
    //
    // Without this the sweep below sorts and then searches one entry per property, every frame, with a multi-level
    // string comparison at each step.
    // At 8,000 entities that was the whole frame.
    if (!_inserted_during_rebuild && _written_this_rebuild.size() == _doc.property_count())
    {
        _written_this_rebuild.clear();
        return;
    }

    cc::sort(_written_this_rebuild, property_path::by_bytes{});

    // Collect first, then withdraw: withdrawing prunes the entries this would otherwise be walking.
    auto stale = cc::vector<property_path>();
    for (auto const& e : _doc.document().entities)
        for (auto const& c : e.value.components)
            for (auto const& p : c.value.properties)
            {
                auto const path = property_path{.entity = e.entity, .component = c.component, .property = p.property};

                if (!cc::find_in_sorted(_written_this_rebuild, path, property_path::by_bytes{}).has_value())
                    stale.push_back(path);
            }

    for (auto const& path : stale)
        impl_withdraw(path);

    _written_this_rebuild.clear();
}

void vdoc::direct_layer::clear()
{
    CC_ASSERT(!_rebuilding, "clear during a rebuild is a contradiction - finish_rebuild first");

    for (auto const& e : _doc.document().entities)
        for (auto const& c : e.value.components)
            for (auto const& p : c.value.properties)
                _changes.add({.entity = e.entity, .component = c.component, .property = p.property});

    _doc = snapshot_document();
    ++_version;
}

vdoc::change_set vdoc::direct_layer::impl_take_changes()
{
    auto out = cc::move(_changes).build();
    _changes = change_set_builder();
    return out;
}

void vdoc::direct_layer::impl_note_written(property_path const& path)
{
    if (_rebuilding)
        _written_this_rebuild.push_back(path);
}

void vdoc::direct_layer::impl_withdraw(property_path const& path)
{
    if (_doc.document().try_get(path) == nullptr)
        return;

    _doc.clear_writers(path);
    _changes.add(path);
    ++_version;
}
