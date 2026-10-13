#include <babel-serializer/geometry/fbx.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/fixed_array.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/set.hh>
#include <clean-core/streams/span_stream.hh>
#include <clean-core/string/format.hh>
#include <typed-geometry/linalg/mat.hh>
#include <typed-geometry/transform/compose.hh>
#include <ufbx.h>

// The parse is ufbx's; this file copies its scene out into babel's structure and frees it again.
//
// ufbx is told to touch nothing outside the input: no external files, no coordinate or unit conversion, no generated normals, geometry transforms and inherit modes preserved as the file states them.
// What it does interpret — the pivot chain, the mapping modes, the inherit modes in `node_to_world` — is what fbx.hh promises, and nothing beyond it.
//
// ufbx_scene is the only owner of everything read here, so every string and array is copied before it is freed.

namespace babel::impl
{
namespace
{
namespace bf = babel::fbx;

/// How deep a node may nest before ufbx refuses the file.
/// A cap rather than a stack budget: past this depth the file is malformed rather than large, and every consumer of a hierarchy walks it somehow.
constexpr u32 fbx_node_depth_limit = 4096;

/// The largest count a mesh may carry, since babel::fbx stores its indices as `i32`.
constexpr size_t fbx_max_count = 0x7fffffff;

/// The automatic memory budget: 32 times the input plus 64 MiB, under a hard 4 GiB, or 1 GiB for a plain stream.
/// Bistro's exterior, 114 MB of binary FBX, builds a 630 MB scene, which leaves room for files that compress their arrays far harder than Maya does.
constexpr i64 fbx_memory_per_input_byte = 32;
constexpr i64 fbx_memory_floor = i64(64) << 20;
constexpr i64 fbx_memory_cap = i64(4) << 30;
constexpr i64 fbx_memory_unsized = i64(1) << 30;

/// The most corners one polygon may have: ufbx's ear clip is quadratic in it, and no modeling tool writes more.
constexpr u32 fbx_max_face_corners = 4096;

[[nodiscard]] i64 memory_limit_of(i64 requested, i64 input_size)
{
    if (requested > 0)
        return requested;
    if (input_size < 0)
        return fbx_memory_unsized;
    auto const scaled = input_size * fbx_memory_per_input_byte + fbx_memory_floor;
    return scaled < fbx_memory_cap ? scaled : fbx_memory_cap;
}

/// Whether `f` is a usable scene unit, in meters: between a nanometer and a million kilometers.
/// Outside it a unit is damage rather than intent, and its f32 would collapse a scene to 0 or blow it up to infinity.
[[nodiscard]] bool is_sane_unit(f64 f)
{
    return f >= 1e-9 && f <= 1e9;
}

[[nodiscard]] cc::string to_string(ufbx_string const& s)
{
    return cc::string(cc::string_view(s.data, isize(s.length)));
}

[[nodiscard]] tg::vec3f to_vec3(ufbx_vec3 const& v)
{
    return tg::vec3f(f32(v.x), f32(v.y), f32(v.z));
}

[[nodiscard]] tg::quat_f to_quat(ufbx_quat const& q)
{
    return tg::quat_f(f32(q.x), f32(q.y), f32(q.z), f32(q.w));
}

[[nodiscard]] bf::transform to_transform(ufbx_transform const& t)
{
    return {.translation = to_vec3(t.translation), .rotation = to_quat(t.rotation), .scale = to_vec3(t.scale)};
}

/// ufbx's 3x4 matrix — three linear columns and a translation — as the affine transform it is.
[[nodiscard]] tg::affine_transform3f to_affine(ufbx_matrix const& m)
{
    auto const linear = tg::mat3f::make_from_cols(to_vec3(m.cols[0]), to_vec3(m.cols[1]), to_vec3(m.cols[2]));
    return tg::compose(tg::affine_transform3f::make_translation(to_vec3(m.cols[3])),
                       tg::affine_transform3f::make_from_linear_mat(linear));
}

[[nodiscard]] cc::optional<bf::axis> to_axis(ufbx_coordinate_axis a)
{
    switch (a)
    {
    case UFBX_COORDINATE_AXIS_POSITIVE_X:
        return bf::axis::positive_x;
    case UFBX_COORDINATE_AXIS_NEGATIVE_X:
        return bf::axis::negative_x;
    case UFBX_COORDINATE_AXIS_POSITIVE_Y:
        return bf::axis::positive_y;
    case UFBX_COORDINATE_AXIS_NEGATIVE_Y:
        return bf::axis::negative_y;
    case UFBX_COORDINATE_AXIS_POSITIVE_Z:
        return bf::axis::positive_z;
    case UFBX_COORDINATE_AXIS_NEGATIVE_Z:
        return bf::axis::negative_z;
    default:
        return {};
    }
}

[[nodiscard]] bf::property_type to_property_type(ufbx_prop_type t)
{
    switch (t)
    {
    case UFBX_PROP_BOOLEAN:
        return bf::property_type::boolean;
    case UFBX_PROP_INTEGER:
        return bf::property_type::integer;
    case UFBX_PROP_NUMBER:
        return bf::property_type::number;
    case UFBX_PROP_VECTOR:
    case UFBX_PROP_TRANSLATION:
    case UFBX_PROP_ROTATION:
    case UFBX_PROP_SCALING:
        return bf::property_type::vector;
    case UFBX_PROP_COLOR:
        return bf::property_type::color;
    case UFBX_PROP_COLOR_WITH_ALPHA:
        return bf::property_type::color_with_alpha;
    case UFBX_PROP_STRING:
        return bf::property_type::string;
    default:
        return bf::property_type::other;
    }
}

[[nodiscard]] bf::property to_property(ufbx_prop const& p)
{
    return {.name = to_string(p.name),
            .type = to_property_type(p.type),
            .value = tg::vec4f(f32(p.value_vec4.x), f32(p.value_vec4.y), f32(p.value_vec4.z), f32(p.value_vec4.w)),
            .value_int = p.value_int,
            .text = to_string(p.value_str)};
}

/// The stream ufbx reads through: `cc::read_stream::read` behind ufbx's C callback.
struct stream_context
{
    cc::read_stream* in = nullptr;
    cc::string error;
};

size_t read_from_stream(void* user, void* dst, size_t size)
{
    auto* const ctx = static_cast<stream_context*>(user);
    auto const n = ctx->in->read(cc::span<byte>(static_cast<byte*>(dst), isize(size)));
    if (n.has_error())
    {
        ctx->error = cc::format("{}", n.error());
        return SIZE_MAX;
    }
    return size_t(n.value());
}

/// ufbx's thread-pool hook, on a `cc::async_scheduler`.
/// ufbx hands over batches of task indices by group and later waits for a group, so each task becomes a node scheduled at once.
/// A wait drives the group's nodes to completion with the calling thread taking part, which is also what makes a pool with no workers finish them.
struct scheduler_pool
{
    cc::async_scheduler* scheduler = nullptr;
    cc::fixed_array<cc::vector<cc::shared_async<cc::unit>>, UFBX_THREAD_GROUP_COUNT> running = {};
};

void run_on_scheduler(void* user, ufbx_thread_pool_context ctx, u32 group, u32 start_index, u32 count)
{
    auto& pool = *static_cast<scheduler_pool*>(user);
    for (auto index = start_index; index < start_index + count; ++index)
    {
        auto task = cc::make_async_lazy(
            [ctx, index]
            {
                ufbx_thread_pool_run_task(ctx, index);
                return cc::unit{};
            });
        task->schedule_on(*pool.scheduler);
        pool.running[isize(group)].push_back(cc::move(task));
    }
}

void wait_on_scheduler(void* user, ufbx_thread_pool_context ctx, u32 group, u32 max_index)
{
    CC_UNUSED(ctx);
    CC_UNUSED(max_index);
    auto& pool = *static_cast<scheduler_pool*>(user);
    for (auto const& task : pool.running[isize(group)])
        pool.scheduler->participate_until_ready(*task);
    pool.running[isize(group)].clear();
}

/// One mesh's copy, kept apart until every mesh is done, so issues land in file order whichever mesh finished first.
struct mesh_result
{
    bf::mesh mesh;
    cc::vector<bf::issue> issues;

    /// set when the mesh cannot be represented, which fails the read
    cc::string error;
};

/// Copies one ufbx scene into a babel::fbx::data.
struct fbx_converter
{
    ufbx_scene const& scene;
    bf::read_options opts;

    bf::data out;

    /// set when the scene cannot be represented, which fails the read
    cc::string error;

    /// the embedded blobs already pinned, by the address ufbx holds them at
    cc::map<isize, cc::pinned_data<byte const>> embedded;

    void note(bf::issue_kind kind, cc::string message)
    {
        out.issues.push_back({.kind = kind, .message = cc::move(message)});
    }

    // the file and its settings
    // ---------------------------------------------------------------------------------------------

    void convert_info()
    {
        auto const& meta = scene.metadata;
        out.info = {.version = meta.version,
                    .encoding = meta.ascii ? bf::encoding::ascii : bf::encoding::binary,
                    .creator = to_string(meta.creator),
                    .application = to_string(meta.original_application.name),
                    .application_version = to_string(meta.original_application.version)};

        auto const& s = scene.settings;
        auto const right = to_axis(s.axes.right);
        auto const up = to_axis(s.axes.up);
        auto const front = to_axis(s.axes.front);

        // The three must name X, Y and Z once each, in either sign; anything else is no coordinate system at all.
        auto valid_axes = right.has_value() && up.has_value() && front.has_value();
        if (valid_axes)
        {
            auto const r = int(right.value()) / 2;
            auto const u = int(up.value()) / 2;
            auto const f = int(front.value()) / 2;
            valid_axes = r != u && u != f && r != f;
        }

        out.settings = {.unit_meters = f64(s.unit_meters),
                        .original_unit_meters = f64(s.original_unit_meters),
                        .frames_per_second = s.frames_per_second};
        if (valid_axes)
        {
            out.settings.right = right.value();
            out.settings.up = up.value();
            out.settings.front = front.value();
        }
        else
            note(bf::issue_kind::malformed, "fbx: the scene's axes do not name X, Y and Z once each, so FBX's default "
                                            "(+X right, +Y up, +Z front) stands in for them");

        if (!is_sane_unit(out.settings.unit_meters))
        {
            note(bf::issue_kind::malformed, cc::format("fbx: the scene's unit is {} meters, outside a nanometer to a "
                                                       "million kilometers, so FBX's default of a centimeter stands in",
                                                       out.settings.unit_meters));
            out.settings.unit_meters = 0.01;
        }
        if (!is_sane_unit(out.settings.original_unit_meters))
            out.settings.original_unit_meters = out.settings.unit_meters;

        // Deduplicated by ufbx: each kind is one warning carrying a count, so the list stays short on a damaged file.
        for (auto i = size_t(0); i < meta.warnings.count; ++i)
        {
            auto const& w = meta.warnings.data[i];
            auto const times = w.count > 1 ? cc::format(" ({} times)", w.count) : cc::string();
            note(bf::issue_kind::malformed, cc::format("fbx: {}{}", to_string(w.description), times));
        }
    }

    /// What the file carries that nothing in babel::fbx holds, each named once.
    void note_unsupported()
    {
        // `what` reads after the count; `consequence` says what the caller gets instead, when that is not obvious.
        auto const skip = [&](size_t count, cc::string_view what, cc::string_view consequence)
        {
            if (count > 0)
                note(bf::issue_kind::unsupported, cc::format("fbx: {} {} not read{}", count, what, consequence));
        };

        skip(scene.cameras.count, "camera(s) are", "");
        skip(scene.skin_deformers.count, "skin deformer(s) are", ", so skinned meshes stay in their bind pose");
        skip(scene.blend_deformers.count, "blend shape deformer(s) are", "");
        skip(scene.cache_deformers.count, "vertex cache deformer(s) are", "");
        skip(scene.nurbs_curves.count + scene.nurbs_surfaces.count + scene.line_curves.count, "curves or surfaces are",
             "");
        skip(scene.procedural_geometries.count, "procedural geometry element(s) are", "");

        auto animated = size_t(0);
        for (auto i = size_t(0); i < scene.anim_layers.count; ++i)
            animated += scene.anim_layers.data[i]->anim_values.count;
        skip(animated, "animated value(s) are", ", so every node stands at its static transform");
    }

    // materials and textures
    // ---------------------------------------------------------------------------------------------

    void convert_textures()
    {
        out.textures.reserve(isize(scene.textures.count));
        for (auto i = size_t(0); i < scene.textures.count; ++i)
        {
            auto const& t = *scene.textures.data[i];
            auto texture
                = bf::texture{.name = to_string(t.name),
                              .relative_filename = to_string(t.relative_filename),
                              .absolute_filename = to_string(t.absolute_filename),
                              .wrap_u = t.wrap_u == UFBX_WRAP_CLAMP ? bf::wrap_mode::clamp : bf::wrap_mode::repeat,
                              .wrap_v = t.wrap_v == UFBX_WRAP_CLAMP ? bf::wrap_mode::clamp : bf::wrap_mode::repeat,
                              .uv_set = to_string(t.uv_set),
                              .has_uv_transform = t.has_uv_transform,
                              .uv_transform = to_transform(t.uv_transform),
                              .is_file = t.type == UFBX_TEXTURE_FILE};

            // ufbx points every texture of one embedded Video at the same blob, so the pin is keyed on that pointer.
            if (t.content.size > 0)
            {
                auto const key = reinterpret_cast<isize>(t.content.data);
                if (auto const* const shared = embedded.get_ptr(key); shared != nullptr)
                    texture.content = *shared;
                else
                {
                    texture.content = cc::pinned_data<byte const>(cc::pinned_data<byte>::create_copy_of(
                        cc::span<byte const>(static_cast<byte const*>(t.content.data), isize(t.content.size))));
                    embedded[key] = texture.content;
                }
            }

            if (!texture.is_file)
                note(bf::issue_kind::unsupported, cc::format("fbx: texture '{}' is layered, procedural or a shader, "
                                                             "which carries no file of its own",
                                                             texture.name));

            out.textures.push_back(cc::move(texture));
        }
    }

    void convert_materials()
    {
        out.materials.reserve(isize(scene.materials.count));
        for (auto i = size_t(0); i < scene.materials.count; ++i)
        {
            auto const& m = *scene.materials.data[i];
            auto material = bf::material{.name = to_string(m.name), .shading_model = to_string(m.shading_model_name)};

            // The file's own properties first, then the template defaults it did not override — what the FBX SDK would report.
            // That is what a reader of `EmissiveFactor` expects when the file left it out.
            // The names the file wrote, looked up as a set so the merge stays linear in the property count.
            auto written = cc::set<cc::string_view>();
            for (auto p = size_t(0); p < m.props.props.count; ++p)
            {
                auto const& own = m.props.props.data[p];
                material.properties.push_back(to_property(own));
                written.insert(cc::string_view(own.name.data, isize(own.name.length)));
            }
            if (m.props.defaults != nullptr)
                for (auto p = size_t(0); p < m.props.defaults->props.count; ++p)
                {
                    auto const& d = m.props.defaults->props.data[p];
                    if (!written.contains(cc::string_view(d.name.data, isize(d.name.length))))
                        material.properties.push_back(to_property(d));
                }

            for (auto t = size_t(0); t < m.textures.count; ++t)
            {
                auto const& mt = m.textures.data[t];
                material.textures.push_back(
                    {.property = to_string(mt.material_prop), .texture = bf::texture_index(int(mt.texture->typed_id))});
            }

            out.materials.push_back(cc::move(material));
        }
    }

    // meshes
    // ---------------------------------------------------------------------------------------------

    /// One ufbx attribute in babel's shape, its indices taken through `corners` — the source corner each output corner copies, which is the identity unless the mesh was triangulated.
    template <class T, class Attrib, class Convert>
    [[nodiscard]] static bf::corner_attribute<T> convert_attribute(Attrib const& a,
                                                                   cc::span<u32 const> corners,
                                                                   Convert&& convert)
    {
        auto out_attr = bf::corner_attribute<T>();
        if (!a.exists)
            return out_attr;

        out_attr.values.reserve(isize(a.values.count));
        for (auto i = size_t(0); i < a.values.count; ++i)
            out_attr.values.push_back(convert(a.values.data[i]));

        out_attr.indices.reserve(corners.size());
        for (auto const c : corners)
            out_attr.indices.push_back(i32(a.indices.data[c]));
        return out_attr;
    }

    /// Whether every count of `m` fits the `i32` indices babel::fbx stores — a corner, a face, a value of any attribute.
    [[nodiscard]] bool fits_indices(ufbx_mesh const& m) const
    {
        auto largest = m.num_indices;
        auto const consider = [&](size_t count) { largest = count > largest ? count : largest; };
        consider(m.faces.count);
        consider(opts.triangulate ? m.num_triangles * 3 : m.num_indices);
        consider(m.vertex_position.values.count);
        consider(m.vertex_normal.values.count);
        consider(m.vertex_tangent.values.count);
        consider(m.vertex_bitangent.values.count);
        for (auto i = size_t(0); i < m.uv_sets.count; ++i)
            consider(m.uv_sets.data[i].vertex_uv.values.count);
        for (auto i = size_t(0); i < m.color_sets.count; ++i)
            consider(m.color_sets.data[i].vertex_color.values.count);
        return largest <= fbx_max_count;
    }

    /// Reads `scene` and `opts` and writes nothing else, so meshes can be copied in parallel.
    [[nodiscard]] mesh_result convert_mesh(ufbx_mesh const& m) const
    {
        auto result = mesh_result();
        auto& mesh = result.mesh;
        mesh.name = to_string(m.name);
        auto const note = [&](bf::issue_kind kind, cc::string message)
        { result.issues.push_back({.kind = kind, .message = cc::move(message)}); };

        if (!fits_indices(m))
        {
            result.error
                = cc::format("fbx: mesh '{}' has more corners, faces or values than an i32 index reaches", mesh.name);
            return result;
        }

        // The source corner of every output corner, and the faces over them.
        auto corners = cc::vector<u32>();
        auto face_sources = cc::vector<u32>(); // the source face of every output face, for its material
        if (opts.triangulate)
        {
            // The scratch holds one face's triangles, sized by the largest face the ear clip is allowed to see.
            auto largest = u32(3);
            for (auto f = size_t(0); f < m.faces.count; ++f)
            {
                auto const corners_of = m.faces.data[f].num_indices;
                if (corners_of <= babel::impl::fbx_max_face_corners && corners_of > largest)
                    largest = corners_of;
            }
            auto scratch = cc::vector<u32>::create_filled(isize(largest - 2) * 3, 0u);
            auto dropped = isize(0);
            auto oversized = isize(0);
            corners.reserve(isize(m.num_triangles) * 3);
            for (auto f = size_t(0); f < m.faces.count; ++f)
            {
                auto const face = m.faces.data[f];
                if (face.num_indices < 3)
                {
                    ++dropped;
                    continue;
                }
                if (face.num_indices > babel::impl::fbx_max_face_corners)
                {
                    ++oversized;
                    continue;
                }

                auto const triangles = ufbx_triangulate_face(scratch.data(), size_t(scratch.size()), &m, face);
                for (auto t = u32(0); t < triangles; ++t)
                {
                    mesh.faces.push_back({.first_corner = i32(corners.size()), .corner_count = 3});
                    face_sources.push_back(u32(f));
                    for (auto k = u32(0); k < 3; ++k)
                        corners.push_back(scratch[isize(t * 3 + k)]);
                }
            }

            if (oversized > 0)
                note(bf::issue_kind::unsupported, cc::format("fbx: mesh '{}' has {} face(s) of more than {} corners, "
                                                             "dropped rather than ear-clipped",
                                                             mesh.name, oversized, babel::impl::fbx_max_face_corners));
            if (dropped > 0)
                note(bf::issue_kind::unsupported, cc::format("fbx: mesh '{}' has {} point or line face(s), dropped by "
                                                             "triangulation",
                                                             mesh.name, dropped));
        }
        else
        {
            corners.reserve(isize(m.num_indices));
            for (auto c = size_t(0); c < m.num_indices; ++c)
                corners.push_back(u32(c));
            for (auto f = size_t(0); f < m.faces.count; ++f)
            {
                auto const face = m.faces.data[f];
                mesh.faces.push_back({.first_corner = i32(face.index_begin), .corner_count = i32(face.num_indices)});
                face_sources.push_back(u32(f));
            }
        }

        auto const pos = [](ufbx_vec3 const& v) { return tg::pos3f(f32(v.x), f32(v.y), f32(v.z)); };
        auto const vec2 = [](ufbx_vec2 const& v) { return tg::vec2f(f32(v.x), f32(v.y)); };
        auto const vec3 = [](ufbx_vec3 const& v) { return to_vec3(v); };
        auto const vec4 = [](ufbx_vec4 const& v) { return tg::vec4f(f32(v.x), f32(v.y), f32(v.z), f32(v.w)); };

        mesh.positions = convert_attribute<tg::pos3f>(m.vertex_position, corners, pos);
        mesh.normals = convert_attribute<tg::vec3f>(m.vertex_normal, corners, vec3);
        mesh.tangents = convert_attribute<tg::vec3f>(m.vertex_tangent, corners, vec3);
        mesh.bitangents = convert_attribute<tg::vec3f>(m.vertex_bitangent, corners, vec3);

        for (auto i = size_t(0); i < m.uv_sets.count; ++i)
        {
            auto const& set = m.uv_sets.data[i];
            mesh.uv_sets.push_back(
                {.name = to_string(set.name), .uvs = convert_attribute<tg::vec2f>(set.vertex_uv, corners, vec2)});
        }
        for (auto i = size_t(0); i < m.color_sets.count; ++i)
        {
            auto const& set = m.color_sets.data[i];
            mesh.color_sets.push_back(
                {.name = to_string(set.name), .colors = convert_attribute<tg::vec4f>(set.vertex_color, corners, vec4)});
        }

        // Kept only when it says something: a single-slot mesh is every face at slot 0.
        auto multi_slot = false;
        for (auto i = size_t(0); i < m.face_material.count; ++i)
            multi_slot = multi_slot || m.face_material.data[i] != 0;
        if (multi_slot)
        {
            mesh.face_materials.reserve(face_sources.size());
            for (auto const f : face_sources)
                mesh.face_materials.push_back(i32(m.face_material.data[f]));
        }

        return result;
    }

    /// Every mesh, on `opts.scheduler` when there is one, and then in file order into `out`.
    void convert_meshes()
    {
        auto const count = isize(scene.meshes.count);
        auto results = cc::vector<mesh_result>::create_defaulted(count);
        if (opts.scheduler == nullptr)
        {
            for (auto i = isize(0); i < count; ++i)
                results[i] = convert_mesh(*scene.meshes.data[i]);
        }
        else
        {
            auto tasks = cc::vector<cc::shared_async<cc::unit>>();
            tasks.reserve(count);
            for (auto i = isize(0); i < count; ++i)
            {
                auto task = cc::make_async_lazy(
                    [this, &results, i]
                    {
                        results[i] = convert_mesh(*scene.meshes.data[i]);
                        return cc::unit{};
                    });
                task->schedule_on(*opts.scheduler);
                tasks.push_back(cc::move(task));
            }
            for (auto const& task : tasks)
                opts.scheduler->participate_until_ready(*task);
        }

        out.meshes.reserve(count);
        for (auto& r : results)
        {
            for (auto& i : r.issues)
                out.issues.push_back(cc::move(i));
            if (error.empty() && !r.error.empty())
                error = cc::move(r.error);
            out.meshes.push_back(cc::move(r.mesh));
        }
    }

    // lights
    // ---------------------------------------------------------------------------------------------

    void convert_lights()
    {
        out.lights.reserve(isize(scene.lights.count));
        for (auto i = size_t(0); i < scene.lights.count; ++i)
        {
            auto const& l = *scene.lights.data[i];

            auto type = bf::light_type::point;
            switch (l.type)
            {
            case UFBX_LIGHT_POINT:
                type = bf::light_type::point;
                break;
            case UFBX_LIGHT_DIRECTIONAL:
                type = bf::light_type::directional;
                break;
            case UFBX_LIGHT_SPOT:
                type = bf::light_type::spot;
                break;
            case UFBX_LIGHT_AREA:
                type = bf::light_type::area;
                break;
            default:
                type = bf::light_type::volume;
                break;
            }

            auto decay = bf::light_decay::none;
            switch (l.decay)
            {
            case UFBX_LIGHT_DECAY_LINEAR:
                decay = bf::light_decay::linear;
                break;
            case UFBX_LIGHT_DECAY_QUADRATIC:
                decay = bf::light_decay::quadratic;
                break;
            case UFBX_LIGHT_DECAY_CUBIC:
                decay = bf::light_decay::cubic;
                break;
            default:
                decay = bf::light_decay::none;
                break;
            }

            out.lights.push_back({.name = to_string(l.name),
                                  .type = type,
                                  .decay = decay,
                                  .color = to_vec3(l.color),
                                  .intensity = f32(l.intensity),
                                  .local_direction = to_vec3(l.local_direction),
                                  .inner_angle_degrees = f32(l.inner_angle),
                                  .outer_angle_degrees = f32(l.outer_angle),
                                  .area_shape_is_sphere = l.area_shape == UFBX_LIGHT_AREA_SHAPE_SPHERE,
                                  .cast_light = l.cast_light,
                                  .cast_shadows = l.cast_shadows});
        }
    }

    // nodes
    // ---------------------------------------------------------------------------------------------

    /// The hierarchy, parents first: a breadth-first walk from the root, so a parent's index is always below its children's whatever order ufbx happens to keep.
    void convert_nodes()
    {
        auto order = cc::vector<ufbx_node const*>();
        order.reserve(isize(scene.nodes.count));
        order.push_back(scene.root_node);

        // ufbx's node id to the index it lands at in `out.nodes`
        auto index_of = cc::vector<i32>::create_filled(isize(scene.nodes.count), -1);
        index_of[isize(scene.root_node->typed_id)] = 0;

        for (auto i = isize(0); i < order.size(); ++i)
            for (auto c = size_t(0); c < order[i]->children.count; ++c)
            {
                auto const* const child = order[i]->children.data[c];
                index_of[isize(child->typed_id)] = i32(order.size());
                order.push_back(child);
            }

        if (order.size() != isize(scene.nodes.count))
            note(bf::issue_kind::malformed, cc::format("fbx: {} node(s) are not reachable from the root and were not "
                                                       "read",
                                                       isize(scene.nodes.count) - order.size()));

        out.nodes.reserve(order.size());
        for (auto const* const n : order)
        {
            auto node = bf::node{.name = to_string(n->name),
                                 .parent = n->parent != nullptr ? bf::node_index(index_of[isize(n->parent->typed_id)])
                                                                : bf::node_index::invalid,
                                 .first_child = i32(out.node_children.size()),
                                 .child_count = i32(n->children.count),
                                 .local = to_transform(n->local_transform),
                                 .geometry = to_transform(n->geometry_transform),
                                 .has_geometry_transform = n->has_geometry_transform,
                                 .node_to_world = to_affine(n->node_to_world),
                                 .geometry_to_world = to_affine(n->geometry_to_world),
                                 .first_material = i32(out.node_materials.size()),
                                 .material_count = i32(n->materials.count),
                                 .visible = n->visible};

            switch (n->inherit_mode)
            {
            case UFBX_INHERIT_MODE_IGNORE_PARENT_SCALE:
                node.inherit = bf::inherit_mode::ignore_parent_scale;
                break;
            case UFBX_INHERIT_MODE_COMPONENTWISE_SCALE:
                node.inherit = bf::inherit_mode::component_wise_scale;
                break;
            default:
                node.inherit = bf::inherit_mode::normal;
                break;
            }

            if (n->mesh != nullptr)
                node.mesh = bf::mesh_index(int(n->mesh->typed_id));
            if (n->light != nullptr)
                node.light = bf::light_index(int(n->light->typed_id));

            for (auto c = size_t(0); c < n->children.count; ++c)
                out.node_children.push_back(bf::node_index(index_of[isize(n->children.data[c]->typed_id)]));
            for (auto m = size_t(0); m < n->materials.count; ++m)
            {
                auto const* const material = n->materials.data[m];
                out.node_materials.push_back(material != nullptr ? bf::material_index(int(material->typed_id))
                                                                 : bf::material_index::invalid);
            }

            out.nodes.push_back(cc::move(node));
        }
    }
};
} // namespace
} // namespace babel::impl

namespace babel::fbx
{
bool scene_settings::is_left_handed() const
{
    auto const direction = [](axis a) -> tg::vec3f
    {
        switch (a)
        {
        case axis::positive_x:
            return tg::vec3f(1, 0, 0);
        case axis::negative_x:
            return tg::vec3f(-1, 0, 0);
        case axis::positive_y:
            return tg::vec3f(0, 1, 0);
        case axis::negative_y:
            return tg::vec3f(0, -1, 0);
        case axis::positive_z:
            return tg::vec3f(0, 0, 1);
        case axis::negative_z:
            return tg::vec3f(0, 0, -1);
        }
        return tg::vec3f(0, 0, 0);
    };

    // Right-handed means right x up = front, FBX's front pointing out of the screen at the viewer.
    auto const r = direction(right);
    auto const u = direction(up);
    auto const f = direction(front);
    auto const cross = tg::vec3f(r[1] * u[2] - r[2] * u[1], r[2] * u[0] - r[0] * u[2], r[0] * u[1] - r[1] * u[0]);
    return cross[0] * f[0] + cross[1] * f[1] + cross[2] * f[2] < 0;
}

property const* material::find_property(cc::string_view property_name) const
{
    for (auto const& p : properties)
        if (p.name == property_name)
            return &p;
    return nullptr;
}

texture_index material::find_texture(cc::string_view property_name) const
{
    for (auto const& t : textures)
        if (t.property == property_name)
            return t.texture;
    return texture_index::invalid;
}

bool data::has_issue_of(issue_kind kind) const
{
    for (auto const& i : issues)
        if (i.kind == kind)
            return true;
    return false;
}

cc::string data::issue_report() const
{
    auto out = cc::string();
    for (auto const& i : issues)
    {
        if (!out.empty())
            out += "\n";
        switch (i.kind)
        {
        case issue_kind::unsupported:
            out += "unsupported: ";
            break;
        case issue_kind::unresolved:
            out += "unresolved: ";
            break;
        case issue_kind::malformed:
            out += "malformed: ";
            break;
        }
        out += i.message;
    }
    return out;
}

namespace
{
/// The one parse every overload reaches; `input_size` is -1 when the source cannot say how long it is.
/// Without `geometry`, ufbx skips every mesh's vertex data and the meshes come back named and empty.
cc::result<data> read_sized(cc::read_stream& in, read_options opts, i64 input_size, bool geometry)
{
    CC_RECORD_SCOPE("fbx.read");

    auto ctx = babel::impl::stream_context{.in = &in};
    auto const stream = ufbx_stream{.read_fn = &babel::impl::read_from_stream, .user = &ctx};

    // Everything off that is not the file's own content: no external files, no conversions, no generated data.
    auto load = ufbx_load_opts{};
    load.file_format = UFBX_FILE_FORMAT_FBX;
    load.no_format_from_extension = true;
    load.load_external_files = false;
    load.ignore_missing_external_files = true;
    load.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_PRESERVE;
    load.inherit_mode_handling = UFBX_INHERIT_MODE_HANDLING_PRESERVE;
    load.node_depth_limit = babel::impl::fbx_node_depth_limit;
    load.ignore_geometry = !geometry;

    auto pool = babel::impl::scheduler_pool{.scheduler = opts.scheduler};
    if (opts.scheduler != nullptr)
        load.thread_opts.pool = ufbx_thread_pool{.run_fn = &babel::impl::run_on_scheduler,
                                                 .wait_fn = &babel::impl::wait_on_scheduler,
                                                 .user = &pool};

    // ufbx allocates a binary array's stated count before it inflates the array, so the ceiling is what makes a crafted count a read error rather than an allocation of whatever it asked for.
    // The allocation count is bounded alongside, since ufbx counts both.
    // One budget, split evenly between ufbx's working memory and the scene it builds, so the two together never pass it.
    // Both at least 1, since ufbx reads 0 as "unlimited" — a tiny budget must refuse rather than lift the ceiling.
    auto const halved = babel::impl::memory_limit_of(opts.memory_limit, input_size) / 2;
    auto const half = size_t(halved > 0 ? halved : 1);
    auto const allocations = half / 64 > 0 ? half / 64 : size_t(1);
    load.temp_allocator.memory_limit = half;
    load.result_allocator.memory_limit = half;
    load.temp_allocator.allocation_limit = allocations;
    load.result_allocator.allocation_limit = allocations;

    auto error = ufbx_error{};
    auto* const scene = ufbx_load_stream(&stream, &load, &error);
    if (scene == nullptr)
    {
        if (!ctx.error.empty())
            return cc::error(cc::format("fbx: reading the stream failed: {}", ctx.error));

        char message[512] = {};
        ufbx_format_error(message, sizeof(message), &error);
        return cc::error(cc::format("fbx: {}", cc::string_view(message)));
    }

    auto converter = babel::impl::fbx_converter{.scene = *scene, .opts = opts};
    converter.convert_info();
    converter.note_unsupported();
    converter.convert_textures();
    converter.convert_materials();
    converter.convert_meshes();
    converter.convert_lights();
    converter.convert_nodes();
    ufbx_free_scene(scene);

    if (!converter.error.empty())
        return cc::error(cc::move(converter.error));
    return cc::move(converter.out);
}
} // namespace

cc::result<data> read(cc::read_stream& in, read_options opts)
{
    return read_sized(in, opts, -1, true);
}

cc::result<data> read(cc::span<byte const> bytes, read_options opts)
{
    auto adapter = cc::span_read_stream_adapter(bytes);
    cc::read_stream stream = adapter;
    return read_sized(stream, opts, bytes.size(), true);
}

cc::result<data> read(cc::string_view text, read_options opts)
{
    return read(text.as_bytes(), opts);
}

cc::result<data> read_structure(cc::read_stream& in, read_options opts)
{
    return read_sized(in, opts, -1, false);
}

cc::result<data> read_structure(cc::span<byte const> bytes, read_options opts)
{
    auto adapter = cc::span_read_stream_adapter(bytes);
    cc::read_stream stream = adapter;
    return read_sized(stream, opts, bytes.size(), false);
}

loading read_async(cc::pinned_data<byte const> bytes, read_options opts)
{
    auto& scheduler = opts.scheduler != nullptr ? *opts.scheduler : cc::compute_scheduler();
    opts.scheduler = &scheduler;

    auto const stage = [&](bool geometry)
    {
        auto node = cc::make_async_lazy<data>(
            [bytes, opts, geometry](cc::async_context<data>& ctx) -> cc::async_step_status
            {
                auto read = geometry ? babel::fbx::read(bytes.span(), opts) : read_structure(bytes.span(), opts);
                if (read.has_error())
                    return ctx.resolve_to_error(cc::async_error::make_error(cc::move(read).error()));
                return ctx.resolve_to_value(cc::move(read).value());
            });
        node->schedule_on(scheduler);
        return node;
    };
    return {.structure = stage(false), .complete = stage(true)};
}
} // namespace babel::fbx
