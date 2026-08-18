#include <clean-core/common/utility.hh> // cc::move
#include <shaped-viewer/scene/mesh_attribute.hh>
#include <shaped-viewer/scene/pbr_material.hh>

namespace sv
{
cc::vector<mesh_attribute> pbr_material_attributes(cc::span<pbr_material const> materials)
{
    auto base_color = cc::vector<tg::vec3f>();
    auto metallic = cc::vector<f32>();
    auto roughness = cc::vector<f32>();
    auto emissive = cc::vector<tg::vec3f>();

    base_color.reserve(materials.size());
    metallic.reserve(materials.size());
    roughness.reserve(materials.size());
    emissive.reserve(materials.size());

    for (auto const& m : materials)
    {
        base_color.push_back(m.base_color);
        metallic.push_back(m.metallic);
        roughness.push_back(m.roughness);
        emissive.push_back(m.emissive);
    }

    auto const frequency = attribute_frequency::per_triangle;

    auto out = cc::vector<mesh_attribute>();
    out.reserve(4);
    out.push_back(mesh_attribute::create(pbr_attribute::base_color, frequency, cc::move(base_color)));
    out.push_back(mesh_attribute::create(pbr_attribute::metallic, frequency, cc::move(metallic)));
    out.push_back(mesh_attribute::create(pbr_attribute::roughness, frequency, cc::move(roughness)));
    out.push_back(mesh_attribute::create(pbr_attribute::emissive, frequency, cc::move(emissive)));
    return out;
}
} // namespace sv
