#include <babel-serializer/geometry/gltf.hh>
#include <babel-serializer/image/image.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <shaped-viewer/asset/asset_loader.hh>
#include <shaped-viewer/asset/impl/asset_import.hh>
#include <shaped-viewer/material/material.hh>
#include <shaped-viewer/material/material_library.hh>
#include <shaped-viewer/material/material_type.hh>
#include <shaped-viewer/scene/mesh.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/linalg/mat.hh>
#include <typed-geometry/linalg/quat.hh>
#include <typed-geometry/transform/compose.hh>

// glTF 2.0 into sv's vocabulary.
//
// The mapping targets OPENPBR rather than pbr, because that is what makes the extensions worth having: transmission,
// ior, clearcoat and sheen land on `transmission_*`, `coat_*` and `fuzz_*` natively where pbr would discard them.
// babel does not interpret the KHR_materials_* extensions yet, so what actually crosses today is the core
// metallic-roughness set plus emission — and an issue names what was left behind.
//
// Textures ride on the MESH rather than on the material, as `mesh_texture`.
// That is what lets the importer stay CPU-side: a mesh texture travels as pixels, while a material binding would need
// an already-minted `texture_id` and therefore a device.

namespace sv
{
namespace
{
namespace bg = babel::gltf;

/// The uv attribute name a glTF TEXCOORD_n set is imported under.
/// `uv` for set 0, because that is what every material's default `uv_attribute` looks for.
[[nodiscard]] cc::string uv_name_of(i32 texcoord)
{
    return texcoord == 0 ? cc::string("uv") : cc::format("uv{}", texcoord);
}

[[nodiscard]] sg::sampler_address_mode address_of(bg::wrap_mode m)
{
    switch (m)
    {
    case bg::wrap_mode::clamp_to_edge:
        return sg::sampler_address_mode::clamp_edge;
    case bg::wrap_mode::mirrored_repeat:
        return sg::sampler_address_mode::mirror_repeat;
    case bg::wrap_mode::repeat:
        return sg::sampler_address_mode::repeat;
    }
    return sg::sampler_address_mode::repeat;
}

/// glTF's magnification / minification filters, of which sv keeps the two that are not a mip policy.
/// The mip filter is always linear here: a ray tracing hit samples an explicit level anyway, and glTF's
/// NEAREST_MIPMAP_* variants exist for a rasterizer's benefit.
[[nodiscard]] sg::sampler_filter filter_of(bg::filter f)
{
    switch (f)
    {
    case bg::filter::nearest:
    case bg::filter::nearest_mipmap_nearest:
    case bg::filter::nearest_mipmap_linear:
        return sg::sampler_filter::nearest;
    default:
        return sg::sampler_filter::linear;
    }
}

/// One decoded image expanded to tightly packed rgba8.
///
/// Every decode lands on rgba8 whatever the file held, because that is the one shape `texture_data` and the bindless
/// table agree on today.
/// A 16-bit PNG keeps its high byte and an f32 image is clamped, both of which lose precision the importer has nowhere
/// to put — block compression and wider formats are a `derived` recipe, not this.
[[nodiscard]] cc::result<cc::vector<byte>> to_rgba8(babel::image::image const& img)
{
    if (img.is_empty() || img.channels < 1 || img.channels > 4)
        return cc::error("gltf: image has no usable pixels");

    auto const pixel_count = isize(img.width) * isize(img.height);
    auto out = cc::vector<byte>::create_filled(pixel_count * 4, byte(255));

    auto const sample = [&](isize index) -> byte
    {
        switch (img.comp)
        {
        case babel::image::component::u8:
            return img.pixels[index];
        case babel::image::component::u16:
        {
            // Host-endian u16 pairs; the high byte is the 8-bit value.
            auto const lo = u32(img.pixels[index * 2]);
            auto const hi = u32(img.pixels[index * 2 + 1]);
            return byte((lo | (hi << 8)) >> 8);
        }
        case babel::image::component::f32:
        {
            auto const v = img.samples_f32()[index];
            auto const clamped = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            return byte(u32(clamped * 255.0f + 0.5f));
        }
        }
        return byte(0);
    };

    for (auto p = isize(0); p < pixel_count; ++p)
        for (auto c = 0; c < img.channels; ++c)
        {
            auto const value = sample(p * isize(img.channels) + c);
            // 1 channel is grey and 2 is grey+alpha, so the single value fills rgb rather than r alone.
            if (img.channels <= 2 && c == 0)
            {
                out[p * 4 + 0] = value;
                out[p * 4 + 1] = value;
                out[p * 4 + 2] = value;
            }
            else if (img.channels <= 2)
                out[p * 4 + 3] = value;
            else
                out[p * 4 + c] = value;
        }

    return cc::move(out);
}

/// Everything one import needs to carry between its passes.
struct gltf_importer
{
    bg::data const& doc;
    asset_loader_config const& cfg;

    asset_data out;

    /// one entry per glTF material, plus a trailing one for primitives naming none when any does
    cc::vector<impl::asset_material_definition> definitions;
    cc::vector<cc::vector<mesh_texture>> material_textures;
    bool has_default_material = false;

    /// decoded images, keyed by image index and color space — the same bytes read as sRGB and as linear are two
    /// textures, and must be, since the color space is part of the format the GPU samples through
    cc::map<u64, texture_data> decoded;

    /// guards against a malformed file whose node graph is not a tree
    cc::vector<u8> visited;

    void note(cc::string message) { out.issues.push_back(cc::move(message)); }

    // materials
    // ---------------------------------------------------------------------------------------------

    [[nodiscard]] cc::optional<texture_data> decode_image(bg::image_index index, bool srgb)
    {
        auto const key = u64(u32(int(index))) | (srgb ? (u64(1) << 32) : 0);
        if (auto const* const resident = decoded.get_ptr(key); resident != nullptr)
            return *resident;

        auto const* const img = doc.find(index);
        if (img == nullptr)
            return {};
        if (!img->resolved || img->data.empty())
        {
            note(cc::format("gltf: image {} was never resolved, so its texture is missing", int(index)));
            return {};
        }

        auto decoded_image = babel::image::read(img->data.span());
        if (decoded_image.has_error())
        {
            note(cc::format("gltf: image {} could not be decoded", int(index)));
            return {};
        }

        auto pixels = to_rgba8(decoded_image.value());
        if (pixels.has_error())
        {
            note(cc::format("gltf: image {} decoded to no usable pixels", int(index)));
            return {};
        }

        auto const format = srgb ? sg::pixel_format::rgba8_unorm_srgb : sg::pixel_format::rgba8_unorm;
        auto data = texture_data::create(cc::move(pixels.value()), format, decoded_image.value().width,
                                         decoded_image.value().height);
        decoded[key] = data;
        return data;
    }

    /// The sample `ref` describes, or nothing when it names no texture or the image was unusable.
    [[nodiscard]] cc::optional<texture_sample> sample_of(bg::texture_ref const& ref,
                                                         bool srgb,
                                                         channel_swizzle swizzle,
                                                         sample_transform transform)
    {
        auto const* const tex = doc.find(ref.texture);
        if (tex == nullptr)
            return {};

        auto data = decode_image(tex->source, srgb);
        if (!data.has_value())
            return {};

        auto sampler = sg::sampler();
        if (auto const* const s = doc.find(tex->sampler); s != nullptr)
        {
            sampler.mag_filter = filter_of(s->mag_filter);
            sampler.min_filter = filter_of(s->min_filter);
            sampler.address_u = address_of(s->wrap_s);
            sampler.address_v = address_of(s->wrap_t);
        }

        return texture_sample{.texture = cc::move(data.value()),
                              .uv_attribute = uv_name_of(ref.texcoord),
                              .sampler = sampler,
                              .swizzle = swizzle,
                              .transform = transform};
    }

    void bind_texture(cc::vector<mesh_texture>& into,
                      cc::string name,
                      bg::texture_ref const& ref,
                      bool srgb,
                      channel_swizzle swizzle,
                      sample_transform transform = {})
    {
        if (auto sample = sample_of(ref, srgb, swizzle, transform); sample.has_value())
            into.push_back({.name = cc::move(name), .source = cc::move(sample.value())});
    }

    /// The bindings and mesh textures one glTF material maps onto.
    void build_material(bg::material const& m, cc::string_view name)
    {
        auto bindings = cc::vector<material_attribute_binding>();
        auto textures = cc::vector<mesh_texture>();

        using binding = material_attribute_binding;
        bindings.push_back(binding::of(
            "base_color", tg::vec3f(m.base_color_factor[0], m.base_color_factor[1], m.base_color_factor[2])));
        bindings.push_back(binding::of("base_metalness", m.metallic_factor));
        bindings.push_back(binding::of("specular_roughness", m.roughness_factor));

        // `emission_luminance` multiplies the color, and its OpenPBR default is 0 — so an emissive glTF material has to
        // bind it or it emits nothing.
        // KHR_materials_emissive_strength would scale this; babel does not interpret it yet.
        if (m.emissive_factor != tg::vec3f::zero)
        {
            bindings.push_back(binding::of("emission_color", m.emissive_factor));
            bindings.push_back(binding::of("emission_luminance", 1.0f));
        }

        // OPAQUE binds nothing, leaving opacity at 1: baseColorFactor's alpha is not a transparency there.
        // MASK is a step, so it binds the threshold as well as the value it is stepped against.
        if (m.alpha != bg::alpha_mode::opaque)
            bindings.push_back(binding::of("opacity", m.base_color_factor[3]));
        if (m.alpha == bg::alpha_mode::mask)
            bindings.push_back(binding::of("alpha_cutoff", m.alpha_cutoff));

        if (cfg.import_textures)
        {
            // Base color is sRGB-encoded and its alpha is not, which is exactly what an sRGB texture format means — so
            // one upload serves both bindings.
            bind_texture(textures, "base_color", m.base_color_texture, true, channel_swizzle());
            if (m.alpha != bg::alpha_mode::opaque)
                bind_texture(textures, "opacity", m.base_color_texture, true,
                             channel_swizzle::of_channel(texture_channel::a));

            // The packing this whole swizzle exists for: one metallic-roughness map, bound twice over a single upload.
            bind_texture(textures, "base_metalness", m.metallic_roughness_texture, false,
                         channel_swizzle::of_channel(texture_channel::b));
            bind_texture(textures, "specular_roughness", m.metallic_roughness_texture, false,
                         channel_swizzle::of_channel(texture_channel::g));

            // `strength` is an affine remap of the sampled value, which is exactly what a sample transform is.
            bind_texture(textures, "occlusion", m.occlusion_texture, false,
                         channel_swizzle::of_channel(texture_channel::r),
                         sample_transform::of_strength(m.occlusion_strength));
            bind_texture(textures, "emission_color", m.emissive_texture, true, channel_swizzle());

            // A normal map stores `[0,1]` and means `[-1,1]`, and `normalTexture.scale` folds into the same two
            // numbers — so the whole of glTF's normal mapping is one transform and no special case.
            // Linear rather than sRGB: these are directions, not colors.
            bind_texture(textures, "normal", m.normal_texture, false, channel_swizzle(),
                         sample_transform::of_signed_normal(m.normal_scale));
        }

        // No library is touched here: the definition is what `acquire_asset_materials` mints, on the thread that owns
        // one — which is what lets everything above this run on a worker.
        definitions.push_back({.name = cc::string(name), .bindings = cc::move(bindings)});
        material_textures.push_back(cc::move(textures));
        out.materials.push_back({.name = cc::string(name)});
    }

    void build_materials()
    {
        if (!cfg.import_materials)
            return;

        for (auto i = isize(0); i < doc.materials.size(); ++i)
        {
            auto const& m = doc.materials[i];
            build_material(m, m.name.empty() ? cc::format("material{}", i) : cc::string(m.name));
        }

        // glTF's own default material, for primitives naming none: a fully rough, fully metallic white surface.
        // A trailing entry rather than a special case, so a primitive's material index always lands in the table — but
        // only when something actually needs it, since an asset listing a material no file named would be a lie.
        for (auto const& p : doc.primitives)
            if (doc.find(p.material) == nullptr)
            {
                build_material(bg::material{}, "default");
                has_default_material = true;
                break;
            }
    }

    /// The slot in `out.materials` a primitive's material index lands in, or -1 when materials are not imported.
    [[nodiscard]] isize slot_of(bg::material_index index) const
    {
        if (definitions.empty())
            return -1;

        auto const raw = isize(int(index));
        if (raw >= 0 && raw < doc.materials.size())
            return raw;
        return has_default_material ? definitions.size() - 1 : -1;
    }

    [[nodiscard]] cc::span<mesh_texture const> textures_of(isize slot) const
    {
        return slot >= 0 ? cc::span<mesh_texture const>(material_textures[slot]) : cc::span<mesh_texture const>();
    }

    // geometry
    // ---------------------------------------------------------------------------------------------

    /// The primitive's triangles as an index list, converting the two strip topologies on the way.
    /// Empty means "not a triangle topology", which the caller reports.
    [[nodiscard]] cc::vector<u32> triangle_indices(bg::primitive const& p, isize vertex_count) const
    {
        auto source = cc::vector<u32>();
        if (p.indices != bg::accessor_index::invalid)
        {
            auto read = doc.read_indices(p);
            if (read.has_error())
                return {};
            source = cc::move(read.value());
        }
        else
        {
            source.reserve(vertex_count);
            for (auto i = isize(0); i < vertex_count; ++i)
                source.push_back(u32(i));
        }

        if (p.mode == bg::primitive_mode::triangles)
            return source;

        auto out_indices = cc::vector<u32>();
        if (p.mode == bg::primitive_mode::triangle_strip)
        {
            // Every other triangle is wound backwards, and un-flipping it here is what keeps the whole mesh's facing
            // consistent for a renderer that culls or reads a geometric normal.
            for (auto i = isize(2); i < source.size(); ++i)
            {
                auto const flip = (i % 2) == 1;
                out_indices.push_back(source[i - 2]);
                out_indices.push_back(source[flip ? i : i - 1]);
                out_indices.push_back(source[flip ? i - 1 : i]);
            }
        }
        else if (p.mode == bg::primitive_mode::triangle_fan)
        {
            for (auto i = isize(2); i < source.size(); ++i)
            {
                out_indices.push_back(source[0]);
                out_indices.push_back(source[i - 1]);
                out_indices.push_back(source[i]);
            }
        }
        return out_indices;
    }

    /// One accessor's elements, packed — the always-safe read.
    /// `invalid`, an unresolved buffer or a component type this element size does not match all come back empty, which
    /// is what makes a quantized attribute a missing attribute rather than a failed import.
    template <class T>
    [[nodiscard]] cc::vector<T> elements_of(bg::accessor_index index) const
    {
        auto const* const a = doc.find(index);
        if (a == nullptr)
            return {};

        auto view = doc.view_of(*a);
        if (view.has_error())
            return {};

        auto elements = view.value().template read_elements<T>();
        if (elements.has_error())
            return {};
        return cc::move(elements.value());
    }

    /// The box a POSITION accessor states, which glTF requires of it — so this costs no payload byte.
    ///
    /// Empty when the file left it out, or stated it at the wrong arity, and `create_mesh` then scans the positions.
    /// Taking the file's word matters more than it looks: it is what lets a placeholder be drawn at the right size
    /// before the geometry has arrived at all.
    [[nodiscard]] cc::optional<tg::aabb3f> bounds_of(bg::primitive const& p) const
    {
        auto const* const accessor = doc.find(doc.find_attribute(p, "POSITION"));
        if (accessor == nullptr)
            return {};

        auto const low = doc.min_of(*accessor);
        auto const high = doc.max_of(*accessor);
        if (low.size() != 3 || high.size() != 3)
            return {};

        return tg::aabb3f(tg::pos3f(low[0], low[1], low[2]), tg::pos3f(high[0], high[1], high[2]));
    }

    /// The uv sets, tangent frames and handedness a primitive supplies, as sv mesh attributes.
    [[nodiscard]] cc::vector<mesh_attribute> attributes_of(bg::primitive const& p,
                                                           isize vertex_count,
                                                           cc::string_view mesh_name)
    {
        auto attributes = cc::vector<mesh_attribute>();

        for (auto const& a : doc.attributes_of(p))
        {
            if (!a.semantic.starts_with("TEXCOORD_"))
                continue;

            auto const uvs = elements_of<tg::vec2f>(a.accessor);
            if (uvs.size() != vertex_count)
            {
                note(cc::format("gltf: attribute {} is not two floats per vertex, so it is not imported", a.semantic));
                continue;
            }

            auto const set = a.semantic.subview({.offset = 9, .size = a.semantic.size() - 9});
            auto index = 0;
            for (auto const c : set)
                index = index * 10 + (c - '0');
            attributes.push_back(mesh_attribute::create(uv_name_of(index), attribute_frequency::per_vertex, uvs));
        }

        if (!cfg.frames.prefer_file)
            return attributes;

        auto const normals = elements_of<tg::vec3f>(doc.find_attribute(p, "NORMAL"));
        if (normals.size() != vertex_count)
            return attributes;

        // One unusable normal drops the whole attribute rather than being substituted per vertex: a made-up frame
        // beside real ones shows as a seam, while supplying none lets the hit shader's geometric fallback answer for
        // the whole mesh, which is correct everywhere.
        auto unusable = isize(0);
        for (auto const& n : normals)
            if (!impl::is_usable_normal(n))
                ++unusable;

        if (unusable > 0)
        {
            note(cc::format("gltf: '{}' has {} of {} normals that cannot be normalized, so no tangent frame is "
                            "imported and the geometric one is used instead",
                            mesh_name, unusable, vertex_count));
            return attributes;
        }

        // TANGENT is a vec4: xyz is the tangent, w the mirror bit no rotation can carry.
        auto const tangents = elements_of<tg::vec4f>(doc.find_attribute(p, "TANGENT"));
        auto const has_tangents = tangents.size() == vertex_count;

        auto frames = cc::vector<tg::quat_f>();
        auto handedness = cc::vector<f32>();
        frames.reserve(vertex_count);
        handedness.reserve(vertex_count);

        for (auto i = isize(0); i < vertex_count; ++i)
        {
            if (has_tangents)
            {
                auto const& t = tangents[i];
                frames.push_back(impl::tangent_frame_of(normals[i], tg::vec3f(t[0], t[1], t[2])));
                handedness.push_back(t[3] < 0.0f ? -1.0f : 1.0f);
            }
            else
            {
                frames.push_back(impl::tangent_frame_of(normals[i]));
                handedness.push_back(1.0f);
            }
        }

        attributes.push_back(mesh_attribute::create("tangent_frame", attribute_frequency::per_vertex, cc::move(frames)));
        attributes.push_back(
            mesh_attribute::create("tangent_handedness", attribute_frequency::per_vertex, cc::move(handedness)));
        return attributes;
    }

    // the scene graph
    // ---------------------------------------------------------------------------------------------

    [[nodiscard]] static tg::affine_transform3f local_transform_of(bg::node const& n)
    {
        if (n.has_matrix)
        {
            // A glTF `matrix` is a full 4x4, of which an affine transform is the upper 3x4 — the last row is (0,0,0,1)
            // for every transform the spec allows, so nothing is dropped.
            auto const linear = tg::mat3f::make_from_cols(tg::vec3f(n.matrix[0, 0], n.matrix[0, 1], n.matrix[0, 2]),
                                                          tg::vec3f(n.matrix[1, 0], n.matrix[1, 1], n.matrix[1, 2]),
                                                          tg::vec3f(n.matrix[2, 0], n.matrix[2, 1], n.matrix[2, 2]));
            auto const translation = tg::vec3f(n.matrix[3, 0], n.matrix[3, 1], n.matrix[3, 2]);
            return tg::compose(tg::affine_transform3f::make_translation(translation),
                               tg::affine_transform3f::make_from_linear_mat(linear));
        }

        // The spec's order: scale, then rotate, then translate.
        auto const s = tg::affine_transform3f::make_scaling(n.scale);
        auto const r = tg::affine_transform3f::make_rotation(n.rotation);
        auto const t = tg::affine_transform3f::make_translation(n.translation);
        return tg::compose(t, tg::compose(r, s));
    }

    void emit_mesh(bg::mesh_index index, tg::affine_transform3f const& placement)
    {
        auto const* const m = doc.find(index);
        if (m == nullptr)
            return;

        auto const primitives = doc.primitives_of(*m);
        auto const base_name = m->name.empty() ? cc::format("mesh{}", int(index)) : cc::string(m->name);

        for (auto pi = isize(0); pi < primitives.size(); ++pi)
        {
            auto const& p = primitives[pi];

            // One mesh per (geometry, material). A glTF mesh with three primitives becomes three meshes, and the name
            // only carries the primitive index when there is something to disambiguate.
            auto const name = primitives.size() == 1 ? base_name : cc::format("{}.{}", base_name, pi);

            if (cfg.include_mesh && !cfg.include_mesh(name))
                continue;

            if (p.mode != bg::primitive_mode::triangles && p.mode != bg::primitive_mode::triangle_strip
                && p.mode != bg::primitive_mode::triangle_fan)
            {
                note(cc::format("gltf: '{}' is not a triangle topology, so it is not imported", name));
                continue;
            }

            auto positions = elements_of<tg::pos3f>(doc.find_attribute(p, "POSITION"));
            if (positions.empty())
            {
                note(cc::format("gltf: '{}' has no readable POSITION accessor — quantized and normalized-integer "
                                "positions are not supported yet",
                                name));
                continue;
            }

            auto const vertex_count = positions.size();
            auto indices = triangle_indices(p, vertex_count);
            if (indices.empty())
            {
                note(cc::format("gltf: '{}' has no readable triangles", name));
                continue;
            }

            auto attributes = attributes_of(p, vertex_count, name);
            auto const slot = slot_of(p.material);

            auto textures = cc::vector<mesh_texture>();
            for (auto const& t : textures_of(slot))
                textures.push_back(t);

            if (slot >= 0)
                out.materials[slot].meshes.push_back(i32(out.meshes.size()));

            out.meshes.push_back(
                {.name = name,
                 .geometry = triangle_geometry::create_from_indexed_triangles(cc::move(positions), cc::move(indices)),
                 .attributes = cc::move(attributes),
                 .transform = placement,
                 // Left invalid on purpose: the slot it belongs to points every mesh it covers at the real id
                 // once the library has minted one.
                 .textures = cc::move(textures),
                 .bounds = bounds_of(p)});
        }
    }

    void visit(bg::node_index index, i32 parent, tg::affine_transform3f const& parent_world)
    {
        auto const raw = isize(int(index));
        auto const* const n = doc.find(index);
        if (n == nullptr || visited[raw] != 0)
            return;
        visited[raw] = 1;

        auto const local = local_transform_of(*n);
        auto const world = tg::compose(parent_world, local);

        auto const slot = i32(out.nodes.size());
        out.nodes.push_back(
            {.name = n->name, .parent = parent, .transform = local, .first_mesh = i32(out.meshes.size()), .mesh_count = 0});

        if (n->mesh != bg::mesh_index::invalid)
            emit_mesh(n->mesh, cfg.flatten_hierarchy ? world : local);

        // Counted before the children run, since their meshes belong to them and not to this node.
        out.nodes[slot].mesh_count = i32(out.meshes.size()) - out.nodes[slot].first_mesh;

        for (auto const child : doc.children_of(*n))
            visit(child, slot, world);
    }

    void walk_scene()
    {
        visited = cc::vector<u8>::create_filled(doc.nodes.size(), u8(0));

        if (doc.scenes.empty())
        {
            // A library file: no scene places anything, so every mesh is imported once at the origin.
            note("gltf: the document declares no scene, so every mesh was imported at identity");
            for (auto i = isize(0); i < doc.meshes.size(); ++i)
                emit_mesh(bg::mesh_index(int(i)), tg::affine_transform3f());
            return;
        }

        auto const* scene = doc.find(doc.default_scene);
        if (scene == nullptr)
            scene = &doc.scenes[0];

        for (auto const n : doc.nodes_of(*scene))
            visit(n, -1, tg::affine_transform3f());
    }
};
} // namespace

cc::result<asset_data> impl::import_gltf(babel::gltf::data const& doc,
                                         asset_loader_config const& cfg,
                                         cc::string_view asset_name,
                                         cc::vector<impl::asset_material_definition>& definitions)
{
    auto importer = gltf_importer{.doc = doc, .cfg = cfg};
    importer.out.name = asset_name.empty() ? cc::string("gltf") : cc::string(asset_name);

    // babel's own issues first, so the report reads in the order the file was processed.
    for (auto const& i : doc.issues)
        importer.out.issues.push_back(i.message);

    importer.build_materials();
    importer.walk_scene();

    if (importer.out.meshes.empty())
        return cc::error(cc::format("shaped-viewer: nothing to import from '{}'", importer.out.name));

    definitions = cc::move(importer.definitions);
    return cc::move(importer.out);
}
} // namespace sv
