#include <clean-core/container/vector.hh>
#include <shaped-viewer/material/material_library.hh>
#include <typed-geometry/linalg/quat.hh>
#include <typed-geometry/linalg/vec.hh>

// The material types every library starts with.
//
// Each `shader` is an HLSL FRAGMENT, not a compilable shader: the generated prologue has already declared and initialized one
// local per signature attribute, and one `sv::surface surface` for it to write.
// So a fragment reads the attributes by their declared names and assigns `surface`, and knows nothing about where any of it came
// from — which is what lets one fragment serve a constant, a per-corner attribute and a texture alike.
//
// `sv::surface` is OpenPBR's parameter set (shaders/openpbr.hlsli), so every type here writes that one vocabulary and the
// integrator has a single BSDF to evaluate.
// `openpbr` exposes it directly; `pbr` and `unlit` are projections onto it, the same way glTF's metallic-roughness maps onto
// OpenPBR.
//
// TEMPORARY: these live as string literals because slib has no named-HLSL-fragment asset kind — a fragment is not a shader, so
// `sc_add_shader_package` has nothing to declare it as.
// Moving them into `shaders/` once it does gets editor support and hot reload; see libs/graphics/shaped-viewer/docs/TODO.md.

namespace sv
{
namespace
{
// The identity fragment: every attribute is already an OpenPBR parameter, so this only clamps what the BSDF requires.
constexpr cc::string_view openpbr_shader = R"hlsl(
    surface.base_weight = saturate(base_weight);
    surface.base_color = saturate(base_color);
    surface.base_metalness = saturate(base_metalness);
    surface.base_diffuse_roughness = saturate(base_diffuse_roughness);

    surface.specular_weight = max(0.0, specular_weight);
    surface.specular_color = saturate(specular_color);
    surface.specular_roughness = saturate(specular_roughness);
    surface.specular_roughness_anisotropy = saturate(specular_roughness_anisotropy);
    surface.specular_ior = max(1.0, specular_ior);

    surface.transmission_weight = saturate(transmission_weight);
    surface.transmission_color = saturate(transmission_color);
    surface.transmission_depth = max(0.0, transmission_depth);

    surface.coat_weight = saturate(coat_weight);
    surface.coat_color = saturate(coat_color);
    surface.coat_roughness = saturate(coat_roughness);
    surface.coat_roughness_anisotropy = saturate(coat_roughness_anisotropy);
    surface.coat_ior = max(1.0, coat_ior);
    surface.coat_darkening = saturate(coat_darkening);

    surface.fuzz_weight = saturate(fuzz_weight);
    surface.fuzz_color = saturate(fuzz_color);
    surface.fuzz_roughness = saturate(fuzz_roughness);

    surface.thin_film_weight = saturate(thin_film_weight);
    surface.thin_film_thickness = max(0.0, thin_film_thickness);
    surface.thin_film_ior = max(1.0, thin_film_ior);

    surface.emission_luminance = max(0.0, emission_luminance);
    surface.emission_color = max(float3(0, 0, 0), emission_color);

    surface.geometry_thin_walled = thin_walled;
    surface.geometry_normal = normalize(normal);
    surface.geometry_coat_normal = normalize(coat_normal);
    surface.geometry_opacity = saturate(opacity);

    surface.geometry_tangent_frame = tangent_frame;
    surface.geometry_tangent = tangent;
    surface.geometry_handedness = tangent_handedness;
)hlsl";

// glTF metallic-roughness, projected onto the OpenPBR surface.
//
// The projection is the one glTF's own OpenPBR mapping uses: `metallic` is `base_metalness`, `roughness` is
// `specular_roughness`, and the dielectric keeps OpenPBR's default IOR of 1.5 — which is glTF's 0.04 reflectance.
// `occlusion` folds into `base_weight` rather than being a parameter of its own, because OpenPBR has no such parameter and a
// baked occlusion term is a modulation of how much base is there.
constexpr cc::string_view pbr_shader = R"hlsl(
    surface.base_weight = saturate(occlusion);
    surface.base_color = base_color;
    surface.base_metalness = saturate(metallic);
    surface.specular_roughness = saturate(roughness);

    surface.emission_luminance = 1.0;
    surface.emission_color = emissive;

    surface.geometry_normal = normalize(normal);
    surface.geometry_tangent_frame = tangent_frame;
    surface.geometry_handedness = tangent_handedness;
)hlsl";

// Unlit is emission and nothing else: no base and no specular means no bounce reaches it, so the integrator returns `color`.
constexpr cc::string_view unlit_shader = R"hlsl(
    surface.base_weight = 0.0;
    surface.specular_weight = 0.0;

    surface.emission_luminance = 1.0;
    surface.emission_color = color;

    surface.geometry_opacity = saturate(opacity);
)hlsl";

[[nodiscard]] material_type make_openpbr()
{
    // The defaults are the specification's own, so a material binding nothing is OpenPBR's default surface.
    auto signature = cc::vector<material_signature_entry>();

    signature.push_back(material_signature_entry::of("base_weight", 1.0f));
    signature.push_back(material_signature_entry::of("base_color", tg::vec3f(0.8f, 0.8f, 0.8f)));
    signature.push_back(material_signature_entry::of("base_metalness", 0.0f));
    signature.push_back(material_signature_entry::of("base_diffuse_roughness", 0.0f));

    signature.push_back(material_signature_entry::of("specular_weight", 1.0f));
    signature.push_back(material_signature_entry::of("specular_color", tg::vec3f(1.0f, 1.0f, 1.0f)));
    signature.push_back(material_signature_entry::of("specular_roughness", 0.3f));
    signature.push_back(material_signature_entry::of("specular_roughness_anisotropy", 0.0f));
    signature.push_back(material_signature_entry::of("specular_ior", 1.5f));

    // `transmission_depth` is the distance `transmission_color` is the color AT.
    // 0 is OpenPBR's default and means the color is a property of the crossing rather than of a volume, which is also the
    // only form a thin wall can have.
    signature.push_back(material_signature_entry::of("transmission_weight", 0.0f));
    signature.push_back(material_signature_entry::of("transmission_color", tg::vec3f(1.0f, 1.0f, 1.0f)));
    signature.push_back(material_signature_entry::of("transmission_depth", 0.0f));

    signature.push_back(material_signature_entry::of("coat_weight", 0.0f));
    signature.push_back(material_signature_entry::of("coat_color", tg::vec3f(1.0f, 1.0f, 1.0f)));
    signature.push_back(material_signature_entry::of("coat_roughness", 0.0f));
    signature.push_back(material_signature_entry::of("coat_roughness_anisotropy", 0.0f));
    signature.push_back(material_signature_entry::of("coat_ior", 1.6f));
    signature.push_back(material_signature_entry::of("coat_darkening", 1.0f));

    signature.push_back(material_signature_entry::of("fuzz_weight", 0.0f));
    signature.push_back(material_signature_entry::of("fuzz_color", tg::vec3f(1.0f, 1.0f, 1.0f)));
    signature.push_back(material_signature_entry::of("fuzz_roughness", 0.5f));

    // Thickness is in nanometres, which is the scale interference happens at — a film is a few hundred of them.
    signature.push_back(material_signature_entry::of("thin_film_weight", 0.0f));
    signature.push_back(material_signature_entry::of("thin_film_thickness", 500.0f));
    signature.push_back(material_signature_entry::of("thin_film_ior", 1.4f));

    signature.push_back(material_signature_entry::of("emission_luminance", 0.0f));
    signature.push_back(material_signature_entry::of("emission_color", tg::vec3f(1.0f, 1.0f, 1.0f)));

    // Tangent space, so the default is the shading normal rather than any particular direction in world space.
    signature.push_back(material_signature_entry::of("normal", tg::vec3f(0.0f, 0.0f, 1.0f)));
    signature.push_back(material_signature_entry::of("opacity", 1.0f));

    // Nonzero makes the surface a shell with no interior: it does not refract and encloses no medium.
    signature.push_back(material_signature_entry::of("thin_walled", 0.0f));

    // The coat's own shading normal, in the same tangent space — a coat with its own bumps over a smoother base.
    // Defaulting to the base's normal is what makes an unbound one share it exactly.
    signature.push_back(material_signature_entry::of("coat_normal", tg::vec3f(0.0f, 0.0f, 1.0f)));

    // Which way the anisotropic lobes are stretched, in the same tangent space.
    // The default is the frame's own tangent, so a surface that never binds this has its highlight follow the uv layout.
    signature.push_back(material_signature_entry::of("tangent", tg::vec3f(1.0f, 0.0f, 0.0f)));

    // The tangent frame, as a rotation taking tangent space to object space, plus the handedness no rotation can carry.
    // Both default to a frame nothing supplied, and the generated `SV_ATTR_SUPPLIED_tangent_frame` is what tells the hit to
    // fall back to the geometric frame rather than trusting the identity.
    signature.push_back(material_signature_entry::of_rotation("tangent_frame", tg::quat_f::make_identity()));
    signature.push_back(material_signature_entry::of("tangent_handedness", 1.0f));

    return material_type::create(cc::string(builtin_material::openpbr), cc::move(signature), cc::string(openpbr_shader));
}

[[nodiscard]] material_type make_pbr()
{
    auto signature = cc::vector<material_signature_entry>();
    signature.push_back(material_signature_entry::of("base_color", tg::vec3f(0.8f, 0.8f, 0.8f)));
    signature.push_back(material_signature_entry::of("metallic", 0.0f));
    signature.push_back(material_signature_entry::of("roughness", 0.5f));
    signature.push_back(material_signature_entry::of("emissive", tg::vec3f(0.0f, 0.0f, 0.0f)));
    // Tangent space, so the default is the geometric normal rather than any particular direction in world space.
    signature.push_back(material_signature_entry::of("normal", tg::vec3f(0.0f, 0.0f, 1.0f)));
    signature.push_back(material_signature_entry::of("occlusion", 1.0f));

    // The tangent frame, as a rotation taking tangent space to object space, plus the handedness no rotation can carry.
    // Both default to a frame nothing supplied, and the generated `SV_ATTR_SUPPLIED_tangent_frame` is what tells the hit to
    // fall back to the geometric frame rather than trusting the identity.
    signature.push_back(material_signature_entry::of_rotation("tangent_frame", tg::quat_f::make_identity()));
    signature.push_back(material_signature_entry::of("tangent_handedness", 1.0f));

    return material_type::create(cc::string(builtin_material::pbr), cc::move(signature), cc::string(pbr_shader));
}

[[nodiscard]] material_type make_unlit()
{
    auto signature = cc::vector<material_signature_entry>();
    signature.push_back(material_signature_entry::of("color", tg::vec3f(0.8f, 0.8f, 0.8f)));
    signature.push_back(material_signature_entry::of("opacity", 1.0f));
    return material_type::create(cc::string(builtin_material::unlit), cc::move(signature), cc::string(unlit_shader));
}
} // namespace

void register_builtin_material_types(material_library& lib)
{
    lib.register_type(make_openpbr());
    lib.register_type(make_pbr());
    lib.register_type(make_unlit());
}
} // namespace sv
