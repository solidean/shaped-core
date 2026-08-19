#include "change_set.hh"

#include <clean-core/algorithm/search.hh>
#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/assert.hh>

using namespace cc::primitive_defines;

namespace
{
using namespace vdoc;

/// The same path with everything below `granularity` reset to a default id.
[[nodiscard]] property_path blanked(property_path const& path, change_granularity granularity)
{
    switch (granularity)
    {
    case change_granularity::component:
        return {.entity = path.entity, .component = path.component};
    case change_granularity::entity:
        return {.entity = path.entity};
    case change_granularity::property:
        break;
    }

    return path;
}

[[nodiscard]] change_granularity coarser_of(change_granularity a, change_granularity b)
{
    return u8(a) > u8(b) ? a : b;
}
} // namespace

vdoc::change_set vdoc::change_set::everything()
{
    auto out = change_set();
    out._everything = true;
    return out;
}

bool vdoc::change_set::covers(property_path const& path) const
{
    if (_everything)
        return true;

    return cc::find_in_sorted(_paths, blanked(path, _granularity), property_path::by_bytes{}).has_value();
}

bool vdoc::change_set::covers_entity(entity_id entity) const
{
    if (_everything)
        return true;

    // A blanked field is the empty id, which sorts before every real one, so this key lands at the entity's first entry
    // whatever the granularity.
    auto const key = property_path{.entity = entity};
    auto const i = cc::first_at_least_in_sorted(_paths, key, property_path::by_bytes{});
    return i < _paths.size() && _paths[i].entity == entity;
}

cc::vector<vdoc::entity_id> vdoc::change_set::entities() const
{
    CC_ASSERT(!_everything, "everything() names no entities - it means all of them, including ones no layer has yet");

    auto out = cc::vector<entity_id>();
    for (auto const& p : _paths)
        if (out.empty() || !(out.back() == p.entity))
            out.push_back(p.entity);

    return out;
}

void vdoc::change_set::coarsen_to(change_granularity granularity)
{
    CC_ASSERT(u8(granularity) >= u8(_granularity),
              "a change set widens but never refines - the finer information is not there, and inventing it would make "
              "a conservative set a lying one");

    if (granularity == _granularity)
        return;

    _granularity = granularity;
    impl_normalize();
}

void vdoc::change_set::union_with(change_set const& rhs)
{
    if (_everything)
        return;

    if (rhs._everything)
    {
        *this = everything();
        return;
    }

    coarsen_to(coarser_of(_granularity, rhs._granularity));

    auto merged = cc::vector<property_path>();
    merged.reserve(_paths.size() + rhs._paths.size());

    // Blanking rhs on the fly keeps it non-decreasing, so this stays a merge; what it can create is duplicates, which
    // the push dedups against the tail.
    auto const push = [&](property_path const& key)
    {
        if (merged.empty() || !(merged.back() == key))
            merged.push_back(key);
    };

    auto i = isize(0);
    auto j = isize(0);

    while (i < _paths.size() && j < rhs._paths.size())
    {
        auto const key = blanked(rhs._paths[j], _granularity);
        auto const order = _paths[i].compare_bytes(key);

        if (order <= 0)
        {
            push(_paths[i]);
            ++i;
            if (order == 0)
                ++j;
        }
        else
        {
            push(key);
            ++j;
        }
    }

    while (i < _paths.size())
        push(_paths[i++]);

    while (j < rhs._paths.size())
        push(blanked(rhs._paths[j++], _granularity));

    _paths = cc::move(merged);
}

void vdoc::change_set::clear()
{
    *this = {};
}

void vdoc::change_set::impl_normalize()
{
    auto kept = isize(0);
    for (auto i = isize(0); i < _paths.size(); ++i)
    {
        auto const key = blanked(_paths[i], _granularity);
        if (kept > 0 && _paths[kept - 1] == key)
            continue;

        _paths[kept] = key;
        ++kept;
    }

    _paths.resize_down_to(kept);
}

vdoc::change_set_builder::change_set_builder(change_granularity granularity) : _granularity(granularity)
{
}

void vdoc::change_set_builder::add(property_path const& path)
{
    if (_everything)
        return;

    _paths.push_back(path);
}

void vdoc::change_set_builder::add_entity(entity_id entity)
{
    if (_everything)
        return;

    _granularity = change_granularity::entity;
    _paths.push_back({.entity = entity});
}

void vdoc::change_set_builder::add_everything()
{
    _everything = true;
    _paths.clear();
}

void vdoc::change_set_builder::clear()
{
    _paths.clear();
    _everything = false;
}

vdoc::change_set vdoc::change_set_builder::build() &&
{
    auto out = change_set();
    out._granularity = _granularity;

    if (_everything)
    {
        out._everything = true;
        _everything = false;
        return out;
    }

    cc::sort(_paths, property_path::by_bytes{});
    out._paths = cc::move(_paths);
    out.impl_normalize();
    return out;
}
