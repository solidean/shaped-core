#include <clean-core/common/assert.hh>
#include <shaped-graphics/binding/staging_binding_group.hh>
#include <shaped-graphics/exceptions.hh>

namespace sg
{
staging_binding_group::~staging_binding_group() = default;

staging_binding_group::staging_binding_group(binding_group_layout_handle layout, cc::vector<int> descriptor_offsets)
  : _layout(cc::move(layout))
{
    CC_ASSERT(_layout != nullptr, "staging_binding_group requires a binding_group_layout");

    auto const bindings = _layout->bindings();
    CC_ASSERT(descriptor_offsets.size() == bindings.size(), "one descriptor offset per layout binding");

    for (isize i = 0; i < bindings.size(); ++i)
    {
        auto const& b = bindings[i];
        _slot_by_name[b.name] = int(i);
        _slots.push_back({.declared = &b, .first_descriptor = descriptor_offsets[i], .touched = false});

        // A static sampler is the one binding that cannot be set: it lives in the root signature, so there is
        // no descriptor here and nothing for snapshot() to demand.
        if (descriptor_offsets[i] >= 0)
            ++_untouched;
    }
}

binding_slot staging_binding_group::slot_of(cc::string_view name) const
{
    int const slot = _slot_by_name.get_or(name, -1);
    if (slot < 0)
        return binding_slot::invalid;
    return binding_slot(u32(slot));
}

binding_slot staging_binding_group::checked_slot_of(cc::string_view name) const
{
    auto const slot = slot_of(name);
    CC_ASSERT(slot != binding_slot::invalid, "staging_binding_group: no binding of that name in the layout");
    return slot;
}

staging_binding_group::slot_info const& staging_binding_group::info_of(binding_slot slot) const
{
    CC_ASSERT(slot != binding_slot::invalid, "staging_binding_group: unresolved slot");
    CC_ASSERT(isize(u32(slot)) < _slots.size(), "staging_binding_group: slot is not from this group");
    return _slots[isize(u32(slot))];
}

bool staging_binding_group::is_array(binding_slot slot) const
{
    return info_of(slot).declared->is_array();
}

int staging_binding_group::array_size(binding_slot slot) const
{
    return int(info_of(slot).declared->count);
}

void staging_binding_group::touch(binding_slot slot)
{
    auto& info = _slots[isize(u32(slot))];
    if (info.touched)
        return;
    info.touched = true;
    --_untouched;
}

void staging_binding_group::set_binding(binding_slot slot, raw_view const& view)
{
    // A scalar binding has no unset: an empty one is a value it can be set to (the null acceleration
    // structure), never an absence, so the vacant marker — which means "no element here" — has no meaning.
    CC_ASSERT(!is_vacant(view), "staging_binding_group: a scalar binding takes a view, not the vacant marker");
    CC_ASSERT(!is_sampler(info_of(slot).declared->type), "staging_binding_group: that binding is a sampler — use "
                                                         "set_sampler");
    CC_ASSERT(!is_array(slot), "staging_binding_group: that binding is an array — use set_array_element / "
                               "set_array_range / set_array");
    write_run(slot, 0, cc::span<raw_view const>(&view, 1));
    touch(slot);
}

void staging_binding_group::set_array_element(binding_slot slot, int element, raw_view const& view)
{
    CC_ASSERT(!is_vacant(view), "staging_binding_group: set_array_element binds a resource — use unset_array_element "
                                "to clear it");
    CC_ASSERT(is_array(slot), "staging_binding_group: that binding is not an array — use set_binding");
    write_run(slot, element, cc::span<raw_view const>(&view, 1));
    touch(slot);
}

void staging_binding_group::unset_array_element(binding_slot slot, int element)
{
    CC_ASSERT(is_array(slot), "staging_binding_group: that binding is not an array — a scalar binding has no unset, "
                              "it is set to another view");
    clear_run(slot, element, 1);
    touch(slot);
}

void staging_binding_group::set_array_range(binding_slot slot, int first_element, cc::span<raw_view const> views)
{
    CC_ASSERT(is_array(slot), "staging_binding_group: that binding is not an array — use set_binding");
    write_run(slot, first_element, views);
    touch(slot); // an empty run writes nothing and still says "this binding is deliberately as it is"
}

void staging_binding_group::unset_array_range(binding_slot slot, int first_element, int count)
{
    CC_ASSERT(is_array(slot), "staging_binding_group: that binding is not an array — a scalar binding has no unset, "
                              "it is set to another view");
    clear_run(slot, first_element, count);
    touch(slot);
}

void staging_binding_group::set_array(binding_slot slot, cc::span<raw_view const> views)
{
    set_array(slot, 0, views);
}

void staging_binding_group::set_array(binding_slot slot, int first_element, cc::span<raw_view const> views)
{
    // A replacement, not a patch: whatever the run does not cover is cleared, so the array afterwards holds exactly these views.
    // That is the difference from set_array_range.
    CC_ASSERT(is_array(slot), "staging_binding_group: that binding is not an array — use set_binding");
    int const size = array_size(slot);
    int const after = first_element + int(views.size());

    // Three runs at most — the head cleared, the run written, the tail cleared — so a full replacement is three
    // calls into the backend however long the array is.
    clear_run(slot, 0, first_element);
    write_run(slot, first_element, views);
    clear_run(slot, after, size - after);
    touch(slot);
}

void staging_binding_group::unset_array(binding_slot slot)
{
    set_array(slot, 0, {});
}

void staging_binding_group::set_sampler(binding_slot slot, sampler const& smp)
{
    auto const& info = info_of(slot);
    CC_ASSERT(is_sampler(info.declared->type), "staging_binding_group: that binding is not a sampler");
    CC_ASSERT(info.first_descriptor >= 0, "staging_binding_group: that sampler is static — it lives in the root "
                                          "signature, not in the group");
    CC_ASSERT(info.declared->count == 1, "dynamic sampler arrays are not supported yet");

    write_sampler_descriptor(info.first_descriptor, smp);
    _dirty = true;
    touch(slot);
}

void staging_binding_group::unset_sampler(binding_slot slot)
{
    set_sampler(slot, sampler{});
}

binding_group_handle staging_binding_group::snapshot()
{
    auto r = try_snapshot();
    if (r.has_value())
        return cc::move(r.value());
    throw binding_group_exception(r.error());
}

cc::result<binding_group_handle> staging_binding_group::try_snapshot()
{
    CC_ASSERT(_untouched == 0, "staging_binding_group: a binding was never set — say what it holds, even if that is "
                               "nothing (unset_array on a bindless table)");
    if (!_dirty && _snapshot != nullptr)
        return _snapshot;

    auto r = mint();
    CC_RETURN_IF_ERROR(r);
    _snapshot = cc::move(r.value());
    _dirty = false;
    return _snapshot;
}

void staging_binding_group::write_run(binding_slot slot, int first_element, cc::span<raw_view const> views)
{
    auto const& info = info_of(slot);
    auto const& b = *info.declared;
    CC_ASSERT(!is_sampler(b.type), "staging_binding_group: that binding is a sampler — it takes no view");
    CC_ASSERT(info.first_descriptor >= 0, "staging_binding_group: that binding has no view descriptor");
    CC_ASSERT(first_element >= 0 && first_element + views.size() <= isize(b.count),
              "staging_binding_group: the run does not fit inside the binding's element count");
    if (views.empty())
        return;

    for (auto const& view : views)
    {
        CC_ASSERT(accepts(b.type, view), "staging_binding_group: the bound view does not match the binding's declared "
                                         "kind");

        // A bound view always carries a resource, since an empty element is cleared rather than bound as a null handle.
        // Visited by arm, so a new raw_view arm has to answer this question rather than fall into a default.
        view.visit([](raw_buffer_view const& bv)
                   { CC_ASSERT(bv.buffer != nullptr, "staging_binding_group: a buffer view must bind a buffer"); },
                   [](raw_texture_view const& tv)
                   { CC_ASSERT(tv.texture != nullptr, "staging_binding_group: a texture view must bind a texture"); },
                   [](raw_tlas_view const&) {}, // a null tlas IS a resource: the acceleration structure every ray misses
                   [](vacant_view const&)
                   {
                       CC_ASSERT(false,
                                 "staging_binding_group: a set binds a view — clear an element through the unset_* "
                                 "family instead");
                   });
    }

    write_view_descriptors(info.first_descriptor + first_element, b, views);
    _dirty = true;
}

void staging_binding_group::clear_run(binding_slot slot, int first_element, int count)
{
    auto const& info = info_of(slot);
    auto const& b = *info.declared;
    CC_ASSERT(!is_sampler(b.type), "staging_binding_group: that binding is a sampler — it takes no view");
    CC_ASSERT(info.first_descriptor >= 0, "staging_binding_group: that binding has no view descriptor");
    CC_ASSERT(count >= 0 && first_element >= 0 && first_element + count <= int(b.count),
              "staging_binding_group: the run does not fit inside the binding's element count");
    if (count == 0)
        return;

    clear_view_descriptors(info.first_descriptor + first_element, b, count);
    _dirty = true;
}
} // namespace sg
