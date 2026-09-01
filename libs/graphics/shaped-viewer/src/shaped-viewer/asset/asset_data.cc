#include <clean-core/common/utility.hh> // cc::min, cc::max
#include <shaped-viewer/asset/asset_data.hh>

namespace sv
{
namespace
{
void grow_to_include(cc::optional<tg::aabb3f>& box, tg::pos3f const& p)
{
    if (!box.has_value())
    {
        box = tg::aabb3f(p, p);
        return;
    }
    for (auto i = 0; i < 3; ++i)
    {
        box.value().min[i] = cc::min(box.value().min[i], p[i]);
        box.value().max[i] = cc::max(box.value().max[i], p[i]);
    }
}
} // namespace

sv::mesh const* asset_data::find_mesh(cc::string_view name) const
{
    for (auto const& m : meshes)
        if (m.name == name)
            return &m;
    return nullptr;
}

cc::vector<sv::mesh const*> asset_data::meshes_with_material(cc::string_view name) const
{
    auto out = cc::vector<sv::mesh const*>();
    for (auto const& slot : materials)
        if (slot.name == name)
            for (auto const index : slot.meshes)
                out.push_back(&meshes[isize(index)]);
    return out;
}

material_id asset_data::material(cc::string_view name) const
{
    for (auto const& m : materials)
        if (m.name == name)
            return m.material;
    return material_id::invalid;
}

isize asset_data::override_material(cc::string_view name, material_id replacement)
{
    auto rewritten = isize(0);
    for (auto& slot : materials)
    {
        if (slot.name != name)
            continue;

        // The slot's own meshes rather than everything sharing its id: a content-addressed library may have handed the
        // same id to another slot, and that one keeps what it had.
        for (auto const index : slot.meshes)
        {
            meshes[isize(index)].material = replacement;
            ++rewritten;
        }
        slot.material = replacement;
    }
    return rewritten;
}

cc::optional<tg::aabb3f> asset_data::bounds() const
{
    auto box = cc::optional<tg::aabb3f>();
    for (auto const& m : meshes)
        for (auto const& p : m.geometry.positions.span())
            grow_to_include(box, p.transformed(m.transform));
    return box;
}
} // namespace sv
