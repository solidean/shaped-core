#include <babel-serializer/geometry/fbx.hh>
#include <babel-serializer/image/image.hh>
#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <shaped-viewer/asset/asset_loader.hh>
#include <shaped-viewer/asset/impl/asset_import.hh>
#include <shaped-viewer/asset/uri_resolver.hh>
#include <shaped-viewer/scene/light.hh>
#include <shaped-viewer/scene/mesh.hh>
#include <typed-geometry/linalg/mat.hh>
#include <typed-geometry/linalg/quat.hh>
#include <typed-geometry/transform/compose.hh>

// FBX into sv's vocabulary.
//
// babel hands back FBX's own object model: polygons with per-corner attributes, and materials as property bags connected to the NODE placing a mesh.
// The scene's axes and unit are reported but not applied.
// So this importer owes three things the glTF one does not.
//
// - **The space conversion.**
//   sv is glTF's convention — meters, +Y up, right-handed — and FBX declares its own.
//   So one affine transform from the file's axes and unit to sv's sits at the root of every placement.
// - **The material model.**
//   FBX's Phong / Lambert properties mean whatever the exporting pipeline made them mean.
//   So the loader's `fbx_material_convention` says which reading to take; the enum documents both.
// - **The uv convention.**
//   FBX's V points up the image and sv samples images top row first, so every uv is flipped.
//   Tangents are built from the unflipped uvs, whose +V is what a normal map calls up.
//
// Textures ride on the mesh as `mesh_texture`, as in the glTF importer, which is what keeps the import CPU-side.

namespace sv
{
namespace
{
namespace bf = babel::fbx;

/// The uv attribute a mesh's `set`-th uv set is imported under: `uv` for the first, which every material's default `uv_attribute` looks for.
[[nodiscard]] cc::string uv_name_of(isize set)
{
    return set == 0 ? cc::string("uv") : cc::format("uv{}", set);
}

/// A signed axis as the unit vector it names.
[[nodiscard]] tg::vec3f direction_of(bf::axis a)
{
    switch (a)
    {
    case bf::axis::positive_x:
        return tg::vec3f(1, 0, 0);
    case bf::axis::negative_x:
        return tg::vec3f(-1, 0, 0);
    case bf::axis::positive_y:
        return tg::vec3f(0, 1, 0);
    case bf::axis::negative_y:
        return tg::vec3f(0, -1, 0);
    case bf::axis::positive_z:
        return tg::vec3f(0, 0, 1);
    case bf::axis::negative_z:
        return tg::vec3f(0, 0, -1);
    }
    return tg::vec3f(0, 1, 0);
}

/// The transform from the file's space to sv's: the file's right, up and front onto +X, +Y and +Z, scaled to meters.
///
/// FBX's front axis points out of the screen at the viewer, which is glTF's +Z, so a right-handed file maps by a rotation and a left-handed one by a mirror.
[[nodiscard]] tg::affine_transform3f space_conversion_of(bf::scene_settings const& s)
{
    auto const right = direction_of(s.right);
    auto const up = direction_of(s.up);
    auto const front = direction_of(s.front);

    // Row i of the linear part is the file direction that lands on sv's axis i, so column j holds component j of each.
    auto const linear = tg::mat3f::make_from_cols(
        tg::vec3f(right[0], up[0], front[0]), tg::vec3f(right[1], up[1], front[1]), tg::vec3f(right[2], up[2], front[2]));
    // babel already replaced a unit outside [1e-9, 1e9] meters; this only keeps a hand-built document honest.
    auto const meters = s.unit_meters >= 1e-9 && s.unit_meters <= 1e9 ? f32(s.unit_meters) : 1.0f;
    return tg::affine_transform3f::make_from_linear_mat(linear * meters);
}

[[nodiscard]] tg::affine_transform3f affine_of(bf::transform const& t)
{
    // FBX's composed local transform applies in glTF's order: scale, then rotate, then translate.
    auto const s = tg::affine_transform3f::make_scaling(t.scale);
    auto const r = tg::affine_transform3f::make_rotation(t.rotation);
    auto const m = tg::affine_transform3f::make_translation(t.translation);
    return tg::compose(m, tg::compose(r, s));
}

[[nodiscard]] tg::vec3f vec3_of(tg::vec4f v)
{
    return tg::vec3f(v[0], v[1], v[2]);
}

[[nodiscard]] bool is_black(tg::vec3f v)
{
    return v[0] <= 0.0f && v[1] <= 0.0f && v[2] <= 0.0f;
}

/// What a decoded image is for, which decides both its color space and what the import does to its pixels.
enum class texture_role : u8
{
    /// sRGB-encoded color, alpha linear — base color and emission
    color,
    /// linear data — a packed specular map
    data,
    /// a tangent-space normal map, linear, with its third channel reconstructed when the file stored two
    normal,
};

/// One decoded texture, plus what the import learned from its pixels.
struct decoded_texture
{
    texture_data data;

    /// any texel with alpha below 1, which is what makes a base color a cutout
    bool has_alpha = false;
};

/// A texture's encoded bytes, or why there are none: a name the importer refused is reported as refused and nothing else.
struct fetched_texture
{
    cc::optional<cc::pinned_data<byte const>> bytes;

    /// no bytes, and the name as written was never safe to ask for
    bool refused = false;
};

/// The reconstructed third component of a two-channel normal map, from the two it stored.
///
/// A BC5 normal map keeps x and y and drops z, since a unit normal's z follows from them.
/// Decoded to rgba8 the blue channel then reads as zero, which unreconstructed would tilt every normal flat into the surface.
void reconstruct_normal_z(cc::vector<byte>& rgba)
{
    for (auto p = isize(0); p + 3 < rgba.size(); p += 4)
    {
        auto const x = f32(u8(rgba[p])) / 127.5f - 1.0f;
        auto const y = f32(u8(rgba[p + 1])) / 127.5f - 1.0f;
        auto const zz = 1.0f - x * x - y * y;
        auto const z = zz > 0.0f ? tg::sqrt(zz) : 0.0f;
        rgba[p + 2] = byte(u32(z * 127.5f + 127.5f + 0.5f));
    }
}

/// Everything one import carries between its passes.
struct fbx_importer
{
    bf::data const& doc;
    asset_loader_config const& cfg;
    uri_resolver resolve;

    /// where the document came from; texture names are joined onto its directory
    cc::string_view base_uri;

    asset_data out;

    /// one definition per FBX material in file order, plus a trailing default when some slot names none
    cc::vector<impl::asset_material_definition> definitions;
    cc::vector<cc::vector<mesh_texture>> material_textures;

    /// per definition: whether it binds a normal map, which is what decides that its meshes want uv-aligned tangents
    cc::vector<u8> material_normal_mapped;
    isize default_slot = -1;

    tg::affine_transform3f conversion = {};

    /// decoded textures by (texture index, role); an empty optional remembers a failure, so each is reported once
    cc::map<u64, cc::optional<decoded_texture>> decoded;

    /// the parts each FBX mesh splits into, built once per (mesh, slot, uv tangents) and shared by every instance
    cc::map<u64, cc::optional<mesh>> parts;

    bool fanned = false;

    /// The file names of textures that could not be fetched or decoded, reported as one issue each at the end.
    /// One issue per texture would bury everything else: a scene whose image format is not decodable yet fails every one.
    cc::vector<cc::string> unfetched;
    cc::vector<cc::string> undecodable;

    /// the texture names that are not a relative file path inside the document's directory — a scheme, a drive, a network path, a device, a `..` — and are never fetched as written
    cc::vector<cc::string> refused;

    /// the materials whose values were not finite and non-negative, and were clamped
    cc::vector<cc::string> clamped_materials;

    /// the ORCA materials whose packed specular map is connected but did not decode
    cc::vector<cc::string> packed_unusable;

    void note(cc::string message) { out.issues.push_back(cc::move(message)); }

    /// Adds `name` to one of the per-texture lists unless it is there already, since one texture decodes once per role.
    static void remember(cc::vector<cc::string>& names, cc::string name)
    {
        for (auto const& known : names)
            if (known == name)
                return;
        names.push_back(cc::move(name));
    }

    /// One issue naming how many textures failed `how`, and the first few of them.
    void note_textures(cc::span<cc::string const> names, cc::string_view how)
    {
        if (names.empty())
            return;
        auto listed = cc::string();
        for (auto i = isize(0); i < names.size() && i < 3; ++i)
            listed += cc::format("{}'{}'", i == 0 ? "" : ", ", names[i]);
        note(cc::format("fbx: {} texture(s) could not be {}, so their materials keep their factors: {}{}", names.size(),
                        how, listed, names.size() > 3 ? ", ..." : ""));
    }

    // textures
    // ---------------------------------------------------------------------------------------------

    /// The texture's encoded bytes: its embedded copy, else its relative file name, else its bare file name — both in the document's own directory, through the resolver.
    ///
    /// A file name is something the FILE says, and it is a raw file path, never a uri, so nothing in it may send the import off the machine or to a place of its own choosing.
    /// - A name holding `:` anywhere — a scheme (`https:`, `file:`) or a drive (`C:\`, `D:x.png`) — is never asked for as written.
    /// - Nor is a name opening with `//` once its backslashes are portable, a UNC or network path.
    /// - Nor is one with a component Windows opens as a device (`CON`, `NUL`, `COM1`, `LPT1`, ... in any case, with any extension).
    /// - Nor is one with a `..` component, which would climb out of the document's directory.
    /// - A name opening with `/` is absolute, so only its bare file name is asked for.
    /// - `%` is escaped as `%25`, so the resolver's percent-decoding hands the name back literally rather than turning `%5C%5C` into a network path or `%2E%2E%2F` into `../`.
    /// - The join is done here, onto the document's directory, so no absolute passthrough is reached.
    /// After those checks the bare file name — what follows the last `/` — is still tried beside the document whenever it is itself safe.
    /// That is what a Windows-authored FBX naming `C:\Users\...\wood.png` needs, and `../shared/wood.png` is asked for as `wood.png` the same way.
    /// So nothing the file names is ever fetched from outside the document's directory; `absolute_filename` is only ever asked for by its bare name.
    [[nodiscard]] fetched_texture fetch(bf::texture const& t)
    {
        if (t.content.size() > 0)
            return {.bytes = t.content};

        auto const& written = t.relative_filename.empty() ? t.absolute_filename : t.relative_filename;

        // An FBX written on Windows separates with backslashes, which only Windows opens.
        auto name = cc::string(written);
        auto* const chars = name.data();
        for (auto i = isize(0); i < name.size(); ++i)
            if (chars[i] == '\\')
                chars[i] = '/';
        auto const view = cc::string_view(name);

        // The whole name as written, when it is a plain relative path.
        auto const whole_safe = is_safe_path(view) && !view.starts_with("/");
        auto const tried = whole_safe && !t.relative_filename.empty();
        if (tried)
            if (auto bytes = resolve_beside(view); bytes.has_value())
                return {.bytes = cc::move(bytes.value())};

        // The bare file name, which is where an archive moved off its author's machine usually keeps it.
        auto const slash = view.rfind('/');
        auto const base = slash >= 0 ? view.subview(slash + 1) : view;
        if (!base.empty() && is_safe_path(base) && (!tried || base != view))
            if (auto bytes = resolve_beside(base); bytes.has_value())
                return {.bytes = cc::move(bytes.value())};

        // Nothing safe resolved; a name that was unsafe as written is the more useful thing to report.
        return {.refused = !is_safe_path(view)};
    }

    /// Whether `path` (portable separators) is a relative file path the importer may hand the resolver.
    /// No `:` (a scheme or a drive), no leading `//` (a network path), no `..` component, and no component naming a Windows device.
    [[nodiscard]] static bool is_safe_path(cc::string_view path)
    {
        if (path.contains(':') || path.starts_with("//") || impl::names_windows_device(path))
            return false;

        auto rest = path;
        while (!rest.empty())
        {
            auto const slash = rest.find('/');
            auto const component = slash >= 0 ? rest.subview({.offset = 0, .size = slash}) : rest;
            if (component == "..")
                return false;
            rest = slash >= 0 ? rest.subview(slash + 1) : cc::string_view();
        }
        return true;
    }

    /// `name` — a relative file path already checked by `fetch` — as a uri in the document's directory, resolved.
    [[nodiscard]] cc::result<cc::pinned_data<byte const>> resolve_beside(cc::string_view name)
    {
        auto uri = cc::string(impl::directory_of(base_uri));
        for (auto const c : name)
        {
            if (c == '%')
                uri += "%25";
            else
                uri += c;
        }
        return resolve(uri);
    }

    [[nodiscard]] cc::optional<decoded_texture> decode(bf::texture_index index, texture_role role)
    {
        auto const key = u64(u32(int(index))) | (u64(role) << 32);
        if (auto const* const known = decoded.get_ptr(key); known != nullptr)
            return *known;

        auto result = decode_uncached(index, role);
        decoded[key] = result;
        return result;
    }

    [[nodiscard]] cc::optional<decoded_texture> decode_uncached(bf::texture_index index, texture_role role)
    {
        auto const* const t = doc.find(index);
        if (t == nullptr)
            return {};

        if (!t->is_file)
            return {}; // babel already said so

        auto fetched = fetch(*t);
        if (fetched.refused)
        {
            remember(refused, t->relative_filename.empty() ? t->absolute_filename : t->relative_filename);
            return {};
        }
        if (!fetched.bytes.has_value())
        {
            remember(unfetched, t->relative_filename.empty() ? t->name : t->relative_filename);
            return {};
        }

        auto image = babel::image::read(fetched.bytes.value().span());
        if (image.has_error())
        {
            remember(undecodable, t->relative_filename.empty() ? t->name : t->relative_filename);
            return {};
        }

        auto const& img = image.value();
        auto pixels = impl::to_rgba8(img);
        if (pixels.has_error())
        {
            note(cc::format("fbx: texture '{}' decoded to no usable pixels", t->name));
            return {};
        }

        auto& rgba = pixels.value();
        if (role == texture_role::normal)
        {
            // Two channels are x and y, not gray and alpha: `to_rgba8` spread the first over rgb and put the second in alpha, so both go back where a normal map keeps them before z is rebuilt.
            auto all_blue_zero = true;
            for (auto p = isize(0); p + 3 < rgba.size(); p += 4)
            {
                if (img.channels == 2)
                {
                    rgba[p + 1] = rgba[p + 3];
                    rgba[p + 3] = byte(255);
                }
                all_blue_zero = all_blue_zero && u8(rgba[p + 2]) == 0;
            }
            if (img.channels == 2 || all_blue_zero)
                reconstruct_normal_z(rgba);
        }

        auto has_alpha = false;
        for (auto p = isize(3); p < rgba.size() && !has_alpha; p += 4)
            has_alpha = u8(rgba[p]) < 255;

        auto const format
            = role == texture_role::color ? sg::pixel_format::rgba8_unorm_srgb : sg::pixel_format::rgba8_unorm;
        return decoded_texture{.data = texture_data::create(cc::move(rgba), format, img.width, img.height),
                               .has_alpha = has_alpha};
    }

    /// Binds `index` under the material attribute `name`, returning the decode so a caller can read what it learned.
    cc::optional<decoded_texture> bind(cc::vector<mesh_texture>& into,
                                       cc::string name,
                                       bf::texture_index index,
                                       texture_role role,
                                       channel_swizzle swizzle,
                                       sample_transform transform = {})
    {
        if (index == bf::texture_index::invalid || !cfg.import_textures)
            return {};

        auto found = decode(index, role);
        if (!found.has_value())
            return {};

        auto const& t = *doc.find(index);
        auto sampler = sg::sampler();
        sampler.address_u = t.wrap_u == bf::wrap_mode::clamp ? sg::sampler_address_mode::clamp_edge
                                                             : sg::sampler_address_mode::repeat;
        sampler.address_v = t.wrap_v == bf::wrap_mode::clamp ? sg::sampler_address_mode::clamp_edge
                                                             : sg::sampler_address_mode::repeat;

        // The texture's own uv set, by name; rewritten per mesh to the index that mesh carries it at.
        into.push_back({.name = cc::move(name),
                        .source = {.texture = found.value().data,
                                   .uv_attribute = t.uv_set.empty() ? cc::string("uv") : cc::string(t.uv_set),
                                   .sampler = sampler,
                                   .swizzle = swizzle,
                                   .transform = transform}});
        return found;
    }

    // materials
    // ---------------------------------------------------------------------------------------------

    /// `v` held finite and non-negative, a NaN to 0, so no file value reaches OpenPBR as something it asserts on.
    /// `material` is the display name, which an issue reports.
    [[nodiscard]] f32 sanitized(cc::string_view material, f32 v)
    {
        if (v >= 0.0f && v < 1e30f)
            return v;
        if (clamped_materials.empty() || clamped_materials.back() != material)
            clamped_materials.push_back(cc::string(material));
        return v >= 1e30f ? 1e30f : 0.0f;
    }

    [[nodiscard]] tg::vec3f color_of(bf::material const& m,
                                     cc::string_view material,
                                     cc::string_view property,
                                     tg::vec3f fallback)
    {
        auto const* const p = m.find_property(property);
        if (p == nullptr)
            return fallback;
        return tg::vec3f(sanitized(material, p->value[0]), sanitized(material, p->value[1]),
                         sanitized(material, p->value[2]));
    }

    [[nodiscard]] f32 number_of(bf::material const& m, cc::string_view material, cc::string_view property, f32 fallback)
    {
        auto const* const p = m.find_property(property);
        return p != nullptr ? sanitized(material, p->value[0]) : fallback;
    }

    void build_material(bf::material const& m, cc::string_view name)
    {
        auto bindings = cc::vector<material_attribute_binding>();
        auto textures = cc::vector<mesh_texture>();
        using binding = material_attribute_binding;

        auto const orca = cfg.fbx_materials == fbx_material_convention::orca_metal_rough;

        // Base color: FBX's DiffuseColor, whose texture wins over it as every sample beats a constant.
        // The Phong reading folds DiffuseFactor in; the ORCA scenes leave it at 1.
        auto const diffuse = color_of(m, name, "DiffuseColor", tg::vec3f(0.8f, 0.8f, 0.8f));
        auto const diffuse_factor = orca ? 1.0f : number_of(m, name, "DiffuseFactor", 1.0f);
        bindings.push_back(binding::of("base_color", diffuse * diffuse_factor));
        auto const base
            = bind(textures, "base_color", m.find_texture("DiffuseColor"), texture_role::color, channel_swizzle());

        // A base color with alpha below 1 anywhere is a cutout, stepped at one half as Falcor's alpha test does.
        // Read off the pixels because FBX has no alpha mode, and binding one for every material would put every mesh through the any-hit path.
        if (base.has_value() && base.value().has_alpha)
        {
            bind(textures, "opacity", m.find_texture("DiffuseColor"), texture_role::color,
                 channel_swizzle::of_channel(texture_channel::a));
            bindings.push_back(binding::of("alpha_cutoff", 0.5f));
        }

        if (orca)
        {
            // NVIDIA's ORCA scenes as Falcor reads them: SpecularColor is a packed map, occlusion in red, roughness in green and metalness in blue.
            // Its constant is read the same way when no map is connected.
            // A connected map that did not decode does NOT fall back to the constant: Bistro's is a placeholder 0.5, which would make every surface half metal.
            // Neutral defaults stand in instead, and say so.
            auto const packed = m.find_texture("SpecularColor");
            auto const specular = color_of(m, name, "SpecularColor", tg::vec3f(0.5f, 0.5f, 0.5f));
            auto roughness = specular[1];
            auto metalness = specular[2];
            if (packed != bf::texture_index::invalid)
            {
                auto const bound = bind(textures, "occlusion", packed, texture_role::data,
                                        channel_swizzle::of_channel(texture_channel::r));
                bind(textures, "specular_roughness", packed, texture_role::data,
                     channel_swizzle::of_channel(texture_channel::g));
                bind(textures, "base_metalness", packed, texture_role::data,
                     channel_swizzle::of_channel(texture_channel::b));
                roughness = 0.5f;
                metalness = 0.0f;
                if (!bound.has_value() && cfg.import_textures)
                    packed_unusable.push_back(cc::string(name));
            }
            bindings.push_back(binding::of("specular_roughness", roughness));
            bindings.push_back(binding::of("base_metalness", metalness));
        }
        else
        {
            // Phong's specular exponent onto a GGX roughness: Blinn-Phong n matches a Beckmann alpha of sqrt(2 / (n + 2)), and OpenPBR's roughness is the square root of alpha.
            // Shininess only stands in for an absent ShininessExponent, so it is never sanitized, or reported, when unused.
            auto const* const exponent_property = m.find_property("ShininessExponent");
            auto const exponent = exponent_property != nullptr ? sanitized(name, exponent_property->value[0])
                                                               : number_of(m, name, "Shininess", 20.0f);
            auto const alpha = tg::sqrt(2.0f / ((exponent > 0.0f ? exponent : 0.0f) + 2.0f));
            bindings.push_back(binding::of("specular_roughness", tg::sqrt(alpha)));
            bindings.push_back(binding::of("base_metalness", 0.0f));

            if (m.find_texture("SpecularColor") != bf::texture_index::invalid)
                note(cc::format("fbx: material '{}' has a specular color map, which a Phong reading has no OpenPBR "
                                "parameter for; ORCA scenes want fbx_material_convention::orca_metal_rough",
                                name));
        }

        // Normal maps: ORCA's are DirectX-convention, green pointing down the image, which is sv's +Y negated.
        auto const normal_transform
            = orca ? sample_transform{.scale = tg::vec4f(2, -2, 2, 2), .bias = tg::vec4f(-1, 1, -1, -1)}
                   : sample_transform::of_signed_normal();
        auto const normal_map = bind(textures, "normal", m.find_texture("NormalMap"), texture_role::normal,
                                     channel_swizzle(), normal_transform);
        if (m.find_texture("Bump") != bf::texture_index::invalid)
            note(cc::format("fbx: material '{}' has a bump map, which sv cannot read as a height field", name));

        // Emission: EmissiveColor (or its map) at a luminance of EmissiveFactor, read one-to-one as nits.
        // FBX gives emission no unit, and the ORCA scenes were tuned for Falcor's renderer, so a caller expecting a lit scene scales them — which is what the Bistro example does for its night shot.
        auto const emissive = color_of(m, name, "EmissiveColor", tg::vec3f::zero);
        auto const emissive_map = m.find_texture("EmissiveColor");
        if (!is_black(emissive) || (emissive_map != bf::texture_index::invalid && cfg.import_textures))
        {
            bindings.push_back(binding::of("emission_color", emissive));
            bindings.push_back(binding::of("emission_luminance", number_of(m, name, "EmissiveFactor", 1.0f)));
            bind(textures, "emission_color", emissive_map, texture_role::color, channel_swizzle());
        }

        // Opacity below 1 is coverage, as glTF's BLEND is read: FBX cannot say whether a surface is glass.
        auto const* const opacity_property = m.find_property("Opacity");
        auto const opacity = opacity_property != nullptr ? sanitized(name, opacity_property->value[0]) : 1.0f;
        if (opacity < 1.0f)
        {
            bindings.push_back(binding::of("opacity", opacity));
            note(cc::format("fbx: material '{}' has Opacity {}, imported as coverage — FBX cannot say whether a "
                            "surface is glass, and transmission is not guessed",
                            name, opacity));
        }
        if (m.find_texture("TransparentColor") != bf::texture_index::invalid)
            note(cc::format("fbx: material '{}' has a transparency map, which is not imported", name));

        definitions.push_back({.name = cc::string(name), .bindings = cc::move(bindings)});
        material_textures.push_back(cc::move(textures));
        material_normal_mapped.push_back(normal_map.has_value() ? 1 : 0);
        out.materials.push_back({.name = cc::string(name)});
    }

    void build_materials()
    {
        if (!cfg.import_materials)
            return;

        for (auto i = isize(0); i < doc.materials.size(); ++i)
        {
            auto const& m = doc.materials[i];
            auto const name = m.name.empty() ? cc::format("material{}", i) : cc::string(m.name);
            build_material(m, name);
        }
    }

    /// The slot in `out.materials` a node's material slot lands in, or -1 when materials are not imported.
    [[nodiscard]] isize slot_of(bf::node const& n, i32 mesh_slot)
    {
        if (!cfg.import_materials)
            return -1;

        auto const slots = doc.materials_of(n);
        if (mesh_slot >= 0 && mesh_slot < slots.size())
            if (auto const raw = isize(int(slots[mesh_slot])); raw >= 0 && raw < doc.materials.size())
                return raw;

        // FBX's own default for a face naming no material is the SDK's gray Lambert, which an OpenPBR default matches.
        if (default_slot < 0)
        {
            default_slot = definitions.size();
            definitions.push_back({.name = "default"});
            material_textures.push_back({});
            material_normal_mapped.push_back(0);
            out.materials.push_back({.name = "default"});
        }
        return default_slot;
    }

    // geometry
    // ---------------------------------------------------------------------------------------------

    /// One (mesh, slot) part as an sv mesh with no transform and no material: welded vertices, uv sets, tangent frames.
    [[nodiscard]] cc::optional<mesh> build_part(bf::mesh const& m,
                                                cc::span<i32 const> faces,
                                                bool uv_tangents,
                                                cc::string_view label)
    {
        // Welded by a hash of the whole vertex — its position index and every other attribute's VALUE — and a chain of the vertices sharing one hash, which an exact compare walks.
        // By value: a by-polygon-vertex attribute stored directly gives every corner its own index, so comparing indices would weld nothing and triple the vertex count of a triangulated mesh.
        // By hash rather than a scan of the position's vertices: a forged mesh hanging a million distinct corners off one control point would make that scan quadratic.
        auto heads = cc::map<u64, u32>(); // the newest vertex with each hash
        auto chain = cc::vector<u32>();   // per vertex, the previous one with its hash; `chain_end` ends it
        auto sources = cc::vector<i32>(); // the corner each vertex came from
        auto indices = cc::vector<u32>();
        constexpr auto chain_end = ~u32(0);
        constexpr auto max_chain_walk = 64;

        // Two corners are the same vertex when every byte the hash reads is equal — bytes rather than floats.
        // So a NaN welds to itself instead of starting a new vertex on one ever-growing chain, and the hash and the compare agree on +0 and -0.
        auto const bytes_equal = [](void const* a, void const* b, isize size)
        {
            auto const* const x = static_cast<unsigned char const*>(a);
            auto const* const y = static_cast<unsigned char const*>(b);
            for (auto i = isize(0); i < size; ++i)
                if (x[i] != y[i])
                    return false;
            return true;
        };
        auto const same = [&](i32 a, i32 b)
        {
            if (m.positions.indices[a] != m.positions.indices[b])
                return false;
            if (m.normals.exists() && !bytes_equal(&m.normals.at(a), &m.normals.at(b), sizeof(tg::vec3f)))
                return false;
            if (m.tangents.exists() && !bytes_equal(&m.tangents.at(a), &m.tangents.at(b), sizeof(tg::vec3f)))
                return false;
            if (m.bitangents.exists() && !bytes_equal(&m.bitangents.at(a), &m.bitangents.at(b), sizeof(tg::vec3f)))
                return false;
            for (auto const& set : m.uv_sets)
                if (set.uvs.exists() && !bytes_equal(&set.uvs.at(a), &set.uvs.at(b), sizeof(tg::vec2f)))
                    return false;
            return true;
        };

        // FNV-1a over the bytes `same` compares.
        auto const hash_of = [&](i32 corner) -> u64
        {
            auto h = u64(0xcbf29ce484222325ull);
            auto const feed = [&](void const* data, isize size)
            {
                auto const* const bytes = static_cast<unsigned char const*>(data);
                for (auto i = isize(0); i < size; ++i)
                    h = (h ^ u64(bytes[i])) * u64(0x100000001b3ull);
            };
            feed(&m.positions.indices[corner], sizeof(i32));
            if (m.normals.exists())
                feed(&m.normals.at(corner), sizeof(tg::vec3f));
            if (m.tangents.exists())
                feed(&m.tangents.at(corner), sizeof(tg::vec3f));
            if (m.bitangents.exists())
                feed(&m.bitangents.at(corner), sizeof(tg::vec3f));
            for (auto const& set : m.uv_sets)
                if (set.uvs.exists())
                    feed(&set.uvs.at(corner), sizeof(tg::vec2f));
            return h;
        };

        auto const vertex_for = [&](i32 corner) -> u32
        {
            auto const h = hash_of(corner);
            auto* const head = heads.get_ptr(h);

            // A chain is only ever long when FNV was made to collide on purpose, so the walk stops after a bounded number of steps.
            // The corner then becomes a vertex of its own — a missed weld, never a quadratic import.
            auto steps = 0;
            for (auto v = head != nullptr ? *head : chain_end; v != chain_end && steps < max_chain_walk;
                 v = chain[isize(v)], ++steps)
                if (same(sources[isize(v)], corner))
                    return v;
            auto const index = u32(sources.size());
            sources.push_back(corner);
            chain.push_back(head != nullptr ? *head : chain_end);
            heads[h] = index;
            return index;
        };

        for (auto const f : faces)
        {
            auto const& face = m.faces[isize(f)];
            fanned = fanned || face.corner_count > 3;

            // A fan, for a document read without triangulation; a triangulated one has only triangles here.
            for (auto i = 1; i + 1 < face.corner_count; ++i)
            {
                indices.push_back(vertex_for(face.first_corner));
                indices.push_back(vertex_for(face.first_corner + i));
                indices.push_back(vertex_for(face.first_corner + i + 1));
            }
        }

        if (indices.empty())
            return {};

        auto positions = cc::vector<tg::pos3f>();
        positions.reserve(sources.size());
        for (auto const c : sources)
            positions.push_back(m.positions.at(c));

        auto attributes = cc::vector<mesh_attribute>();
        auto first_uvs = cc::vector<tg::vec2f>(); // set 0 as the file wrote it, for tangents
        for (auto s = isize(0); s < m.uv_sets.size(); ++s)
        {
            auto const& set = m.uv_sets[s].uvs;
            if (!set.exists())
                continue;
            auto uvs = cc::vector<tg::vec2f>();
            uvs.reserve(sources.size());
            for (auto const c : sources)
            {
                auto const uv = set.at(c);
                if (s == 0)
                    first_uvs.push_back(uv);
                uvs.push_back(tg::vec2f(uv[0], 1.0f - uv[1]));
            }
            attributes.push_back(mesh_attribute::create(uv_name_of(s), attribute_frequency::per_vertex, cc::move(uvs)));
        }

        if (cfg.frames.prefer_file && m.normals.exists())
            add_frames(m, sources, positions, indices, first_uvs, uv_tangents, label, attributes);

        return mesh{.name = cc::string(label),
                    .geometry = triangle_geometry::create_from_indexed_triangles(cc::move(positions), cc::move(indices)),
                    .attributes = cc::move(attributes)};
    }

    void add_frames(bf::mesh const& m,
                    cc::span<i32 const> sources,
                    cc::span<tg::pos3f const> positions,
                    cc::span<u32 const> indices,
                    cc::span<tg::vec2f const> uvs,
                    bool uv_tangents,
                    cc::string_view label,
                    cc::vector<mesh_attribute>& attributes)
    {
        auto normals = cc::vector<tg::vec3f>();
        normals.reserve(sources.size());
        auto unusable = isize(0);
        for (auto const c : sources)
        {
            normals.push_back(m.normals.at(c));
            if (!impl::is_usable_normal(normals.back()))
                ++unusable;
        }

        // One unusable normal drops the attribute for the whole part, as in the glTF importer: a made-up frame beside real ones shows as a seam, while the geometric fallback is right everywhere.
        if (unusable > 0)
        {
            note(cc::format("fbx: '{}' has {} of {} normals that cannot be normalized, so no tangent frame is imported "
                            "and the geometric one is used instead",
                            label, unusable, normals.size()));
            return;
        }

        auto frames = cc::vector<tg::quat_f>();
        auto handedness = cc::vector<f32>();
        if (m.tangents.exists() && m.bitangents.exists())
        {
            // The file's own tangents, its bitangent deciding the mirror bit.
            frames.reserve(sources.size());
            handedness.reserve(sources.size());
            for (auto v = isize(0); v < sources.size(); ++v)
            {
                auto const t = m.tangents.at(sources[v]);
                auto const b = m.bitangents.at(sources[v]);
                frames.push_back(impl::tangent_frame_of(normals[v], t));
                auto const mirror = tg::dot(tg::dual(tg::cross(normals[v], t)), b) < 0.0f;
                handedness.push_back(mirror ? -1.0f : 1.0f);
            }
        }
        else if (uv_tangents && uvs.size() == sources.size())
        {
            // A normal map needs a tangent that follows the uv layout, and FBX rarely carries one.
            auto generated = impl::tangent_frames_from_uvs(positions, normals, uvs, indices);
            frames = cc::move(generated.frames);
            handedness = cc::move(generated.handedness);
        }
        else
        {
            frames.reserve(sources.size());
            handedness.reserve(sources.size());
            for (auto const& n : normals)
            {
                frames.push_back(impl::tangent_frame_of(n));
                handedness.push_back(1.0f);
            }
        }

        attributes.push_back(mesh_attribute::create("tangent_frame", attribute_frequency::per_vertex, cc::move(frames)));
        attributes.push_back(
            mesh_attribute::create("tangent_handedness", attribute_frequency::per_vertex, cc::move(handedness)));
    }

    /// The part for (mesh, slot), built on first use and copied after — a copy shares the pinned payloads, which is what makes a second instance of a mesh cost a transform and nothing else.
    [[nodiscard]] cc::optional<mesh> part_of(bf::mesh_index index, i32 slot, bool uv_tangents, cc::string_view label)
    {
        auto const key = (u64(u32(int(index))) << 33) | (u64(u32(slot)) << 1) | (uv_tangents ? 1u : 0u);
        if (auto const* const known = parts.get_ptr(key); known != nullptr)
            return *known;

        auto const& grouped = faces_by_slot(index);
        auto faces = cc::span<i32 const>();
        for (auto i = isize(0); i < grouped.slots.size(); ++i)
            if (grouped.slots[i] == slot)
                faces = cc::span<i32 const>(grouped.faces)
                            .subspan({.offset = isize(grouped.starts[i]),
                                      .size = isize(grouped.starts[i + 1] - grouped.starts[i])});

        auto built = build_part(*doc.find(index), faces, uv_tangents, label);
        parts[key] = built;
        return built;
    }

    /// A mesh's drawable faces grouped by material slot, sorted once rather than scanned once per slot.
    /// Slot `slots[i]` owns `faces[starts[i] .. starts[i + 1])`; the slots ascend, so the parts a mesh splits into are named in a stable order.
    struct slot_faces
    {
        cc::vector<i32> slots;
        cc::vector<i32> starts;
        cc::vector<i32> faces;
    };
    cc::map<int, slot_faces> grouped_faces;

    [[nodiscard]] slot_faces const& faces_by_slot(bf::mesh_index index)
    {
        if (auto const* const known = grouped_faces.get_ptr(int(index)); known != nullptr)
            return *known;

        auto const& m = *doc.find(index);
        auto keys = cc::vector<u64>(); // slot in the high half, face in the low, so one sort groups and orders both
        keys.reserve(m.faces.size());
        for (auto f = isize(0); f < m.faces.size(); ++f)
            if (m.faces[f].corner_count >= 3 && !m.is_hole(f))
                keys.push_back((u64(u32(m.material_slot_of(f))) << 32) | u64(u32(f)));
        cc::sort(keys);

        auto grouped = slot_faces();
        grouped.faces.reserve(keys.size());
        for (auto const k : keys)
        {
            auto const s = i32(u32(k >> 32));
            if (grouped.slots.empty() || grouped.slots.back() != s)
            {
                grouped.slots.push_back(s);
                grouped.starts.push_back(i32(grouped.faces.size()));
            }
            grouped.faces.push_back(i32(u32(k)));
        }
        grouped.starts.push_back(i32(grouped.faces.size()));

        grouped_faces[int(index)] = cc::move(grouped);
        return *grouped_faces.get_ptr(int(index));
    }

    void emit_meshes(bf::node const& n, tg::affine_transform3f const& placement)
    {
        auto const* const m = doc.find(n.mesh);
        if (m == nullptr)
            return;

        auto const base_name = n.name.empty() ? (m->name.empty() ? cc::string("mesh") : cc::string(m->name)) : n.name;
        auto const used = cc::vector<i32>(faces_by_slot(n.mesh).slots);

        for (auto const slot : used)
        {
            auto const name = used.size() == 1 ? base_name : cc::format("{}.{}", base_name, slot);
            if (cfg.include_mesh && !cfg.include_mesh(name))
                continue;

            auto const material = slot_of(n, slot);
            auto const normal_mapped = material >= 0 && material_normal_mapped[material] != 0;
            auto part = part_of(n.mesh, slot, normal_mapped, name);
            if (!part.has_value())
                continue;

            auto placed = cc::move(part.value());
            placed.name = name;
            placed.transform = placement;

            if (material >= 0)
            {
                for (auto t : material_textures[material])
                {
                    // A texture naming a uv set samples whichever index this mesh carries that set at.
                    if (t.source.uv_attribute != "uv")
                    {
                        auto found = cc::string("uv");
                        for (auto s = isize(0); s < m->uv_sets.size(); ++s)
                            if (m->uv_sets[s].name == t.source.uv_attribute)
                                found = uv_name_of(s);
                        t.source.uv_attribute = cc::move(found);
                    }
                    placed.textures.push_back(cc::move(t));
                }
                out.materials[material].meshes.push_back(i32(out.meshes.size()));
            }

            out.meshes.push_back(cc::move(placed));
        }
    }

    // lights
    // ---------------------------------------------------------------------------------------------

    void emit_light(bf::node const& n, tg::affine_transform3f const& placement)
    {
        auto const* const found = doc.find(n.light);
        if (found == nullptr)
            return;
        auto const& l = *found;
        auto const label = l.name.empty() ? (n.name.empty() ? cc::string("a light") : cc::format("light '{}'", n.name))
                                          : cc::format("light '{}'", l.name);

        if (!l.cast_light)
        {
            note(cc::format("fbx: {} is switched off, so it was not imported", label));
            return;
        }

        // Everything the file gives a light is checked before sv sees it, since sv asserts on what it cannot mean.
        // A NaN fails every comparison below, which is how each check also catches one.
        auto const finite = [](f32 v) { return v > -1e30f && v < 1e30f; };
        auto const where = placement.transform(tg::pos3f::zero);
        auto const direction = placement.transform(l.local_direction);
        if (!finite(where[0]) || !finite(where[1]) || !finite(where[2]))
        {
            note(cc::format("fbx: {} is placed at a position that is not finite, so it was not imported", label));
            return;
        }
        auto const direction_usable
            = finite(direction[0]) && finite(direction[1]) && finite(direction[2]) && direction.length() > 1e-12f;
        if (!direction_usable && l.type != bf::light_type::point)
        {
            note(cc::format("fbx: {} has no usable direction once placed, so it was not imported", label));
            return;
        }

        // FBX's intensity is a percentage of the DCC tool's own unit, which is no physical unit at all.
        // It is read one-to-one as candela for a point or spot and lux for a directional light, and a caller rescales to taste.
        auto intensity = l.intensity;
        if (!(intensity >= 0.0f && intensity < 1e30f))
        {
            note(cc::format("fbx: {} has intensity {}, imported as 0", label, l.intensity));
            intensity = 0.0f;
        }

        // Each channel held finite and >= 0, a NaN to 0, since a light emits no negative light.
        auto color = l.color;
        auto color_clamped = false;
        for (auto c = 0; c < 3; ++c)
            if (!(color[c] >= 0.0f && color[c] < 1e30f))
            {
                color[c] = color[c] >= 1e30f ? 1e30f : 0.0f;
                color_clamped = true;
            }
        if (color_clamped)
            note(cc::format("fbx: {} has a color that is not finite and non-negative, clamped", label));
        auto light = sv::light::point(where);
        switch (l.type)
        {
        case bf::light_type::point:
            light = sv::light::point(where).candela(intensity);
            break;
        case bf::light_type::directional:
            light = sv::light::directional(direction.normalized()).lux(intensity);
            break;
        case bf::light_type::spot:
        {
            // FBX's cone angles are full angles, held to [0, 180] (a NaN to 0); sv's are half, inner never past outer.
            auto const full = [](f32 degrees) { return degrees >= 0.0f ? (degrees <= 180.0f ? degrees : 180.0f) : 0.0f; };
            auto const outer_degrees = full(l.outer_angle_degrees);
            auto const inner_degrees
                = full(l.inner_angle_degrees) < outer_degrees ? full(l.inner_angle_degrees) : outer_degrees;
            if (outer_degrees != l.outer_angle_degrees || inner_degrees != l.inner_angle_degrees)
                note(cc::format("fbx: {} has a cone of {} to {} degrees, imported as {} to {}", label,
                                l.inner_angle_degrees, l.outer_angle_degrees, inner_degrees, outer_degrees));
            auto const outer = tg::angle_f::make_from_degree(outer_degrees * 0.5f);
            auto const inner = tg::angle_f::make_from_degree(inner_degrees * 0.5f);
            light = sv::light::spot(where, direction.normalized(), outer, inner).candela(intensity);
            break;
        }
        case bf::light_type::area:
        case bf::light_type::volume:
            note(cc::format("fbx: {} is an area or volume light, which FBX gives no size for, so it was not imported",
                            label));
            return;
        }
        light.color(color);
        light.emission.casts_shadows = l.cast_shadows;

        if (l.type != bf::light_type::directional && l.decay != bf::light_decay::quadratic)
            note(cc::format(
                "fbx: {} has {} decay, imported with sv's physical inverse-square falloff", label,
                l.decay == bf::light_decay::none ? "no" : (l.decay == bf::light_decay::linear ? "linear" : "cubic")));

        out.lights.push_back({.id = l.name.empty() ? n.name : l.name, .light = light});
    }

    // the scene graph
    // ---------------------------------------------------------------------------------------------

    /// One forward pass, since babel hands the nodes parents first.
    void walk_nodes()
    {
        auto hidden_meshes = isize(0);
        auto hidden_lights = isize(0);
        auto any_inherit_mode = false;

        // FBX's VisibilityInheritance is on unless a file says otherwise, and neither ufbx nor babel carries it, so a hidden node hides its whole subtree.
        auto shown = cc::vector<bool>();
        shown.reserve(doc.nodes.size());
        for (auto i = isize(0); i < doc.nodes.size(); ++i)
        {
            auto const& n = doc.nodes[i];
            auto const parent = isize(int(n.parent));
            any_inherit_mode = any_inherit_mode || n.inherit != bf::inherit_mode::normal;
            shown.push_back(n.visible && (parent < 0 || shown[parent]));
            auto const visible = shown.back();

            // The root's local transform takes the space conversion, so an unflattened tree composes into sv's space too.
            auto local = affine_of(n.local);
            if (parent < 0)
                local = tg::compose(conversion, local);

            auto const slot = i32(out.nodes.size());
            out.nodes.push_back({.name = n.name,
                                 .parent = i32(parent),
                                 .transform = local,
                                 .first_mesh = i32(out.meshes.size()),
                                 .first_light = i32(out.lights.size())});

            // Flattened, babel's own world transforms carry the file's scale inheritance, which composing locals cannot.
            auto const mesh_placement
                = cfg.flatten_hierarchy ? tg::compose(conversion, n.geometry_to_world) : affine_of(n.geometry);
            auto const light_placement
                = cfg.flatten_hierarchy ? tg::compose(conversion, n.node_to_world) : tg::affine_transform3f();

            // A hidden node is helper or proxy content its author switched off, so its meshes and lights stay out.
            if (n.mesh != bf::mesh_index::invalid)
            {
                if (visible)
                    emit_meshes(n, mesh_placement);
                else
                    ++hidden_meshes;
            }
            if (n.light != bf::light_index::invalid)
            {
                if (visible)
                    emit_light(n, light_placement);
                else
                    ++hidden_lights;
            }

            out.nodes[slot].mesh_count = i32(out.meshes.size()) - out.nodes[slot].first_mesh;
            out.nodes[slot].light_count = i32(out.lights.size()) - out.nodes[slot].first_light;
        }

        if (hidden_meshes > 0)
            note(cc::format("fbx: {} node(s) placing a mesh are hidden, by their own Visibility or an ancestor's, so "
                            "their meshes were not imported",
                            hidden_meshes));
        if (hidden_lights > 0)
            note(cc::format("fbx: {} light(s) are hidden, by their node's Visibility or an ancestor's, so they were "
                            "not "
                            "imported",
                            hidden_lights));

        if (any_inherit_mode && !cfg.flatten_hierarchy)
            note("fbx: some nodes ignore or reshape their parent's scale, which composing `asset_node::transform` does "
                 "not reproduce; a flattened import does");
    }
};
} // namespace

cc::result<asset_data> impl::import_fbx(babel::fbx::data const& doc,
                                        asset_loader_config const& cfg,
                                        cc::string_view asset_name,
                                        uri_resolver resolve,
                                        cc::string_view base_uri,
                                        cc::vector<impl::asset_material_definition>& definitions)
{
    auto importer = fbx_importer{.doc = doc, .cfg = cfg, .resolve = resolve, .base_uri = base_uri};
    importer.out.name = asset_name.empty() ? cc::string("fbx") : cc::string(asset_name);
    importer.conversion = space_conversion_of(doc.settings);

    // babel's own issues first, so the report reads in the order the file was processed.
    for (auto const& i : doc.issues)
        importer.out.issues.push_back(i.message);

    if (doc.settings.is_left_handed())
        importer.note("fbx: the scene's axes are left-handed, so the conversion to sv's mirrors it; faces keep their "
                      "winding, which the tracer does not cull by");

    for (auto const& t : doc.textures)
        if (t.has_uv_transform)
            importer.note(cc::format("fbx: texture '{}' carries a uv transform, which is not applied", t.name));

    importer.build_materials();
    importer.walk_nodes();
    impl::disambiguate_light_ids(importer.out.lights);
    importer.note_textures(importer.unfetched, "fetched");
    if (!importer.refused.empty())
        importer.note(cc::format("fbx: {} texture(s) name a scheme, a drive, a network path, a device or a parent "
                                 "directory rather than a file beside the document, which is never fetched: '{}'{}",
                                 importer.refused.size(), importer.refused[0],
                                 importer.refused.size() > 1 ? ", ..." : ""));
    if (!importer.clamped_materials.empty())
        importer.note(cc::format("fbx: {} material(s) carry values that are not finite and non-negative, clamped: "
                                 "'{}'{}",
                                 importer.clamped_materials.size(), importer.clamped_materials[0],
                                 importer.clamped_materials.size() > 1 ? ", ..." : ""));
    if (!importer.packed_unusable.empty())
        importer.note(cc::format("fbx: {} material(s) connect a packed specular map that did not decode, so they take "
                                 "metalness 0 and roughness 0.5 rather than the SpecularColor constant: '{}'{}",
                                 importer.packed_unusable.size(), importer.packed_unusable[0],
                                 importer.packed_unusable.size() > 1 ? ", ..." : ""));
    importer.note_textures(importer.undecodable, "decoded");

    if (importer.fanned)
        importer.note("fbx: polygons were fanned into triangles; read with read_options::triangulate for an ear clip");

    if (importer.out.is_empty())
        return cc::error(cc::format("shaped-viewer: nothing to import from '{}'", importer.out.name));

    definitions = cc::move(importer.definitions);
    return cc::move(importer.out);
}
} // namespace sv
