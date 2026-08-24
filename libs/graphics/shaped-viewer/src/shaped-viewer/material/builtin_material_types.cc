#include <clean-core/container/vector.hh>
#include <shaped-viewer/material/material_library.hh>
#include <typed-geometry/linalg/vec.hh>

// The material types every library starts with.
//
// Each `shader` is an HLSL FRAGMENT, not a compilable shader: the generated prologue has already declared and initialized one
// local per signature attribute, and one `sv_surface surface` for it to write.
// So a fragment reads the attributes by their declared names and assigns `surface`, and knows nothing about where any of it came
// from — which is what lets one fragment serve a constant, a per-corner attribute and a texture alike.
//
// TEMPORARY: these live as string literals because slib has no named-HLSL-fragment asset kind — a fragment is not a shader, so
// `sc_add_shader_package` has nothing to declare it as.
// Moving them into `shaders/` once it does gets editor support and hot reload; see libs/graphics/shaped-viewer/docs/TODO.md.

namespace sv
{
namespace
{
constexpr cc::string_view pbr_shader = R"hlsl(
    surface.albedo = base_color;
    surface.metallic = saturate(metallic);
    // A perfectly smooth surface makes the specular lobe a delta the estimator cannot sample, so the floor is not cosmetic.
    surface.roughness = clamp(roughness, 0.03, 1.0);
    surface.emissive = emissive;
    surface.normal = normalize(normal);
    surface.occlusion = saturate(occlusion);
    surface.opacity = 1.0;
)hlsl";

constexpr cc::string_view unlit_shader = R"hlsl(
    // Unlit is emission and nothing else: no albedo means no bounce reaches it, so the integrator returns `color` directly.
    surface.albedo = float3(0, 0, 0);
    surface.emissive = color;
    surface.metallic = 0.0;
    surface.roughness = 1.0;
    surface.normal = float3(0, 0, 1);
    surface.occlusion = 1.0;
    surface.opacity = opacity;
)hlsl";

[[nodiscard]] material_type make_pbr()
{
    auto signature = cc::vector<material_attribute_decl>();
    signature.push_back(material_attribute_decl::of("base_color", tg::vec3f(0.8f, 0.8f, 0.8f)));
    signature.push_back(material_attribute_decl::of("metallic", 0.0f));
    signature.push_back(material_attribute_decl::of("roughness", 0.5f));
    signature.push_back(material_attribute_decl::of("emissive", tg::vec3f(0.0f, 0.0f, 0.0f)));
    // Tangent space, so the default is the geometric normal rather than any particular direction in world space.
    signature.push_back(material_attribute_decl::of("normal", tg::vec3f(0.0f, 0.0f, 1.0f)));
    signature.push_back(material_attribute_decl::of("occlusion", 1.0f));
    return material_type::create(cc::string(builtin_material::pbr), cc::move(signature), cc::string(pbr_shader));
}

[[nodiscard]] material_type make_unlit()
{
    auto signature = cc::vector<material_attribute_decl>();
    signature.push_back(material_attribute_decl::of("color", tg::vec3f(0.8f, 0.8f, 0.8f)));
    signature.push_back(material_attribute_decl::of("opacity", 1.0f));
    return material_type::create(cc::string(builtin_material::unlit), cc::move(signature), cc::string(unlit_shader));
}
} // namespace

void register_builtin_material_types(material_library& lib)
{
    lib.register_type(make_pbr());
    lib.register_type(make_unlit());
}
} // namespace sv
