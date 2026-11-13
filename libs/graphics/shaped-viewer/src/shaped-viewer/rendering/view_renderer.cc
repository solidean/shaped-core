#include <clean-core/common/asserts.hh>
#include <clean-core/common/hash.hh>
#include <clean-core/common/log.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/common/time.hh>    // cc::current_time_steady_secs
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/span.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/mix_routine.hh>
#include <shaped-rendering/reconstruct.hh>
#include <shaped-rendering/slug_atlas.hh>
#include <shaped-rendering/slug_routine.hh> // sr::slug_instance
#include <shaped-viewer/drawing/decal.hh>
#include <shaped-viewer/drawing/drawing_manager.hh>
#include <shaped-viewer/impl/gpu_marks.hh>
#include <shaped-viewer/rendering/hybrid_routine.hh>
#include <shaped-viewer/rendering/impl/emitters.hh>
#include <shaped-viewer/rendering/impl/resolved_view.hh>
#include <shaped-viewer/rendering/pathtrace_routine.hh>
#include <shaped-viewer/rendering/raster_routine.hh>
#include <shaped-viewer/rendering/view_renderer.hh>
#include <shaped-viewer/rendering/wavefront_routine.hh>
#include <shaped-viewer/resources/gpu_resource_manager.hh>
#include <shaped-viewer/resources/material_shader_cache.hh>
#include <shaped-viewer/resources/resource_managers.hh>
#include <shaped-viewer/scene/light.hh>
#include <shaped-viewer/view/render_settings.hh>
#include <shaped-viewer/view/view_data.hh>
#include <shaped-viewer/view/view_store.hh>
#include <shaped-viewer/view/viewer_definition.hh>
#include <typed-geometry/transform/compose.hh>


namespace sv
{
namespace
{
/// Every guide sv's raygen is able to write, which is what `automatic` is resolved against here.
///
/// The set a given frame HAS is smaller — a layer declares the specular pair only once something wants it, and the
/// temporal slots arrive a frame later — but the calls below decide what to declare, so asking with this frame's set
/// would answer "nothing needs the split", declare nothing, and keep answering that forever.
/// `hit_distance` is absent because nothing writes one yet, so a member requiring it is correctly skipped.
[[nodiscard]] sr::reconstruct_guide_set traceable_guides()
{
    using g = sr::reconstruct_guide;
    return g::albedo | g::specular_albedo | g::normal | g::roughness | g::depth | g::motion | g::split_diffuse_specular;
}

/// Packs an affine placement into the TLAS instance's row-major 3x4 wire layout, translation in column 3.
/// tg's linear part is column-major (`l[c, r]`), so the transpose happens here rather than in a mat4 round-trip.
void pack_transform(sg::tlas_instance& inst, tg::affine_transform3f const& t)
{
    auto const l = t.linear_mat();
    auto const p = t.translation();
    for (auto r = 0; r < 3; ++r)
    {
        for (auto c = 0; c < 3; ++c)
            inst.transform[r * 4 + c] = l[c, r];
        inst.transform[r * 4 + 3] = p[r];
    }
}


using impl::resolved_view;

/// The mask of an instance every hybrid ray skips: every bit but module `tracer`'s `ray_mask_hybrid` and `ray_mask_hybrid_shadow`, so every other trace still meets it.
constexpr u8 instance_mask_hybrid_skips = 0xFC;

/// The mask of an instance the hybrid's shadow rays pass while its bounces still meet it: every bit but `ray_mask_hybrid_shadow`.
constexpr u8 instance_mask_hybrid_shadow_skips = 0xFD;

/// The constant `f` names in `r`, or `fallback` where it is not one.
[[nodiscard]] f32 constant_in(material_emission_factor const& f, instance_record const& r, f32 fallback)
{
    if (!f.is_constant())
        return fallback;
    for (auto const& slot : r.slots)
        if (slot.kind == material_slot_kind::constant && u32(slot.offset) == f.offset && slot.constant.size() >= 4)
        {
            auto value = 0.0f;
            cc::memcpy(&value, slot.constant.data(), 4);
            return value;
        }
    return fallback;
}

/// Whether an instance of `secondary` transmits at all.
[[nodiscard]] bool is_transmissive(material_secondary const& secondary, instance_record const& r)
{
    return constant_in(secondary.transmission, r, 0.0f) > 0.0f;
}

/// Whether an instance of `secondary` is a pane: transmissive, and a thin wall with no interior to cross.
[[nodiscard]] bool is_pane(material_secondary const& secondary, instance_record const& r)
{
    return is_transmissive(secondary, r) && constant_in(secondary.thin_walled, r, 0.0f) != 0.0f;
}

/// Whether an instance of `opacity` keeps its opacity as coverage rather than thresholding it: one that cuts out at all, with no cutoff or a cutoff of 0.
/// `r` is the instance's parameter block, where the cutoff is a constant.
[[nodiscard]] bool keeps_coverage(material_opacity const& opacity, instance_record const& r)
{
    if (!opacity.can_cut_out())
        return false;
    if (opacity.cutoff == material_opacity::no_offset)
        return true;
    for (auto const& slot : r.slots)
        if (slot.kind == material_slot_kind::constant && u32(slot.offset) == opacity.cutoff && slot.constant.size() >= 4)
        {
            auto cutoff = 0.0f;
            cc::memcpy(&cutoff, slot.constant.data(), 4);
            return cutoff <= 0.0f;
        }
    return true;
}

/// The hit-group index for `key` — a `scene_item::shader_key` — in `out`, appending it on first use, so the order is the
/// scene's own.
[[nodiscard]] u32 hit_group_of(resolved_view& out, cc::hash128 key, gpu_resource_manager& resources)
{
    for (auto i = isize(0); i < out.permutations.size(); ++i)
        if (out.permutations[i] == key)
            return u32(i);

    auto const* const permutation = resources.shaders.find(key);
    CC_ASSERT(permutation != nullptr, "a scene_item names a permutation the shader cache never generated");
    out.permutations.push_back(key);
    out.hit_groups.push_back(permutation);
    return u32(out.hit_groups.size() - 1);
}

/// What a pending mesh is traced as: the shared unit-cube BLAS, mapped onto the mesh's own object-space box.
///
/// The box is folded into the TLAS transform rather than into a BLAS of its own, which is what makes every placeholder
/// in a scene one acceleration structure and a 3x4 matrix each.
/// The cube spans `[0,1]^3`, so the mapping is a scale by the box's extent followed by a translation to its corner —
/// and then the item's own placement on top of that, since the box is in object space.
[[nodiscard]] tg::affine_transform3f placeholder_transform(tg::aabb3f const& box, tg::affine_transform3f const& placement)
{
    auto const extent = box.max - box.min;
    auto const onto_box
        = tg::compose(tg::affine_transform3f::make_translation(tg::vec3f(box.min[0], box.min[1], box.min[2])),
                      tg::affine_transform3f::make_scaling(extent));
    return tg::compose(placement, onto_box);
}

/// Where `p`'s shading stands in the integrator `mode` traces with: the megakernel's hit group, or the wavefront's shade kernel.
/// A raster frame resolves no light, so for it every permutation is ready.
[[nodiscard]] impl::permutation_state permutation_state_in(sg::command_list& cmd,
                                                           render_mode mode,
                                                           material_permutation const* p)
{
    if (mode == render_mode::megakernel)
        return impl::hit_group_state(cmd.context(), p);
    if (mode == render_mode::wavefront)
        return wavefront_routine::shade_kernel_state(cmd, p);
    return impl::permutation_state::ready;
}

/// `l` resolved for a frame recorded onto `cmd` in `mode`, its acceleration structures built on demand where it traces.
///
/// A raster frame builds none, which is what lets it draw on a device without ray tracing.
resolved_view resolve_scene(sg::command_list& cmd, layer const& l, gpu_resource_manager& resources, render_mode mode)
{
    auto const traced = mode != render_mode::raster;
    auto out = resolved_view{};

    // A frame mesh's attributes are written here, ahead of every read this command list records, so they are resident this frame.
    resources.attributes.record_inline_uploads(cmd);

    // Acquired once rather than per item: it is a resolve walk plus a key hash plus a lookup, and every placeholder in
    // the layer shades through the same one.
    auto const& fallback = resources.shaders.acquire_fallback();

    for (auto const& item : l.items)
    {
        if (item.kind == scene_item_kind::quadric_set)
        {
            auto const* const set = resources.quadrics.get_ptr(item.quadrics);
            CC_ASSERT(set != nullptr, "scene_item references an unknown quadric_set_id");
            CC_ASSERT(resources.contains_instance(item.instance), "scene_item references an unknown instance_id");

            // A batch still streaming keeps its place as a box, exactly as a mesh does, and one that declared no extent is
            // skipped for the same reason: there is no honest place to draw it.
            // So does one whose attributes still stream: its parameter block would bind them, stalling the frame until they land.
            auto const pending = set->state != residency::complete || !resources.attributes_resident(item.instance);
            if (pending && !set->bounds.has_value())
                continue;

            // A placeholder is a TRIANGLE cube, so it must shade through the triangle fallback rather than the batch's own
            // quadric permutation — a procedural hit group on a triangle BLAS is exactly the mismatch that refuses to build.
            auto const permutation = hit_group_of(out, pending ? fallback.key : item.shader_key, resources);

            // The bounds are read first: building the BLAS charges the budget, which may move what `set` points at.
            auto const placement = pending ? placeholder_transform(set->bounds.value(), item.transform) : item.transform;
            // None on a raster frame, which draws from the records alone.
            auto blas = sg::blas_handle();
            if (traced)
                blas = pending ? resources.meshes.placeholder_blas()
                               : resources.quadrics.acquire_blas(cmd, item.quadrics);
            auto inst = sg::tlas_instance{.blas = cc::move(blas),
                                          .instance_id = u32(out.instances.size()),
                                          .hit_group_offset = permutation * 2,
                                          .opaque_override = true};
            pack_transform(inst, placement);
            out.instances.push_back(cc::move(inst));

            out.records.push_back(resources.describe_instance(cmd, item.quadrics, item.instance));
            place_instance_record(out.records.back(), out.instances.back(), out.hit_groups[permutation]->opacity,
                                  out.hit_groups[permutation]->secondary);
            out.parameter_blocks.push_back(item.instance);
            out.frame_contents.push_back({});
            out.contents.push_back(pending ? cc::hash128() : resources.quadrics.hash_of(item.quadrics));
            out.own_indices.push_back({});
            // A pending batch is the cube a raster frame draws too; a landed one has no triangles to draw.
            out.triangle_counts.push_back(pending ? u32(resources.meshes.placeholder_geometry().triangle_count) : 0);
            out.primitive_counts.push_back(pending ? 0 : u32(set->primitive_count));
            out.bounds.push_back(pending ? cc::optional<tg::aabb3f>(tg::aabb3f(tg::pos3f(0, 0, 0), tg::pos3f(1, 1, 1)))
                                         : cc::optional<tg::aabb3f>());

            // Set for any batch, pending or not: a pending one is a triangle instance this frame and a procedural one
            // the next, and the stand-in has to already be compiling by then.
            out.has_quadrics = true;
            continue;
        }

        if (item.kind != scene_item_kind::triangle_mesh)
            continue;

        // A frame mesh is uploaded right here, so it is never pending and never a placeholder; it lights nothing (see `mesh_lifetime::frame`).
        if (item.lifetime == mesh_lifetime::frame)
        {
            CC_ASSERT(resources.contains_instance(item.instance), "scene_item references an unknown instance_id");
            auto const& m = resources.acquire_frame_mesh(cmd, item.frame_geometry, traced);
            auto const shade_fallback = !resources.attributes_resident(item.instance);
            auto const permutation = hit_group_of(out, shade_fallback ? fallback.key : item.shader_key, resources);
            auto inst = sg::tlas_instance{.blas = traced ? m.blas : sg::blas_handle(),
                                          .instance_id = u32(out.instances.size()),
                                          .hit_group_offset = permutation * 2,
                                          .opaque_override = !out.hit_groups[permutation]->opacity.can_cut_out()};
            pack_transform(inst, item.transform);
            out.instances.push_back(cc::move(inst));
            out.records.push_back(resources.describe_instance(cmd, m, item.instance));
            place_instance_record(out.records.back(), out.instances.back(), out.hit_groups[permutation]->opacity,
                                  out.hit_groups[permutation]->secondary);
            out.parameter_blocks.push_back(item.instance);
            out.frame_contents.push_back(item.frame_geometry.hash);
            out.contents.push_back(item.frame_geometry.hash);
            out.own_indices.push_back(m.draw_indices);
            out.triangle_counts.push_back(u32(m.triangle_count));
            out.primitive_counts.push_back(0);
            // No box: a frame mesh's geometry is new each frame, so a raster frame draws it always rather than measure it.
            out.bounds.push_back(cc::optional<tg::aabb3f>());
            continue;
        }

        auto const* const mesh = resources.meshes.get_ptr(item.mesh);
        CC_ASSERT(mesh != nullptr, "scene_item references an unknown mesh_id");
        CC_ASSERT(resources.contains_instance(item.instance), "scene_item references an unknown instance_id");

        // The TLAS instance's own id is the row of the instance table this item occupies, which is what a hit's instance id reads.
        // The opaque override is what makes an any-hit reachable at all: a BLAS is built opaque, and the trace then behaves
        // as if the hit group carried no any-hit whatever the pipeline attached.
        // Per instance rather than per BLAS, because "can this cut out" is a property of the MATERIAL — the same mesh
        // under an opaque material and a cutout one would otherwise need two acceleration structures.
        // Two records per permutation — the surface one and the shadow one — so the offset is the permutation's index
        // doubled, which `pathtrace_routine` rewrites to that permutation's row of its table.
        // A mesh whose geometry has not landed keeps its place in the scene as a box, so a load reads as an asset
        // sharpening rather than as objects popping into existence one at a time.
        // One that declared no bounds is skipped instead: there is no honest extent to draw it at, and an invented one
        // would put it somewhere it is not.
        auto const is_pending = mesh->state != residency::complete;
        if (is_pending && !mesh->bounds.has_value())
            continue;

        // A placeholder shades through the NEUTRAL hit group rather than the item's own permutation.
        //
        // Not a shortcut: the cube's triangles have nothing to do with the mesh's, so per-vertex attributes read
        // through them would be indexed out of the data they belong to.
        // The fallback reads no attributes and no parameter block, which is exactly what makes it safe here.
        //
        // An instance whose attributes are still streaming shades through it too, on its real geometry: binding those
        // buffers would stall the trace until every one of them lands.
        auto const shade_fallback = is_pending || !resources.attributes_resident(item.instance);
        auto const permutation = hit_group_of(out, shade_fallback ? fallback.key : item.shader_key, resources);
        // Read first: building the BLAS charges the budget, which may move what `mesh` points at.
        auto const placement = is_pending ? placeholder_transform(mesh->bounds.value(), item.transform) : item.transform;
        auto const triangle_count
            = u32(is_pending ? resources.meshes.placeholder_geometry().triangle_count : mesh->geometry.triangle_count);
        // A placeholder is the unit cube, which its placement scales onto the mesh's box.
        auto const object_bounds
            = is_pending ? cc::optional<tg::aabb3f>(tg::aabb3f(tg::pos3f(0, 0, 0), tg::pos3f(1, 1, 1))) : mesh->bounds;
        auto const object_area = mesh->surface_area;
        auto blas = sg::blas_handle();
        if (traced)
            blas = is_pending ? resources.meshes.placeholder_blas() : resources.meshes.acquire_blas(cmd, item.mesh);
        auto inst = sg::tlas_instance{.blas = cc::move(blas),
                                      .instance_id = u32(out.instances.size()),
                                      .hit_group_offset = permutation * 2,
                                      .opaque_override = !out.hit_groups[permutation]->opacity.can_cut_out()};
        pack_transform(inst, placement);
        out.instances.push_back(cc::move(inst));

        // This is where every index a hit reads is minted, so it must stay ahead of `freeze()` and on the list that traces.
        out.records.push_back(resources.describe_instance(cmd, item.mesh, item.instance));
        place_instance_record(out.records.back(), out.instances.back(), out.hit_groups[permutation]->opacity,
                              out.hit_groups[permutation]->secondary);
        out.parameter_blocks.push_back(item.instance);
        out.frame_contents.push_back({});
        out.contents.push_back(is_pending ? cc::hash128() : resources.meshes.hash_of(item.mesh));
        out.own_indices.push_back({});
        out.triangle_counts.push_back(triangle_count);
        out.primitive_counts.push_back(0);
        out.bounds.push_back(object_bounds);

        // An instance lights the scene where its own permutation declares an emission that is not dark throughout.
        // The GPU reads each factor at the sampled point, so a constant, a texture and a mesh attribute all qualify; the estimate weighs selection alone.
        if (shade_fallback)
            continue;
        // A texture that does not sample its own texels yet, or ever, is unknown rather than proven by them.
        auto texture_summary = [&](texture_id id) -> cc::optional<value_summary>
        {
            auto const* const r = resources.textures.get_ptr(id);
            return r != nullptr ? impl::sampled_texture_summary(r->state, r->summary) : cc::optional<value_summary>();
        };
        auto attribute_summary = [&](attribute_id id) -> cc::optional<value_summary>
        {
            auto const* const r = resources.attributes.get_ptr(id);
            return r != nullptr ? r->summary : cc::optional<value_summary>();
        };
        auto const& emission = out.hit_groups[permutation]->emission;
        auto const estimate = impl::emitter_estimate_of(emission, resources.get_instance(item.instance),
                                                        {.texture = texture_summary, .attribute = attribute_summary});

        // The hybrid's rays skip an instance that keeps coverage and lights nothing: such a material is a decal over a
        // surface, mostly fractional, so every crossing asks the cutout decision for a shadow and a bounce that barely move.
        // One that lights the scene stays, since the bounce weighs the lights it meets against the light sample.
        if (!estimate.has_value()
            && keeps_coverage(out.hit_groups[permutation]->opacity, resources.get_instance(item.instance)))
            out.instances.back().mask = instance_mask_hybrid_skips;

        // A pane is drawn where the hybrid rasterizes it, and every one of its rays passes it, unrefracted and untinted.
        // Its single bounce is spent getting through the first pane, so a second one met would be lit as an opaque
        // surface of its base color; and what lies behind a pane has no other way to the light.
        // Solid glass is met by the bounce, which refracts through it, and passed by the shadow rays alone: a light inside a
        // bulb, or a room behind a windscreen modelled as a closed slab, is otherwise reached by nothing but rare bounces.
        auto const& instance = resources.get_instance(item.instance);
        auto const& secondary = out.hit_groups[permutation]->secondary;
        if (is_pane(secondary, instance))
            out.instances.back().mask = instance_mask_hybrid_skips;
        else if (is_transmissive(secondary, instance))
        {
            out.instances.back().mask &= instance_mask_hybrid_shadow_skips;
            out.records.back().glass = 1u;
        }

        if (!estimate.has_value() || !traced || !resources.meshes.can_build_emitter_cdfs())
            continue;
        auto const state = permutation_state_in(cmd, mode, out.hit_groups[permutation]);
        if (estimate.value().shows_emission() && state != impl::permutation_state::failed)
            ++out.emissive_items;
        // A permutation the trace stands in for shades no emission, so its light record would credit the bounce strategy with light it never carries.
        // One still compiling may land before the trace binds it, which then shades the lamp with no record for it: an image of a darker scene, so it restarts.
        if (state != impl::permutation_state::ready)
        {
            out.emitters_pending = out.emitters_pending || state == impl::permutation_state::pending;
            continue;
        }

        // Not pending, so `triangle_count` is the mesh's own, read with `object_area` before any build could move `mesh`.
        auto const triangles = isize(triangle_count);
        auto const* const cdf = resources.meshes.acquire_emitter_cdf(cmd, item.mesh);
        if (cdf == nullptr)
        {
            // Its kernels are still compiling: camera-only this frame.
            out.emitters_pending = true;
            continue;
        }
        auto const cdf_index = u32(resources.acquire_buffer(cdf->raw()->as_raw_readonly()));

        // Exact under a similarity; under any other placement the estimate stands in until the GPU's exact sum lands.
        // Selection alone reads the area, so the stand-in costs noise and the area landing restarts the image once.
        auto area = impl::world_area_of(object_area, item.transform);
        if (auto const linear = item.transform.linear_mat(); !impl::is_similarity(linear))
            if (auto const exact = resources.meshes.acquire_world_area(cmd, item.mesh, linear); exact.has_value())
                area = exact.value();

        out.emitter_lights.push_back(
            {.position = {},
             .path = light_path_mesh,
             .u = {},
             .area = area,
             .v = {},
             .flags = light_flag_two_sided | (estimate.value().unseen ? light_flag_unseen_emission : 0u),
             // selection's estimate alone: the sampled point's emission is read from data
             .emission = estimate.value().radiance,
             .cone_scale = 0,
             .normal = {},
             .cone_offset = 1,
             .one_minus_cos_angular_radius = 0,
             .link_mask = ~0u,
             .select_pdf = 0,
             .select_cdf = 0});
        out.emitters.push_back(emitter_record_of(u32(out.records.size() - 1), cdf_index, u32(triangles),
                                                 emitter_cdf_block_count(triangles), emission));
    }

    CC_ASSERT(!out.instances.empty(), "a view needs at least one drawable item to render");
    return out;
}

/// The instance table `r` describes, uploaded for this recording.
[[nodiscard]] sg::buffer<shaders::tracer::instance_record> upload_instances(sg::command_list& cmd, resolved_view const& r)
{
    auto const buffer = cmd.context().transient.create_buffer<shaders::tracer::instance_record>(
        r.records.size(), sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
    cmd.upload.data_to_buffer(buffer, r.records);
    return buffer;
}

/// The lights the path tracer samples, grouped by path: every light in the layer, then every emissive instance `r` resolved.
/// A layer with neither takes its fallback — so a layer nobody lit is still visible, unless its fallback was turned off.
[[nodiscard]] pt_light_table traced_lights(layer const& l, resolved_view const& r)
{
    auto records = cc::vector<shaders::tracer::light_record>();
    for (auto const& sl : l.lights)
        records.push_back(light_record_of(sl.light));

    if (records.empty() && r.emissive_items == 0 && l.fallback_light.has_value())
        records.push_back(light_record_of(l.fallback_light.value()));

    // Last, as the table groups them: `light_path_mesh` follows every analytic path, and keeps this order within its run.
    for (auto const& e : r.emitter_lights)
        records.push_back(e);

    return pt_light_table::grouped(records);
}

/// Writes into each emissive instance's row of `r.records` the index of its light record in `lights`.
void link_emitters(resolved_view& r, pt_light_table const& lights)
{
    for (auto k = isize(0); k < r.emitters.size(); ++k)
        r.records[r.emitters[k].instance].emitter = lights.path_offset[light_path_mesh] + u32(k);
}

/// The emitter records `r` resolved, uploaded for this recording, or null for none.
[[nodiscard]] sg::buffer<shaders::tracer::emitter_record> upload_emitters(sg::command_list& cmd, resolved_view const& r)
{
    if (r.emitters.empty())
        return {};
    return cmd.context().transient.create_buffer_from_data(cmd, r.emitters, sg::buffer_usage::readonly_buffer);
}

/// The table `lights` holds, uploaded for this recording.
/// Null for no lights, which the routine binds a stand-in for, since a buffer cannot be empty.
[[nodiscard]] sg::buffer<shaders::tracer::light_record> upload_lights(sg::command_list& cmd, pt_light_table const& lights)
{
    if (lights.records.empty())
        return {};

    auto const buffer = cmd.context().transient.create_buffer<shaders::tracer::light_record>(
        lights.records.size(), sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
    cmd.upload.data_to_buffer(buffer, lights.records);
    return buffer;
}

/// A layer's decals as the trace reads them: each projector, and the shapes it covers read back from the decal atlas.
struct traced_decals
{
    cc::vector<shaders::tracer::decal_record> decals;
    cc::vector<shaders::tracer::decal_shape> shapes;
};

/// `l`'s decals over `atlas`, skipping one that draws nothing.
[[nodiscard]] traced_decals decals_of(layer const& l, sr::slug_atlas const& atlas)
{
    auto out = traced_decals{};
    for (auto const& d : l.decals)
    {
        if (d.record_count == 0)
            continue;
        out.decals.push_back(impl::decal_record_of(d, u32(out.shapes.size())));
        for (auto i = u32(0); i < d.record_count; ++i)
            out.shapes.push_back(impl::decal_shape_of(atlas.record(d.first_record + i)));
    }
    return out;
}

/// What a trace binds for `decals`: the two buffers and the atlas, prepared, or all null for none.
struct uploaded_decals
{
    sg::buffer<shaders::tracer::decal_record> decals;
    sg::buffer<shaders::tracer::decal_shape> shapes;
    sr::slug_atlas const* atlas = nullptr;
};

[[nodiscard]] uploaded_decals upload_decals(sg::command_list& cmd, traced_decals const& decals, sr::slug_atlas& atlas)
{
    if (decals.decals.empty())
        return {};

    auto& ctx = cmd.context();
    atlas.prepare(cmd);
    auto out = uploaded_decals{.decals = ctx.transient.create_buffer<shaders::tracer::decal_record>(
                                   decals.decals.size(), sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst),
                               .shapes = ctx.transient.create_buffer<shaders::tracer::decal_shape>(
                                   decals.shapes.size(), sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst),
                               .atlas = &atlas};
    cmd.upload.data_to_buffer(out.decals, decals.decals);
    cmd.upload.data_to_buffer(out.shapes, decals.shapes);
    return out;
}

shaders::tracer::frame_constants make_frame_constants(view_data const& v,
                                                      layer const& l,
                                                      pt_light_table const& lights,
                                                      tg::vec2i resolution)
{
    auto fc = default_frame_constants();
    // The projection carries the aspect ratio.
    // It comes from the resolution the frame settled on rather than the view's own field, since a layout-following
    // view is sized by the rect it landed in.
    auto cam = v.camera;
    cam.projection.aspect_ratio = f64(resolution[0]) / f64(resolution[1] > 0 ? resolution[1] : 1);
    fc.camera = camera_record_of(cam);
    set_pixel_footprint(fc, resolution[1]);

    lights.describe_in(fc);

    fc.samples_per_pixel = l.settings.samples_per_pixel;
    // the primary cone narrows where the image averages many samples, which filter the pixel already (pbrt-v4's rule)
    auto const expected = l.settings.expected_samples_per_pixel;
    fc.primary_scale = expected > 1 ? cc::max(0.125f, 1.0f / tg::sqrt(f32(expected))) : 1.0f;
    fc.channel_mask = l.settings.channels;
    // A hybrid layer lights its own count of bounces, which its shaders read from the same field.
    fc.max_bounces
        = l.settings.mode == render_mode::hybrid ? cc::clamp(l.settings.hybrid_bounces, 1, 2) : l.settings.max_bounces;

    // `seed` and `accum_frame` are stamped by execute from the view's persistent record, once it knows whether this frame restarts.
    return fc;
}

/// Everything this trace uploads, folded into one value — the signal that decides whether the accumulated image is still valid.
///
/// It hashes the *uploaded* bytes rather than the view's fields, so it cannot drift away from what the shader actually reads.
///
/// Where the view lands is structurally absent — a view_data carries no position — so relayout cannot discard a converged image.
/// That discipline serves interaction, not just tidiness: a leaf's fit mode, sampler, zoom and
/// post-process parameters never reach an upload, which is exactly why dragging a wipe slider cannot restart a trace.
///
/// `resolution` is the one deliberate exception to "uploaded bytes only".
/// It reaches the upload only through the camera's aspect ratio, so 960x540 and 1920x1080 hash identically; without it
/// correctness would rest entirely on the resize check noticing, and a same-size texture handed back by a pool would blend two views.
///
/// `shader_generation` is the second: the shaders are not uploaded bytes but they decide what the numbers mean, so a
/// reload has to restart the estimator rather than average two different integrators together.
///
/// **The camera is in it**, which is what makes the accumulator a single, honest estimate: every sample it holds was
/// drawn through the same eye, so the mean it converges to is the image this frame is asking for and no other.
/// Moving the eye therefore restarts it, and that is the trade — no reprojection to salvage the old samples, and in
/// exchange no smearing, no per-pixel rejection heuristic, and an uncapped mean that converges to ground truth.
[[nodiscard]] u64 trace_hash(shaders::tracer::frame_constants fc,
                             background_gpu const& bg,
                             pt_light_table const& lights,
                             traced_decals const& decals,
                             resolved_view const& r,
                             tg::vec2i resolution,
                             u64 shader_generation)
{
    // The two fields that legitimately differ every frame; hashing either would restart the estimator forever.
    fc.rng_seed = 0;
    fc.accum_frame = 0;

    auto h = cc::make_hash_of_bytes(cc::span<shaders::tracer::frame_constants const>(&fc, 1).as_bytes());
    h = cc::combine_hash(h, cc::make_hash_of_bytes(cc::span<background_gpu const>(&bg, 1).as_bytes()));

    // Every byte of a shaders::tracer::light_record is written, pads included, so equal lights hash equal and any change is seen.
    h = cc::combine_hash(
        h, cc::make_hash_of_bytes(cc::span<shaders::tracer::light_record const>(lights.records).as_bytes()));

    // Every byte of both is written too, pads included.
    h = cc::combine_hash(
        h, cc::make_hash_of_bytes(cc::span<shaders::tracer::decal_record const>(decals.decals).as_bytes()));
    h = cc::combine_hash(
        h, cc::make_hash_of_bytes(cc::span<shaders::tracer::decal_shape const>(decals.shapes).as_bytes()));
    h = cc::combine_hash(h, cc::make_hash(resolution[0], resolution[1], shader_generation));

    // tlas_instance holds a handle and an optional, so its padding is not hashable — take the fields the build reads.
    // A frame mesh's BLAS is new every frame, so its geometry's content key stands in for it: an unchanged mesh keeps the image.
    for (auto i = isize(0); i < r.instances.size(); ++i)
    {
        auto const& inst = r.instances[i];
        auto const content = r.frame_contents[i];
        h = cc::combine_hash(h, cc::make_hash_of_bytes(cc::span<float const>(inst.transform, 12).as_bytes()));
        if (content == cc::hash128())
            h = cc::combine_hash(h, cc::make_hash(inst.instance_id, inst.hit_group_offset, inst.blas.get()));
        else
            h = cc::combine_hash(h, cc::make_hash(inst.instance_id, inst.hit_group_offset, content.low, content.high));
    }

    // The instance table is the whole of what a hit reads, geometry and material parameters alike, so hashing the
    // records covers content identity for both: a re-upload under one id lands on a different bindless element.
    //
    // `param_buffer` is the one field taken out.
    // The index is epoch-scoped by contract even where the slot table happens to hand back the same one, so it says
    // nothing about the image and leaving it in would risk restarting the estimator for nothing.
    // The `instance_id` beside it is the content identity that field stood for, since one is minted per `parameter_key`.
    // A frame mesh's buffers are new every frame as well, so their indices are taken out with it; its content key above covers them.
    for (auto i = isize(0); i < r.records.size(); ++i)
    {
        auto anonymized = r.records[i];
        anonymized.param_buffer = 0;
        if (r.frame_contents[i] != cc::hash128())
        {
            anonymized.vertices = 0;
            anonymized.indices = 0;
        }
        h = cc::combine_hash(
            h, cc::make_hash_of_bytes(cc::span<shaders::tracer::instance_record const>(&anonymized, 1).as_bytes()));
    }
    for (auto const block : r.parameter_blocks)
        h = cc::combine_hash(h, cc::make_hash(u32(block)));

    // Which shader reads those records is not in them, and a different permutation is a different image.
    for (auto const& key : r.permutations)
        h = cc::combine_hash(h, cc::make_hash(key.low, key.high));
    return h;
}

/// The key a traced image's random sequence is drawn from: the image, hashed over inputs that are equal on any machine.
///
/// So a capture is one image however often it restarted on the way: cold compiles and drains restart it on their own clock.
/// Where `trace_hash` reads a BLAS address or a bindless index, this reads the content behind it, or nothing.
/// A material's parameters and the decals are left out, since their keys name ids a process mints.
/// An edit to them alone keeps the sequence: the mean still restarts on `trace_hash`, and draws the same samples of a different image.
/// Moving frames are each a different camera, so they still draw sequences apart.
[[nodiscard]] u64 seed_key_of(shaders::tracer::frame_constants const& fc,
                              background_gpu const& bg,
                              pt_light_table const& lights,
                              resolved_view const& r,
                              tg::vec2i resolution)
{
    auto h = cc::make_hash_of_bytes(cc::span<shaders::tracer::camera_record const>(&fc.camera, 1).as_bytes());
    h = cc::combine_hash(h, cc::make_hash(fc.max_bounces, fc.samples_per_pixel, resolution[0], resolution[1]));
    // The channel mask is not here: the image does not depend on it, and a new mask reallocates the wavefront state,
    // which restarts the accumulation by itself.
    h = cc::combine_hash(h, cc::make_hash(cc::bit_cast<u32>(fc.primary_scale)));
    h = cc::combine_hash(h, cc::make_hash_of_bytes(cc::span<background_gpu const>(&bg, 1).as_bytes()));
    h = cc::combine_hash(
        h, cc::make_hash_of_bytes(cc::span<shaders::tracer::light_record const>(lights.records).as_bytes()));
    for (auto i = isize(0); i < r.instances.size(); ++i)
    {
        auto const& inst = r.instances[i];
        auto const shader = r.permutations[inst.hit_group_offset / 2];
        h = cc::combine_hash(h, cc::make_hash_of_bytes(cc::span<float const>(inst.transform, 12).as_bytes()));
        h = cc::combine_hash(h, cc::make_hash(r.contents[i].low, r.contents[i].high, shader.low, shader.high));
    }
    return h;
}

/// The random seed of frame `frame` of `slot`'s run: its image's `seed_key_of`, mixed with the frame within the run.
/// Mixed rather than spaced apart, since a run counts frames up to `frame_count_cap`, far past what a u32 could hold between two keys.
/// Never 0, which the tracer reads as no seed.
[[nodiscard]] u32 seed_at(impl::temporal_slot const& slot, u32 frame)
{
    auto const seed = u32(cc::make_hash(slot.seed_key, frame));
    return seed != 0 ? seed : 1;
}

/// The random seed of the frame `slot` traces next.
[[nodiscard]] u32 seed_of(impl::temporal_slot const& slot)
{
    return seed_at(slot, slot.accum_frame);
}

/// Element `index` of the Halton sequence in `base`, in [0, 1).
[[nodiscard]] f32 halton(u32 index, u32 base)
{
    auto fraction = 1.0f;
    auto value = 0.0f;
    for (auto i = index; i > 0; i /= base)
    {
        fraction /= f32(base);
        value += fraction * f32(i % base);
    }
    return value;
}

/// The sub-pixel offset, in [-0.5, 0.5), that sample `index` of a full-size hybrid mean draws every pixel through.
/// Halton (2, 3), so the first few already spread over the pixel, and the mean of many is its coverage of whatever an edge crosses.
[[nodiscard]] tg::vec2f still_jitter(u32 index)
{
    return tg::vec2f(halton(index + 1, 2) - 0.5f, halton(index + 1, 3) - 0.5f);
}

/// Bytes a texture of `size` in `format` costs, for the cache's budget.
[[nodiscard]] isize texture_bytes(tg::vec2i size, sg::pixel_format format)
{
    return isize(size[0]) * isize(size[1]) * isize(sg::format_block_size(format));
}

/// Whether `plan` declares the temporal resource `temporal_id` for view `id` this frame.
[[nodiscard]] bool declares(render_plan const& plan, view_id id, u64 temporal_id)
{
    for (auto const& t : plan.temporals)
        if (t.id == id && t.temporal_id == temporal_id)
            return true;
    return false;
}

/// The slot `store` keeps for `(id, temporal_id)`, its texture created or re-created if the extent moved.
/// `resized` is what tells a caller its history is gone, since a fresh texture holds nothing to blend into.
struct ensured_slot
{
    impl::temporal_slot* slot = nullptr;
    bool resized = false;
};

[[nodiscard]] ensured_slot ensure_temporal(sg::context& ctx,
                                           view_store& store,
                                           view_id id,
                                           u64 temporal_id,
                                           tg::vec2i resolution,
                                           sg::pixel_format format)
{
    auto& slot = store.get_or_create(id).temporal[temporal_id];

    auto const resized = slot.texture.raw() == nullptr || slot.texture.width() != resolution[0]
                      || slot.texture.height() != resolution[1];
    if (resized)
    {
        // UAV-written by whatever fills it, sampled by whatever reads it back — every temporal resource needs both.
        // Copyable too, so the host can read one back: a double-click reads a texel of the primary depth.
        // A slot the renderer mints is a render target too, since a raster frame draws the very slots a trace writes;
        // a caller's own keeps to these, as its format need not be renderable.
        auto usage = sg::texture_usage::texture | sg::texture_usage::image | sg::texture_usage::copy_src;
        if (temporal_id >= sv::temporal_id::caller_range_end)
            usage |= sg::texture_usage::render_target;
        auto const desc
            = sg::texture_2d_description{.format = format, .width = resolution[0], .height = resolution[1], .usage = usage};

        if (slot.texture.raw() != nullptr)
            slot.texture.raw()->expire();

        slot.texture = ctx.persistent.create_texture_2d(desc);
    }

    return {.slot = &slot, .resized = resized};
}

/// The mode a plan's layer is recorded in: `automatic` is the viewer's to resolve, and one that reaches a plan built by hand is the wavefront.
[[nodiscard]] render_mode planned_mode(render_mode mode)
{
    return mode == render_mode::automatic ? render_mode::wavefront : mode;
}

/// The bounce after which the wavefront's Russian roulette may end a path, which leaves `max_bounces` a cap.
constexpr u32 wavefront_roulette_after = 3;

/// The megakernel's noise counters: laid out as the wavefront's, so the sample floor and its shortfall sit at `counter_sample_floor`
/// (shaders/sgl/tracer_wavefront.sgl) and read back as a sample floor does.
constexpr isize noise_counter_count = 7;
constexpr isize noise_counter_floor = 5;

/// The frames every pixel under a noise target takes before its noise is judged: a variance over fewer is too noisy itself.
/// 16 frames are 64 samples at the default `samples_per_pixel`; at 4, a smooth wall stopped at 16 samples, where the noise
/// still had visible structure, and a variance over four frame means let pixels stop on an unlucky estimate.
constexpr u32 noise_min_frames = 16;

/// Where a traced layer's frame counter stops: far past any denoiser's hand-off, since it seeds each frame rather than saying how far the image has got.
constexpr u32 frame_count_cap = u32(1) << 30;

/// The samples a megakernel frame of a layer under `settings` adds to every pixel.
[[nodiscard]] u32 megakernel_samples_per_frame(render_settings const& settings)
{
    return u32(cc::max(1, settings.samples_per_pixel));
}

/// The frames `slot`'s mean has been held for once a frame that traces lands, which the denoiser's hand-off reads.
[[nodiscard]] u32 next_frame_count(impl::temporal_slot const& slot)
{
    return slot.accum_frame < frame_count_cap ? slot.accum_frame + 1 : slot.accum_frame;
}

/// The samples per pixel a traced layer stops at: `requested`, or `accumulation_sample_cap` where that is lower or `requested` is 0.
[[nodiscard]] u32 effective_sample_target(u32 requested)
{
    return requested == 0 ? accumulation_sample_cap : cc::min(requested, accumulation_sample_cap);
}

/// Counts a frame of `slot`'s layer that traced: its frame, and for a megakernel frame the samples it added to every pixel.
/// A megakernel layer's frames therefore hold `samples_per_pixel` times however many it took to reach its target, the first that did included.
void count_traced_frame(impl::temporal_slot& slot, render_settings const& settings)
{
    slot.accum_frame = next_frame_count(slot);
    if (settings.mode == render_mode::megakernel)
        slot.samples = slot.accum_frame * megakernel_samples_per_frame(settings);
    if (settings.mode == render_mode::hybrid)
        slot.samples = slot.accum_frame;
}

/// Raises `slot.samples` by every wavefront sample floor that has landed, and drops one whose download failed.
/// Its shortfall, how many pixels were still below the frame's target, replaces the last one.
void land_sample_floors(impl::temporal_slot& slot)
{
    while (!slot.sample_floors.empty() && slot.sample_floors.front().is_ready())
    {
        auto const floor = slot.sample_floors.front().try_get_data();
        // A resolve over no pixel leaves the floor where the reset raised it, which counts nothing.
        if (floor.has_value() && floor.value().size() == 2 && floor.value()[0] != ~u32(0))
        {
            slot.samples = cc::max(slot.samples, floor.value()[0]);
            slot.pixels_short = floor.value()[1];
            // A megakernel frame under a noise target in which no pixel was left tracing: the image is down to the target.
            if (slot.noise_adaptive && floor.value()[1] == 0)
                slot.noise_settled = true;
        }
        slot.sample_floors.remove_at(0);
    }

    // A device that never lands one must not grow the queue forever; a later floor only ever counts more.
    constexpr isize max_in_flight = 8;
    while (slot.sample_floors.size() > max_in_flight)
        slot.sample_floors.remove_at(0);
}

/// Whether this frame of `slot`'s layer traces at all: not a full-size one whose mean is at its target already.
///
/// Neither integrator has anything to add there, whether the target is `slot.sample_target` or `slot.frame_target`.
/// A megakernel frame would blend more samples into a mean that is to stop, and the wavefront starts no sample past the target,
/// so its floor at the target means no path is in flight.
/// A frame that does not trace counts nothing, which keeps the megakernel's `samples` at `accum_frame` frames of `samples_per_pixel`:
/// the shader's blend weight relies on that once a raised target lets it go on.
/// Lands the wavefront's sample floors first, so the answer is as fresh as the GPU allows.
[[nodiscard]] bool frame_samples(impl::temporal_slot& slot, bool scaled)
{
    land_sample_floors(slot);
    return scaled || !slot.is_at_target();
}

/// Marks a guide or primary-depth `slot` as holding nothing, where there is one.
/// Its `accum_frame` is what says it does: the normal guide's counts the frames written into the guides, and the primary depth's is 1 once a trace wrote it.
void mark_unwritten(impl::temporal_slot* slot)
{
    if (slot != nullptr)
        slot->accum_frame = 0;
}

/// Whether `a` and `b` are one camera, exactly: an image made from one is an image of the other.
[[nodiscard]] bool same_camera(shaders::tracer::camera_record const& a, shaders::tracer::camera_record const& b)
{
    return a.position == b.position && a.forward == b.forward && a.right_scaled == b.right_scaled
        && a.up_scaled == b.up_scaled;
}

/// Marks a denoised slot as holding this frame's image, made from `camera`, which an incomplete frame may present in its place.
void mark_denoised(impl::temporal_slot& slot, shaders::tracer::camera_record const& camera)
{
    slot.accum_frame = 1;
    slot.last_camera = camera;
    slot.has_last_camera = true;
}

/// What a still frame runs under `budget_ms`, its layer's share of the frame's: `rounds_per_frame` at full fill, or what fits the budget once a frame is measured.
///
/// Folds in every frame whose timestamps have landed first, so the plan is as fresh as the GPU allows.
[[nodiscard]] impl::wavefront_plan still_plan(render_settings const& settings, f32 budget_ms, impl::wavefront_pacing& pacing)
{
    auto const fixed = cc::max(1, settings.rounds_per_frame);
    if (budget_ms <= 0.0f)
    {
        pacing.reset();
        return {.rounds = fixed, .fill = 1.0f};
    }

    auto const budget = f64(budget_ms) * 1e-3;
    while (!pacing.in_flight.empty())
    {
        auto const& front = pacing.in_flight.front();
        auto const seconds = front.timing.try_seconds();
        if (!seconds.has_value())
            break;
        auto total = 0.0;
        for (auto const s : seconds.value())
            total += s;
        pacing.land(front, total, budget);
        pacing.in_flight.remove_at(0);
    }

    // A device that times nothing never lands one, so the queue is bounded rather than grown forever.
    constexpr isize max_in_flight = 8;
    while (pacing.in_flight.size() > max_in_flight)
        pacing.in_flight.remove_at(0);

    return pacing.next(budget, fixed);
}

/// Traces one frame of a layer into `slot` with the layer's integrator, `d.frame` holding `fc`, a still wavefront frame under `budget_ms`.
///
/// The wavefront's path state lives on `slot`, so this is where it is sized and where a frame decides between restarting and carrying.
/// A frame whose mean restarts (`accum_frame == 0`) restarts every path and drains: it starts one sample per pixel and runs them to their end,
/// so a moving image is always an exact mean.
/// Every other frame carries the paths the last one left.
/// A `scaled` frame, traced at the upscaler's input extent, always restarts and drains, in paths of its own.
///
/// A pixel starts no sample past `sample_target`, an `effective_sample_target`.
/// Each full-size frame reads back the fewest samples any pixel holds, which raises `slot.samples` a frame or more later.
/// Called only for a frame `frame_samples` says traces.
[[nodiscard]] sg::routine_outcome trace_layer(sg::command_list& cmd,
                                              render_settings const& settings,
                                              pt_trace_desc const& d,
                                              shaders::tracer::frame_constants const& fc,
                                              impl::temporal_slot& slot,
                                              bool scaled,
                                              f32 budget_ms,
                                              u32 sample_target)
{
    if (settings.mode == render_mode::megakernel)
        return pathtrace_routine::execute(cmd, d);

    auto const extent = tg::vec2i(d.output.width(), d.output.height());
    auto const permutations = i32(d.hit_groups.size());
    auto& held = scaled ? slot.scaled_paths : slot.paths;
    if (!held.has_value() || held.value().extent != extent || held.value().max_permutations < permutations
        || held.value().channel_mask != settings.channels)
    {
        // Twice today's permutations, so a material appearing does not reallocate every slot of the view.
        held = wavefront_state::create(cmd.context(), extent, cc::max(16, 2 * permutations), settings.channels);
        // A new state holds no paths, so its mean starts over whatever the hash says.
        impl::restart_accumulation(slot);
    }

    auto& paths = held.value();
    auto const restart = scaled || slot.accum_frame == 0;

    // A walk deeper than a drain is still alive at the frame's end: a still next frame finishes it, and a restart drops it,
    // which biases a moving image only where a medium scatters that often.
    auto const plan = restart
                        ? impl::wavefront_plan{.rounds = wavefront_routine::drain_rounds(fc.max_bounces), .fill = 1.0f}
                        : still_plan(settings, budget_ms, slot.pacing);

    // Only a still frame keeps its rounds as full as its fill, so only its timestamps say what a round costs.
    // A frame whose pixels all held the target never gets here, though `samples` trails the GPU: a frame or two past the
    // target still dispatches, and its near-empty rounds cost only a frame's worth of the queue.
    auto* timing = static_cast<wavefront_timing*>(nullptr);
    if (restart)
        slot.pacing.note_restart();
    else
    {
        if (budget_ms > 0.0f)
            timing = &slot.pacing.queue(plan);
        CC_RECORD_STAT("sv.wavefront.still_rounds", cc::rec::unit_count, plan.rounds);
        CC_RECORD_STAT("sv.wavefront.still_fill", cc::rec::unit_ratio, plan.fill);
    }

    // A scaled frame is no sample of the mean, so only a full-size one says how far the mean has got.
    auto floor = sg::data_future<u32>();
    auto const outcome = wavefront_routine::execute(cmd, {.trace = d,
                                                          .frame = fc,
                                                          .state = &paths,
                                                          .rounds = plan.rounds,
                                                          .restart = restart,
                                                          .regenerate = !restart,
                                                          .fill = plan.fill,
                                                          .roulette_after = wavefront_roulette_after,
                                                          .sample_target = sample_target,
                                                          .timing = timing,
                                                          .sample_floor = scaled ? nullptr : &floor});
    if (floor.is_valid())
        slot.sample_floors.push_back(cc::move(floor));
    // A declined frame records no timestamps, so its entry would never land and would hold back every later one.
    if (timing != nullptr && outcome != sg::routine_outcome::executed)
        slot.pacing.withdraw();
    return outcome;
}

/// Whether a layer traced in `mode` may be traced on `ctx`, saying so once per `slot` when it may not.
///
/// Both tracers need the ray-tracing pipeline: the megakernel is one, and the structure the wavefront's group binds is declared for it.
/// The wavefront traces inline too, so it needs `ray_query` beside it.
/// A layer that cannot trace is declined rather than drawn blank, since nothing it could draw is its image.
/// The feature a traced `mode` needs that `ctx` lacks, or none when it can trace.
[[nodiscard]] cc::optional<sg::feature> missing_trace_feature(sg::context const& ctx, render_mode mode)
{
    if (!ctx.supports(sg::feature::raytracing_pipeline))
        return sg::feature::raytracing_pipeline;
    if ((mode == render_mode::wavefront || mode == render_mode::hybrid) && !ctx.supports(sg::feature::ray_query))
        return sg::feature::ray_query;
    return {};
}

[[nodiscard]] bool can_trace(sg::context const& ctx, render_mode mode, impl::view_state const& rec, impl::temporal_slot& slot)
{
    auto const missing = missing_trace_feature(ctx, mode);
    if (!missing.has_value())
        return true;

    if (!slot.untraceable_reported)
    {
        CC_LOG_ERROR("view '{}' traces a layer on a device without sg::feature::{}, so that layer is not drawn; "
                     "render_mode::raster draws it without ray tracing",
                     rec.display_name, sg::to_string(missing.value()));
        slot.untraceable_reported = true;
    }
    return false;
}

/// Everything a raster layer's frame is drawn from that a trace of it would upload too.
struct raster_scene
{
    resolved_view const* resolved = nullptr;
    pt_light_table const* lights = nullptr;
    traced_decals const* decals = nullptr;
    background_gpu const* background = nullptr;

    /// The trace hash of what is drawn, which a frame drawn through a stand-in still compiling must not leave behind as the slot's.
    u64 hash = 0;
};

/// A layer's raster draws: its permutations, each once, and one draw per row of `resolved`.
struct raster_batch
{
    cc::vector<material_permutation const*> permutations;
    cc::vector<raster_draw> draws;
};

/// The draws of `resolved` a raster pass makes.
[[nodiscard]] raster_batch raster_batch_of(resolved_view const& resolved)
{
    auto batch = raster_batch{};
    batch.permutations.reserve(resolved.hit_groups.size());
    for (auto const* const group : resolved.hit_groups)
        batch.permutations.push_back(group);

    for (auto i = isize(0); i < resolved.instances.size(); ++i)
        batch.draws.push_back({.instance = u32(i),
                               .triangle_count = resolved.triangle_counts[i],
                               .primitive_count = resolved.primitive_counts[i],
                               .permutation = resolved.instances[i].hit_group_offset / 2,
                               .first_index = resolved.records[i].first_index,
                               .base_vertex = resolved.records[i].base_vertex,
                               .bounds = resolved.bounds[i],
                               .indices = resolved.own_indices[i]});
    return batch;
}

/// Draws one frame of raster layer `l` of `v`, layer index `layer`, into `slot` and the G-buffer slots `rec` keeps beside it.
///
/// The slots must exist at `resolution`: the plan's resolve allocates them, and the single-view path ensures them itself.
/// A frame that draws is final, so `slot` is left converged; one drawn through a stand-in still compiling restarts next frame instead.
[[nodiscard]] sg::routine_outcome raster_layer(sg::command_list& cmd,
                                               view_data const& v,
                                               layer const& l,
                                               u8 layer,
                                               tg::vec2i resolution,
                                               raster_scene const& scene,
                                               impl::view_state& rec,
                                               impl::temporal_slot& slot,
                                               gpu_resource_manager& resources)
{
    auto& ctx = cmd.context();
    auto const& resolved = *scene.resolved;

    auto const slot_of = [&](u64 id)
    {
        auto* const s = rec.temporal.get_ptr(id);
        CC_ASSERT(s != nullptr && s->texture.raw() != nullptr, "a raster layer's G-buffer slot was not allocated");
        return s;
    };
    auto* const motion = slot_of(temporal_id::motion_guide(layer));
    auto const batch = raster_batch_of(resolved);

    // The camera, with the aspect of the resolution the frame settled on, as a trace would describe it.
    auto const fc = make_frame_constants(v, l, *scene.lights, resolution);
    auto const frame = shaders::raster::frame_constants{
        .camera = fc.camera,
        .previous_camera = motion->has_last_camera ? motion->last_camera : fc.camera,
        .near_plane = f32(v.camera.projection.near_plane),
        .light_count = u32(scene.lights->records.size()),
        .decal_count = u32(scene.decals->decals.size()),
        .pixel_spread = fc.pixel_spread,
        .pixel_width = fc.pixel_width,
        .size = tg::vec2f(f32(resolution[0]), f32(resolution[1])),
        .jitter = tg::vec2f(0, 0),
    };

    auto const background
        = ctx.transient.create_buffer<background_gpu>(1, sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
    cmd.upload.pod_to_buffer(background, *scene.background);
    auto const instance_table = upload_instances(cmd, resolved);
    auto const light_buffer = upload_lights(cmd, *scene.lights);
    auto const decal_buffers = upload_decals(cmd, *scene.decals, resources.drawings.decal_atlas());

    // Held across the draws: the tables are what a pixel reaches every mesh and texture through.
    // A snapshot that does not fit the descriptor heap declines the layer, as it declines a trace.
    auto frozen = resources.try_freeze();
    if (frozen.has_error())
    {
        CC_LOG_ERROR("the bindless tables could not be snapshot for a raster layer: {}", frozen.error().to_string());
        return sg::routine_outcome::declined;
    }
    auto const& bindless = frozen.value();

    // Every layer keeps one, since a double-click picks off it whether or not anything draws over the frame.
    auto* const primary_depth = rec.temporal.get_ptr(temporal_id::primary_depth(layer));

    auto stand_ins = stand_in_report{};
    auto const outcome = raster_routine::execute(
        cmd, {.frame = frame,
              .background = background,
              .instance_table = instance_table,
              .lights = light_buffer,
              .decals = decal_buffers.decals,
              .decal_shapes = decal_buffers.shapes,
              .decal_atlas = decal_buffers.atlas,
              .draws = batch.draws,
              .index_pool = resources.meshes.index_pool(),
              .permutations = batch.permutations,
              .fallback = &resources.shaders.acquire_fallback(),
              .bindless = &bindless,
              .targets = {.color = slot.texture,
                          .normal = slot_of(temporal_id::normal_guide(layer))->texture,
                          .depth = slot_of(temporal_id::depth_guide(layer))->texture,
                          .albedo = slot_of(temporal_id::albedo_guide(layer))->texture,
                          .specular_albedo = slot_of(temporal_id::specular_albedo_guide(layer))->texture,
                          .roughness = slot_of(temporal_id::roughness_guide(layer))->texture,
                          .motion = motion->texture,
                          .primary_depth = primary_depth != nullptr ? primary_depth->texture : sg::texture_2d()},
              .stand_ins = &stand_ins,
              .visibility = &slot.raster_visibility});
    if (outcome == sg::routine_outcome::declined)
        return outcome;

    motion->last_camera = fc.camera;
    motion->has_last_camera = true;

    // Every frame drawn is the image itself, so the layer is converged the moment one lands.
    // One drawn through a stand-in still compiling is an image of other materials, which must not read as finished.
    // A stand-in for a material that failed for good is the image this layer will keep drawing, so it is final.
    slot.stand_in_hash = stand_ins.hash;
    slot.reset_hash = stand_ins.pending ? cc::combine_hash(scene.hash, u64(1)) : scene.hash;
    slot.accum_frame = 1;
    slot.samples = stand_ins.pending ? 0 : accumulation_sample_cap;
    return outcome;
}

/// The G-buffer slots `rec` keeps for layer `layer`, which a hybrid frame at the view's own size writes; `color` is left to the frame.
[[nodiscard]] raster_gbuffer gbuffer_slots_of(impl::view_state& rec, u8 layer)
{
    auto const slot_of = [&](u64 id)
    {
        auto* const s = rec.temporal.get_ptr(id);
        CC_ASSERT(s != nullptr && s->texture.raw() != nullptr, "a hybrid layer's G-buffer slot was not allocated");
        return s->texture;
    };
    auto const* const primary_depth = rec.temporal.get_ptr(temporal_id::primary_depth(layer));
    return {.normal = slot_of(temporal_id::normal_guide(layer)),
            .depth = slot_of(temporal_id::depth_guide(layer)),
            .albedo = slot_of(temporal_id::albedo_guide(layer)),
            .specular_albedo = slot_of(temporal_id::specular_albedo_guide(layer)),
            .roughness = slot_of(temporal_id::roughness_guide(layer)),
            .motion = slot_of(temporal_id::motion_guide(layer)),
            .primary_depth = primary_depth != nullptr ? primary_depth->texture : sg::texture_2d()};
}

/// What `hybrid_frame` drew: whether it did, and how many samples of a pack it blended, the last at `jitter`.
struct hybrid_pack
{
    sg::routine_outcome outcome = sg::routine_outcome::declined;
    i32 samples = 0;
    tg::vec2f jitter = tg::vec2f(0, 0);
};

/// Draws `samples` samples of hybrid layer `l` of `v` at `extent`, its G-buffer into `guides`, and blends each into `slot`'s mean where `into_mean`.
///
/// A sample is one light sample per pixel and one bounce, so the layer accumulates while its image holds still, as a traced layer does.
/// `fc` is the frame block a trace of the layer would upload, its seed and jitter set, and `slot` must already have restarted on its hash.
/// **Sample `i` of a pack is the one frame `slot.accum_frame + i` would draw**: its seed, its jitter where `fc` fixes one, and its blend weight.
/// So a pack changes how fast the mean climbs, never its image; it shares one upload of the scene and one TLAS, which is what it saves.
/// `samples` above 1 needs `into_mean`, and `jitter` the first sample's offset where `fc` fixes one.
/// A frame drawn for an upscaler is at the upscaler's input extent, into scratch `guides`, and is no sample of the mean.
/// Where `frame_output` is set, it receives the last sample alone, which a temporal denoiser reads.
/// Where `pacing` is set, the pack's GPU time is queued on it.
/// Counts nothing: the caller counts the samples blended, as a trace counts its own frame.
[[nodiscard]] hybrid_pack hybrid_frame(sg::command_list& cmd,
                                       view_data const& v,
                                       layer const& l,
                                       u8 layer,
                                       tg::vec2i extent,
                                       raster_scene const& scene,
                                       shaders::tracer::frame_constants fc,
                                       impl::view_state& rec,
                                       impl::temporal_slot& slot,
                                       gpu_resource_manager& resources,
                                       raster_gbuffer const& guides,
                                       bool into_mean,
                                       sg::texture_2d const& frame_output,
                                       stand_in_report& stand_ins,
                                       i32 samples = 1,
                                       tg::vec2f jitter = tg::vec2f(0, 0),
                                       impl::hybrid_pacing* pacing = nullptr)
{
    CC_ASSERT(samples >= 1 && (samples == 1 || into_mean), "only samples of the mean pack");
    auto declined = hybrid_pack{};
    auto& ctx = cmd.context();
    auto const& resolved = *scene.resolved;
    auto const slot_of = [&](u64 id)
    {
        auto* const s = rec.temporal.get_ptr(id);
        CC_ASSERT(s != nullptr && s->texture.raw() != nullptr, "a hybrid layer's G-buffer slot was not allocated");
        return s;
    };
    auto* const motion = slot_of(temporal_id::motion_guide(layer));
    auto const batch = raster_batch_of(resolved);

    fc.previous_camera = motion->has_last_camera ? motion->last_camera : fc.camera;
    fc.near_plane = f32(v.camera.projection.near_plane);

    auto const background
        = ctx.transient.create_buffer<background_gpu>(1, sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
    cmd.upload.pod_to_buffer(background, *scene.background);
    auto const instance_table = upload_instances(cmd, resolved);
    auto const light_buffer = upload_lights(cmd, *scene.lights);
    auto const emitter_buffer = upload_emitters(cmd, resolved);
    auto const decal_buffers = upload_decals(cmd, *scene.decals, resources.drawings.decal_atlas());

    auto frozen = resources.try_freeze();
    if (frozen.has_error())
    {
        CC_LOG_ERROR("the bindless tables could not be snapshot for a hybrid layer: {}", frozen.error().to_string());
        return declined;
    }
    auto const& bindless = frozen.value();

    // One sample alone, which the mean then takes in by its weight.
    auto const color = ctx.transient.create_texture_2d(
        {.format = sg::pixel_format::rgba32_float,
         .width = extent[0],
         .height = extent[1],
         .usage = sg::texture_usage::render_target | sg::texture_usage::texture | sg::texture_usage::image});
    auto targets = guides;
    targets.color = color;
    // Created once for the pack, as the color is: a sample's targets are rewritten whole by the next.
    if (targets.primary_depth.raw() == nullptr)
        targets.primary_depth
            = ctx.transient.create_texture_2d({.format = sg::pixel_format::r32_float,
                                               .width = extent[0],
                                               .height = extent[1],
                                               .usage = sg::texture_usage::render_target | sg::texture_usage::texture});
    auto const thin = l.settings.hybrid_lighting == hybrid_lighting::deferred
                        ? hybrid_routine::create_thin_targets(ctx, extent)
                        : raster_thin_targets{};
    auto const scratch = raster_routine::create_scratch(ctx, extent);

    // Built here rather than by each sample: the scene holds still across a pack, and on a scene of many instances the build is a fifth of the CPU's frame.
    auto const tlas = cmd.raytracing.build_tlas(resolved.instances);

    auto const begin = pacing != nullptr ? cmd.query.record_gpu_timestamp() : sg::gpu_timestamp();
    auto drawn = hybrid_pack{.outcome = sg::routine_outcome::executed, .jitter = jitter};
    for (auto i = 0; i < samples; ++i)
    {
        auto sample = fc;
        if (i > 0)
        {
            auto const frame = slot.accum_frame + u32(i);
            sample.rng_seed = seed_at(slot, frame);
            if (fc.fixed_jitter != 0)
            {
                drawn.jitter = still_jitter(u32(slot.seed_key & 0xffffu) + frame);
                sample.jitter = drawn.jitter + tg::vec2f(0.5f, 0.5f);
            }
        }

        auto const outcome = hybrid_routine::execute(cmd, {.lighting = l.settings.hybrid_lighting,
                                                           .frame = sample,
                                                           .background = background,
                                                           .instances = resolved.instances,
                                                           .tlas = tlas,
                                                           .instance_table = instance_table,
                                                           .lights = light_buffer,
                                                           .emitters = emitter_buffer,
                                                           .decals = decal_buffers.decals,
                                                           .decal_shapes = decal_buffers.shapes,
                                                           .decal_atlas = decal_buffers.atlas,
                                                           .draws = batch.draws,
                                                           .index_pool = resources.meshes.index_pool(),
                                                           .permutations = batch.permutations,
                                                           .fallback = &resources.shaders.acquire_fallback(),
                                                           .bindless = &bindless,
                                                           .targets = targets,
                                                           .thin = thin,
                                                           .scratch = scratch,
                                                           .stand_ins = &stand_ins,
                                                           .visibility = &slot.raster_visibility});

        // A sample the blends could not take in yet is drawn for nothing, so it counts nothing.
        // One that declines past the first ends the pack on the samples already blended.
        auto const blended
            = outcome != sg::routine_outcome::declined
           && (!into_mean
               || sr::mix_routine::execute(cmd, slot.texture, color, 1.0f / f32(slot.accum_frame + u32(i) + 1)))
           && (i + 1 < samples || frame_output.raw() == nullptr
               || sr::mix_routine::execute(cmd, frame_output, color, 1.0f));
        if (!blended)
        {
            if (i == 0)
                return declined;
            break;
        }
        drawn.samples = i + 1;
    }
    impl::gpu_mark(cmd, "mix");
    if (pacing != nullptr)
        pacing->in_flight.push_back({.begin = begin, .end = cmd.query.record_gpu_timestamp(), .samples = drawn.samples});

    motion->last_camera = fc.camera;
    motion->has_last_camera = true;
    return drawn;
}

/// `hybrid_frame` for the single-view path, which denoises nothing: draws nothing once `slot` holds `target`, and counts what it draws.
[[nodiscard]] sg::routine_outcome hybrid_layer(sg::command_list& cmd,
                                               view_data const& v,
                                               layer const& l,
                                               u8 layer,
                                               tg::vec2i resolution,
                                               raster_scene const& scene,
                                               shaders::tracer::frame_constants const& fc,
                                               u32 target,
                                               impl::view_state& rec,
                                               impl::temporal_slot& slot,
                                               gpu_resource_manager& resources)
{
    slot.sample_target = target;
    if (!frame_samples(slot, false))
        return sg::routine_outcome::executed;
    auto seeded = fc;
    seeded.rng_seed = seed_of(slot);
    auto stand_ins = stand_in_report{};
    auto const outcome = hybrid_frame(cmd, v, l, layer, resolution, scene, seeded, rec, slot, resources,
                                      gbuffer_slots_of(rec, layer), true, {}, stand_ins)
                             .outcome;
    if (outcome == sg::routine_outcome::declined)
        return outcome;

    // As a trace: one drawn through a stand-in still compiling, or short of an emitter still loading, restarts next frame.
    slot.stand_in_hash = stand_ins.hash;
    if (stand_ins.pending || scene.resolved->emitters_pending)
        slot.reset_hash = cc::combine_hash(slot.reset_hash, u64(1));
    count_traced_frame(slot, l.settings);
    return outcome;
}
} // namespace

render_mode resolve_render_mode(sg::context const& ctx, render_mode mode)
{
    if (mode != render_mode::automatic)
        return mode;
    auto const traces = ctx.supports(sg::feature::raytracing_pipeline) && ctx.supports(sg::feature::ray_query);
    return traces ? render_mode::hybrid : render_mode::raster;
}

// NOLINTNEXTLINE(performance-unnecessary-value-param) — by value because it is a coroutine, see sg's init_once
cc::shared_async<cc::unit> view_renderer::init(sg::routine_init_scope scope)
{
    auto& ctx = scope.context();

    // Warmed rather than declared: a raster layer must draw without waiting on the tracer, and on a device without ray
    // tracing it never could.
    // A traced layer finds the tracer still initializing on its first frame at most, and declines that frame as it would anyway.
    if (ctx.supports(sg::feature::raytracing_pipeline))
        pathtrace_routine::prewarm(ctx);
    if (ctx.supports(sg::feature::raytracing_pipeline) && ctx.supports(sg::feature::ray_query))
        wavefront_routine::prewarm(ctx);

    // Warmed rather than declared: a layer that never crossfades must not wait on it, and one that does reaches the
    // fade a good many frames after the first trace — so the compile has long since finished by then.
    // It compiles per destination format, and what it fades is a denoised slot, which view_data declares half float.
    sr::mix_routine::prewarm(ctx, sg::pixel_format::rgba16_float);
    // A hybrid layer blends each frame into its full-float mean.
    sr::mix_routine::prewarm(ctx, sg::pixel_format::rgba32_float);

    // This runs again on every reload, which is exactly when an accumulated image stops being comparable to a fresh one.
    ++_shader_generation;
    co_return;
}

plan_resources view_renderer::resolve(sg::command_list& cmd, render_plan const& plan, view_store& store)
{
    auto& ctx = cmd.context();

    auto out = plan_resources{};
    out.targets.resize_to_defaulted(plan.targets.size());
    out.traces.resize_to_defaulted(plan.traces.size());
    out.scenes.resize_to_defaulted(plan.traces.size());

    // Touch first, and touch everything: a view that is throttled this frame is still sampled by its parent, so
    // letting the idle reclaim pass over it would release a texture that is about to be read.
    for (auto const id : plan.reachable)
        (void)store.get_or_create(id);

    for (auto i = u32(0); i < plan.targets.size(); ++i)
    {
        auto const& t = plan.targets[i];
        if (t.is_output)
            continue; // the caller's texture; nothing of ours allocates or keeps it

        auto& rec = store.get_or_create(t.id);
        auto const stale = rec.composite.raw() == nullptr || rec.composite.width() != t.resolution[0]
                        || rec.composite.height() != t.resolution[1];
        if (stale)
        {
            if (rec.composite.raw() != nullptr)
                rec.composite.raw()->expire();
            rec.composite = ctx.persistent.create_texture_2d(
                {.format = t.format,
                 .width = t.resolution[0],
                 .height = t.resolution[1],
                 .usage = sg::texture_usage::texture | sg::texture_usage::render_target});
        }
        out.targets[i] = rec.composite;
    }

    // Every temporal resource every reachable view declared, whatever writes it — the tracer's accumulators are just
    // the ones a scene_3d layer implies.
    for (auto const& t : plan.temporals)
    {
        auto const e = ensure_temporal(ctx, store, t.id, t.temporal_id, t.resolution, t.format);

        // A fresh texture holds no history, and a declaration that changed describes something else.
        //
        // Against `declared_hash`, never `reset_hash`: the latter belongs to whatever writes the resource, and is
        // stamped later in the frame by `trace`.
        // Comparing or overwriting it here would pit the two against each
        // other and restart the accumulation on every frame.
        if (e.resized || e.slot->declared_hash != t.reset_hash)
            impl::restart_accumulation(*e.slot);
        e.slot->declared_hash = t.reset_hash;
    }

    for (auto i = u32(0); i < plan.traces.size(); ++i)
    {
        auto const& tr = plan.traces[i];
        auto& rec = store.get_or_create(tr.id);
        auto const* const slot = rec.temporal.get_ptr(temporal_id::accumulation(tr.layer));

        // The loop above allocated it, so a miss means the plan named a trace it declared no accumulator for.
        CC_ASSERT(slot != nullptr, "a plan trace has no accumulator among its view's temporal inputs");
        out.traces[i] = slot->texture;

        // What a parent samples is the denoised image once there is one, which is also what a throttled view re-presents.
        // Only while this frame still declares it: a layer that stopped denoising leaves the slot behind until the store
        // reclaims it, and presenting that would show a frozen image.
        if (declares(plan, tr.id, temporal_id::denoised(tr.layer)))
        {
            auto const* const d = rec.temporal.get_ptr(temporal_id::denoised(tr.layer));
            if (d != nullptr && d->accum_frame > 0)
                out.traces[i] = d->texture;
        }
    }

    // One budget entry per view, covering everything it holds — the store reclaims a view's textures whole or not at all.
    for (auto const id : plan.reachable)
    {
        auto const* const rec = store.peek_ptr(id);
        if (rec == nullptr)
            continue;

        auto bytes = isize(0);
        if (rec->composite.raw() != nullptr)
            bytes += texture_bytes(tg::vec2i(rec->composite.width(), rec->composite.height()),
                                   rec->composite.raw()->description().format);
        for (auto const& [temporal_id, slot] : rec->temporal)
        {
            if (slot.texture.raw() != nullptr)
                bytes += texture_bytes(tg::vec2i(slot.texture.width(), slot.texture.height()),
                                       slot.texture.raw()->description().format);
            if (slot.paths.has_value())
                bytes += slot.paths.value().size_in_bytes();
            if (slot.scaled_paths.has_value())
                bytes += slot.scaled_paths.value().size_in_bytes();
        }
        store.set_payload_bytes(id, bytes);
    }

    return out;
}

void view_renderer::acquire_trace(sg::command_list& cmd,
                                  viewer_definition const& def,
                                  render_plan const& plan,
                                  u32 trace_index,
                                  plan_resources& res,
                                  gpu_resource_manager& resources)
{
    auto const& tr = plan.traces[trace_index];
    CC_ASSERT(isize(u32(tr.view)) < def.views.size(), "a plan trace names a view the definition does not hold");
    auto const& v = def[tr.view];
    CC_ASSERT(isize(tr.layer) < v.layers.size(), "a plan trace names a layer the view does not hold");
    if (res.traces[trace_index].raw() == nullptr)
        return; // trace() will not run it either
    // The walk trace() takes, kept for it: residency only moves at the frame's settle pass, so it still holds when the
    // trace records — under the same mode, which decides what a walk acquires.
    auto const& l = v.layers[tr.layer];
    auto const mode = planned_mode(l.settings.mode);
    auto const is_traced = mode != render_mode::raster;
    // trace() declines a layer this device cannot trace, and says so; a walk for it would build BLASes nothing can.
    if (is_traced && missing_trace_feature(cmd.context(), mode).has_value())
        return;
    res.scenes[trace_index] = resolve_scene(cmd, l, resources, mode);
}

sg::routine_outcome view_renderer::trace(sg::command_list& cmd,
                                         viewer_definition const& def,
                                         render_plan const& plan,
                                         u32 trace_index,
                                         plan_resources& res,
                                         gpu_resource_manager& resources,
                                         view_store& store)
{
    auto& ctx = cmd.context();

    auto const& tr = plan.traces[trace_index];
    CC_ASSERT(isize(u32(tr.view)) < def.views.size(), "a plan trace names a view the definition does not hold");
    auto const& v = def[tr.view];
    CC_ASSERT(isize(tr.layer) < v.layers.size(), "a plan trace names a layer the view does not hold");
    auto const& l = v.layers[tr.layer];

    if (res.traces[trace_index].raw() == nullptr)
        return sg::routine_outcome::executed; // resolve() refused it; there was nothing to trace

    // Held for the whole trace because the reload generation is read under it.
    auto self = try_acquire_exclusive(cmd);
    if (!self.is_ready())
        return sg::routine_outcome::declined;

    auto& rec = store.get_or_create(v.id);
    auto* const slot = rec.temporal.get_ptr(temporal_id::accumulation(tr.layer));
    if (slot == nullptr)
        return sg::routine_outcome::executed; // resolve() refused it; there was nothing to accumulate

    auto const mode = planned_mode(l.settings.mode);
    auto const is_traced = mode != render_mode::raster;
    if (is_traced && !can_trace(ctx, mode, rec, *slot))
        return sg::routine_outcome::declined;

    auto resolved = res.scenes[trace_index].has_value() ? cc::move(res.scenes[trace_index].value())
                                                        : resolve_scene(cmd, l, resources, mode);
    res.scenes[trace_index] = cc::nullopt;

    // The aspect comes from the resolution the plan settled on, not the definition's own field: a layout-following
    // view's resolution is decided by the rect it landed in.
    auto const lights = traced_lights(l, resolved);
    link_emitters(resolved, lights);
    auto const decals = decals_of(l, resources.drawings.decal_atlas());
    auto fc = make_frame_constants(v, l, lights, tr.resolution);
    fc.decal_count = u32(decals.decals.size());
    // Only the megakernel reads it, so no other mode restarts on it.
    if (mode != render_mode::megakernel)
        fc.samples_per_pixel = 0;
    auto const bg = background_gpu::from(l.background.seen_from(v.camera));
    // The integrator is folded in too: both converge to the same image, but a mean must not hold samples of both.
    // So is what the last frame stood in for a permutation not compiled, which only the routine decides.
    auto const hash
        = cc::combine_hash(trace_hash(fc, bg, lights, decals, resolved, tr.resolution, self->_shader_generation),
                           cc::make_hash(u32(mode), u32(l.settings.hybrid_lighting), slot->stand_in_hash));

    // A cut teleports the camera, so nothing from the last frame reprojects: the temporal history describes another
    // part of the scene, and the motion guide would reproject into a camera nothing connects it to.
    // Every slot of the view at once rather than this layer's, so a view carrying two traced layers cuts as one.
    //
    // **A cut is the only thing that restarts a temporal history**, where the mean restarts on any change of the image.
    // A history follows content per pixel — motion, deformation, meshes and lights coming and going — which is what it exists for.
    // The denoiser itself rebuilds it on a new extent or member.
    if (rec.camera_cut_pending)
    {
        for (auto&& [slot_id, s] : rec.temporal)
        {
            (void)slot_id;
            s.denoise.reset();
            s.scaled_denoise.reset();
            s.has_last_camera = false;
        }
        rec.camera_cut_pending = false;
    }

    // The path state is the wavefront's alone, and the store's budget counts it for as long as the slot holds it.
    if (mode != render_mode::wavefront)
    {
        slot->paths = {};
        slot->scaled_paths = {};
    }

    // A rasterized layer draws its final image and its G-buffer in one frame, so nothing below — accumulation,
    // denoising, upscaling — has anything to do with it.
    if (!is_traced)
        return raster_layer(
            cmd, v, l, tr.layer, tr.resolution,
            {.resolved = &resolved, .lights = &lights, .decals = &decals, .background = &bg, .hash = hash}, rec, *slot,
            resources);

    // Whether this frame's image differs from the last one's, which is what may trace it at the upscaler's extent.
    auto const image_changed = slot->reset_hash != hash;

    // A move lasts a moment past the image's last change, so a frame without input in the middle of one keeps its stream.
    auto const now = cc::current_time_steady_secs();
    if (image_changed)
        slot->last_change_seconds = now;
    auto const moving = image_changed || (now - slot->last_change_seconds) * 1000.0 < f64(l.settings.motion_hold_ms);

    // A different image must not be averaged into the old one.
    // The tracer publishes its own hash as this slot's reset rule: it covers the bytes actually uploaded, so it
    // cannot drift away from what the shader reads the way a hand-declared one could.
    if (slot->reset_hash != hash)
        impl::restart_accumulation(*slot);
    slot->reset_hash = hash;
    slot->seed_key = seed_key_of(fc, bg, lights, resolved, tr.resolution);

    // A megakernel layer under a noise target keeps per-pixel statistics at its extent.
    // Statistics made now describe no earlier frame, so a mean that was already climbing starts over with them.
    auto const noise = mode == render_mode::megakernel && l.settings.noise_target > 0.0f;
    if (noise
        && (!slot->noise_adaptive || slot->pixel_stats.raw() == nullptr || slot->pixel_stats.width() != tr.resolution[0]
            || slot->pixel_stats.height() != tr.resolution[1]))
    {
        // copy_src too, so a test or a tool reads back where the samples went.
        slot->pixel_stats
            = ctx.persistent.create_texture_2d({.format = sg::pixel_format::rg32_float,
                                                .width = tr.resolution[0],
                                                .height = tr.resolution[1],
                                                .usage = sg::texture_usage::image | sg::texture_usage::copy_src});
        slot->noise_counts = ctx.persistent.create_texture_2d(
            {.format = sg::pixel_format::r32_float,
             .width = tr.resolution[0],
             .height = tr.resolution[1],
             .usage = sg::texture_usage::image | sg::texture_usage::texture | sg::texture_usage::copy_src});
        if (slot->noise_counters.raw() == nullptr)
            slot->noise_counters = ctx.persistent.create_buffer<u32>(
                noise_counter_count,
                sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_dst | sg::buffer_usage::copy_src);
        if (slot->accum_frame > 0)
            impl::restart_accumulation(*slot);
    }
    if (!noise)
    {
        slot->pixel_stats = {};
        slot->noise_counts = {};
    }
    slot->noise_adaptive = noise;


    // The denoiser's slots, when this layer denoises: the guides and the output are declared together, and the two
    // temporal ones only when the layer may denoise temporally.
    auto const denoising = l.settings.reconstruct.denoiser != sr::denoise_method::none;
    auto const slot_of = [&](u64 id) { return denoising ? rec.temporal.get_ptr(id) : nullptr; };

    // Whether the member that will actually run reads the two lobes apart.
    // Resolved against the device here rather than taken from the slots existing: `automatic` DECLARES them, because
    // that declaration is made before any device is consulted, and on a machine whose best member ignores them the
    // tracer would otherwise split every frame's radiance for nobody.
    //
    // Asked once rather than per slot: the three travel together, and `resolve_denoise_method` re-probes every
    // member's shaders on `automatic`.
    // A hybrid frame writes no split signal, so a member wanting one reads the whole radiance instead.
    auto const splitting = [&]
    {
        if (!denoising || mode == render_mode::hybrid)
            return false;
        // The split signals are this frame's own samples, so it is the temporal member's answer that decides.
        auto fresh = l.settings.reconstruct;
        fresh.fresh_samples = true;
        // Asked against what the tracer CAN write rather than what this frame has, because this is the call that
        // decides what it writes — reading the slots here would say no on the first frame and no forever after.
        return sr::required_guides(sr::resolve_denoise_method(ctx, fresh, traceable_guides()))
            .has(sr::reconstruct_guide::split_diffuse_specular);
    }();
    // A hybrid frame writes no split signal, but its deferred kernel writes the hit distance, which dlss_rr reads beside the whole radiance.
    // Resolved against the device for the reason `splitting` is.
    auto const hybrid_hit_distance = [&]
    {
        if (!denoising || mode != render_mode::hybrid || l.settings.hybrid_lighting != hybrid_lighting::deferred)
            return false;
        auto fresh = l.settings.reconstruct;
        fresh.fresh_samples = true;
        auto const member = sr::resolve_denoise_method(ctx, fresh, traceable_guides());
        return (sr::required_guides(member) | sr::optional_guides(member)).has(sr::reconstruct_guide::hit_distance);
    }();
    auto const split_slot_of = [&](u64 id) { return splitting ? rec.temporal.get_ptr(id) : nullptr; };
    auto const ds = denoise_slots{
        .normal = slot_of(temporal_id::normal_guide(tr.layer)),
        .depth = slot_of(temporal_id::depth_guide(tr.layer)),
        .albedo = slot_of(temporal_id::albedo_guide(tr.layer)),
        .specular_albedo = slot_of(temporal_id::specular_albedo_guide(tr.layer)),
        .roughness = slot_of(temporal_id::roughness_guide(tr.layer)),
        .denoised = slot_of(temporal_id::denoised(tr.layer)),
        .frame = slot_of(temporal_id::frame_samples(tr.layer)),
        .motion = slot_of(temporal_id::motion_guide(tr.layer)),
        .crossfade = slot_of(temporal_id::denoised_crossfade(tr.layer)),
        .frame_diffuse = split_slot_of(temporal_id::frame_diffuse(tr.layer)),
        .frame_specular = split_slot_of(temporal_id::frame_specular(tr.layer)),
        .hit_distance
        = splitting || hybrid_hit_distance ? rec.temporal.get_ptr(temporal_id::hit_distance_guide(tr.layer)) : nullptr,
    };
    // The guides are declared together, so they are all present or all absent.
    auto const has_guides = ds.normal != nullptr && ds.depth != nullptr && ds.albedo != nullptr
                         && ds.specular_albedo != nullptr && ds.roughness != nullptr && ds.denoised != nullptr;
    auto const has_temporal = has_guides && ds.frame != nullptr && ds.motion != nullptr;

    // The depth a layer's 3D drawings are occluded by and a pick reads — set after the hash, so a drawing coming or going
    // never restarts the image.
    auto* const depth_slot = rec.temporal.get_ptr(temporal_id::primary_depth(tr.layer));
    auto const draws_over = !l.drawings.empty() || !l.annotations.empty();

    // A guide slot the layer stops asking for stays until the store reclaims it, holding nothing of the means after it.
    // Marked unwritten, so asking for it again is asking for a new one.
    if (!has_guides)
        mark_unwritten(rec.temporal.get_ptr(temporal_id::normal_guide(tr.layer)));

    // Whether a full-size frame traces at all, which one whose pixels all hold the target does not.
    // Asked before anything is scheduled on it, since a frame that traces nothing has no samples of its own to denoise.
    //
    // Guides first asked for, or a primary depth no full-size frame has written yet, once the mean holds samples are
    // written by the next frame that traces, from what that frame or the wavefront's tallies saw of every pixel.
    // At the target no frame traces, so the mean starts over instead: a denoiser would read blank guides, and a drawing test or a pick blank depth.
    auto const target = effective_sample_target(def.sample_target);
    slot->sample_target = target;
    // The last frame of an image traces once more and reaches its target, so it also takes the denoise kept for the target.
    slot->frame_target = def.last_frame_of_image ? slot->accum_frame + 1 : def.frame_target;
    auto const guides_unwritten = has_guides && slot->accum_frame > 0 && ds.normal->accum_frame == 0;
    auto const depth_unwritten = depth_slot != nullptr && slot->accum_frame > 0 && depth_slot->accum_frame == 0;
    auto const at_target = !frame_samples(*slot, false);
    if (at_target && (guides_unwritten || depth_unwritten))
        impl::restart_accumulation(*slot);
    auto const samples = !at_target || slot->accum_frame == 0;

    // The shader reads accum_frame == 0 as "overwrite", anything above it as "blend in place".
    fc.accum_frame = slot->accum_frame;

    // Set after the hash, like accum_frame, so none of it reaches the hash.
    // The guides count on the normal slot's own frames, since they may have started after the mean did.
    // They describe the same image, so they restart whenever it does.
    if (has_guides)
    {
        if (slot->accum_frame == 0)
            ds.normal->accum_frame = 0;
        fc.write_guides = 1;
        fc.guide_frame = ds.normal->accum_frame;
        fc.write_specular_guides = 1;

        // Where a partial coverage can cut out, the megakernel takes its guides from a guide ray per sample, as the
        // wavefront does from one per pixel, which `wavefront_routine` decides for itself.
        if (mode == render_mode::megakernel)
            for (auto const* p : resolved.hit_groups)
                if (p->opacity.can_cut_out())
                    fc.wavefront_flags |= impl::wavefront_flag_guide_rays;
    }

    // Decided before the frame block is uploaded, so the tracer writes this frame's own samples and motion only on the
    // frames the temporal member reads them.
    // On the mean's count as `_denoise` sees it, after this frame's increment below, which only a frame that traces makes.
    // What the display multiplies this layer's image by, its automatic exposure included once a reading has landed.
    auto display_exposure = l.settings.display.exposure;
    if (l.settings.display.automatic && isize(tr.layer) < rec.auto_exposure.size() && rec.auto_exposure[tr.layer].primed)
        display_exposure += rec.auto_exposure[tr.layer].exposure;
    auto const display_scale = sr::display_transform{.exposure = display_exposure}.exposure_scale();
    auto const schedule = has_guides
                            ? _schedule_denoise(ctx, l.settings, ds,
                                                samples ? next_frame_count(*slot) : slot->accum_frame, display_scale)
                            : denoise_schedule{};
    auto const writes_temporal = has_temporal && schedule.runs_temporal();

    // All three or none: a member filtering the lobes apart needs both halves AND the distances, and two of the three
    // would let it run against a hit distance that describes something else.
    // Gated on `writes_temporal` like the rest of the temporal signals, so a frame no temporal member will read does
    // not pay to split its radiance.
    auto const has_split
        = writes_temporal && ds.frame_diffuse != nullptr && ds.frame_specular != nullptr && ds.hit_distance != nullptr;
    // The hit distance alone, which a deferred hybrid frame writes for a member that reads it beside the whole radiance.
    auto const has_hit_distance = writes_temporal && hybrid_hit_distance && ds.hit_distance != nullptr;

    if (writes_temporal)
    {
        fc.write_temporal = 1;
        fc.previous_camera = ds.motion->has_last_camera ? ds.motion->last_camera : fc.camera;
        fc.write_split = has_split ? 1 : 0;
        fc.write_hit_distance = has_hit_distance ? 1 : 0;
    }

    // A frame of a move traces at the upscaler's input extent, every primary ray at one jitter, and the temporal member
    // denoises it on its own samples before the upscaler brings it to the view's size.
    // The first frame past the move's hold restarts at full size and accumulates as any still frame does.
    // Only where the temporal member runs, since an upscaler does not denoise, and never under drawings, whose depth
    // test reads the primary depth at the view's own size.
    auto upscale_settings = schedule.temporal_settings;
    upscale_settings.upscaler = l.settings.reconstruct.upscaler;
    // A layer that can never trace at the upscaler's extent holds no paths for it.
    if (!has_temporal || draws_over || upscale_settings.upscaler == sr::upscale_method::none)
        slot->scaled_paths = {};
    auto const may_scale = moving && writes_temporal && !draws_over;
    auto const input_extent = may_scale
                                ? sr::reconstruct_input_extent(ctx, upscale_settings, tr.resolution, traceable_guides())
                                : tr.resolution;
    auto const scaled = input_extent != tr.resolution;

    // A scaled frame seeds from its own count below.
    if (!scaled)
        fc.rng_seed = seed_of(*slot);
    // A scaled frame is an image of its own rather than a sample of the mean, so it traces whatever the mean holds.
    auto const traces = samples || scaled;

    // Written only at the slot's own size: a scaled frame leaves it to the next full-size one, and keeps
    // `accum_frame` at 0, which a pick reads as no depth to take yet.
    if (depth_slot != nullptr && !scaled)
    {
        fc.write_primary_depth = 1;
        fc.near_plane = f32(v.camera.projection.near_plane);
    }

    auto jitter = tg::vec2f(0, 0);
    if (scaled)
    {
        // Each stream's history reprojects only from the frame before it, so a switch from the other one starts it over.
        if (!slot->traced_scaled)
            ds.frame->scaled_denoise.reset();

        jitter = sr::reconstruct_jitter(ctx, upscale_settings, tr.resolution, slot->scaled_frame, traceable_guides());
        fc.fixed_jitter = 1;
        fc.jitter = jitter + tg::vec2f(0.5f, 0.5f);
        set_pixel_footprint(fc, input_extent[1]);
        // an upscaled frame is one sample a pixel, averaged with no others, so its primaries read a whole pixel
        fc.primary_scale = 1.0f;
        // A moving frame's mean restarts every time, so the accumulation's seed would draw one sequence on all of them.
        fc.rng_seed = slot->scaled_frame + 1;
        fc.guide_frame = 0;
        ++slot->scaled_frame;
    }
    else if (slot->traced_scaled && has_temporal)
    {
        ds.frame->denoise.reset();
    }

    // A raster samples each pixel where the projection puts its center, so a full-size hybrid frame shifts the whole
    // projection by a sub-pixel offset of its own, as a traced frame's rays each draw one, or its mean would converge
    // to an aliased image: every edge a staircase, however long the view stands still.
    auto previous_jitter = slot->last_jitter;
    if (!scaled && mode == render_mode::hybrid)
    {
        // Offset by the image's seed key: a move restarts the mean every frame, and the temporal member reconstructs
        // sub-pixel detail only from an offset that changes from frame to frame.
        jitter = still_jitter(u32(slot->seed_key & 0xffffu) + slot->accum_frame);
        fc.fixed_jitter = 1;
        fc.jitter = jitter + tg::vec2f(0.5f, 0.5f);
        slot->last_jitter = jitter;
    }

    // How many samples each pixel averages, where the wavefront traces a layer the spatial denoiser reads: its pixels
    // finish different numbers of paths, so one count for the image would misjudge every one of them.
    // Transient, since the tallies hold the counts and the resolve rewrites every pixel each frame.
    // None on a frame that traces nothing, since no resolve writes it then: every pixel holds the target, which one count says.
    auto const sample_counts
        = traces && has_guides && mode == render_mode::wavefront
            ? ctx.transient.create_texture_2d({.format = sg::pixel_format::r32_float,
                                               .width = input_extent[0],
                                               .height = input_extent[1],
                                               .usage = sg::texture_usage::texture | sg::texture_usage::image})
            : sg::texture_2d();

    // What the trace writes: the layer's own slots, or on a scaled frame scratch at the input extent, which the
    // upscale reads and the epoch drops.
    auto const image_of = [&](impl::temporal_slot const* s, bool written, sg::pixel_format format) -> sg::texture_2d
    {
        if (!written)
            return {};
        if (!scaled)
            return s->texture;
        // A render target too, since a hybrid frame rasterizes its G-buffer into it.
        return ctx.transient.create_texture_2d(
            {.format = format,
             .width = input_extent[0],
             .height = input_extent[1],
             .usage = sg::texture_usage::texture | sg::texture_usage::image | sg::texture_usage::render_target});
    };
    auto const output = image_of(slot, true, sg::pixel_format::rgba32_float);
    auto const guide_normal = image_of(ds.normal, has_guides, sg::pixel_format::rgba16_float);
    auto const guide_depth = image_of(ds.depth, has_guides, sg::pixel_format::r32_float);
    auto const guide_albedo = image_of(ds.albedo, has_guides, sg::pixel_format::rgba16_float);
    auto const guide_specular_albedo = image_of(ds.specular_albedo, has_guides, sg::pixel_format::rgba16_float);
    auto const guide_roughness = image_of(ds.roughness, has_guides, sg::pixel_format::r16_float);
    auto const frame_output = image_of(ds.frame, writes_temporal, sg::pixel_format::rgba16_float);
    auto const guide_motion = image_of(ds.motion, writes_temporal, sg::pixel_format::rg32_float);
    auto const frame_diffuse = image_of(ds.frame_diffuse, has_split, sg::pixel_format::rgba16_float);
    auto const frame_specular = image_of(ds.frame_specular, has_split, sg::pixel_format::rgba16_float);
    auto const guide_hit_distance
        = image_of(ds.hit_distance, has_split || has_hit_distance, sg::pixel_format::rg32_float);

    // Everything the dispatch reads beside those, uploaded only by a frame that traces.
    // The scene's resolve and the hash above run on every frame: the hash is what notices a change that restarts the mean,
    // and the resolve is what keeps the scene's working set resident.
    auto stand_ins = stand_in_report{};

    // This layer's share of the frame's budget for still layers, which a wavefront frame fills with rounds and a hybrid one with samples.
    auto const budget_ms = impl::still_budget_share(def.still_budget_ms, impl::still_budgeted_traces(def, plan, store));

    // How many samples a hybrid frame packs: one, unless it is still and no temporal member reads this frame's samples alone.
    // A frame whose mean restarted is a move's, and the temporal member reading fresh samples must see one frame's.
    // Never past a target, so a capture stops on the sample it would have stopped on a frame at a time.
    auto pack = 1;
    auto* pacing = static_cast<impl::hybrid_pacing*>(nullptr);
    if (mode == render_mode::hybrid && traces && !scaled && slot->accum_frame > 0
        && (!writes_temporal || schedule.temporal_on_mean))
    {
        if (budget_ms > 0.0f)
        {
            pack = slot->sample_pacing.next(f64(budget_ms) * 1e-3);
            pacing = &slot->sample_pacing;
        }
        else
        {
            slot->sample_pacing.reset();
            pack = i32(cc::max(1u, def.still_samples_per_frame));
        }
        auto remaining = target > slot->samples ? target - slot->samples : 1u;
        if (slot->frame_target > 0)
            remaining = cc::min(remaining,
                                slot->frame_target > slot->accum_frame ? slot->frame_target - slot->accum_frame : 1u);
        pack = i32(cc::clamp(u32(pack), 1u, remaining));
    }
    auto packed = 1;

    auto const dispatch = [&]
    {
        // A hybrid frame draws its primary hits and lights them inline, then blends into the mean the trace would have.
        // A scaled one draws at the upscaler's input extent into the scratch guides the upscale reads, and is no sample of the mean.
        if (mode == render_mode::hybrid)
        {
            auto const drawn = hybrid_frame(
                cmd, v, l, tr.layer, input_extent,
                {.resolved = &resolved, .lights = &lights, .decals = &decals, .background = &bg, .hash = hash}, fc, rec,
                *slot, resources,
                scaled ? raster_gbuffer{.normal = guide_normal,
                                        .depth = guide_depth,
                                        .albedo = guide_albedo,
                                        .specular_albedo = guide_specular_albedo,
                                        .roughness = guide_roughness,
                                        .motion = guide_motion,
                                        .hit_distance = guide_hit_distance}
                       : [&]
                {
                    auto slots = gbuffer_slots_of(rec, tr.layer);
                    slots.hit_distance = guide_hit_distance;
                    return slots;
                }(),
                !scaled, frame_output, stand_ins, pack, jitter, pacing);
            packed = cc::max(1, drawn.samples);
            // The last sample's offset is what the next frame's upscaler reads as the previous one.
            if (!scaled && fc.fixed_jitter != 0)
            {
                jitter = drawn.jitter;
                slot->last_jitter = jitter;
            }
            return drawn.outcome;
        }

        // Under a noise target the megakernel judges every pixel against it after the display's scale, and counts the ones still tracing.
        // A scaled frame is no sample of the mean, so it keeps no statistics.
        auto const noise_on = noise && !scaled;
        if (noise_on)
        {
            fc.noise_target = l.settings.noise_target;
            fc.noise_scale = display_scale;
            fc.noise_min_frames = noise_min_frames;
            cmd.upload.data_to_buffer(slot->noise_counters, cc::vector<u32>::create_defaulted(noise_counter_count));
        }

        auto const frame = ctx.transient.create_buffer<shaders::tracer::frame_constants>(
            1, sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
        cmd.upload.pod_to_buffer(frame, fc);

        auto const background = ctx.transient.create_buffer<background_gpu>(
            1, sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
        cmd.upload.pod_to_buffer(background, bg);

        auto const instance_table = upload_instances(cmd, resolved);
        auto const light_buffer = upload_lights(cmd, lights);
        auto const emitter_buffer = upload_emitters(cmd, resolved);
        auto const decal_buffers = upload_decals(cmd, decals, resources.drawings.decal_atlas());

        // Held across the dispatch: the tables are what the closest-hit reaches every mesh and texture through, and
        // nothing may mint a descriptor the bound snapshot would not contain while it is being recorded against.
        // A snapshot the descriptor heap cannot hold declines this trace and leaves the frame standing.
        auto frozen = resources.try_freeze();
        if (frozen.has_error())
        {
            CC_LOG_ERROR("the bindless tables could not be snapshot for a trace: {}", frozen.error().to_string());
            return sg::routine_outcome::declined;
        }
        auto const& bindless = frozen.value();

        // Acquired as soon as the view holds a quadric batch, rather than when one is caught uncompiled: the trace
        // declines entirely while a quadric permutation is unready and this is not, so a late acquire blanks the view for
        // however long the stand-in takes to compile.
        // A triangle-only view still never asks for it.
        auto const* const quadric_fallback
            = resolved.has_quadrics ? &resources.shaders.acquire_quadric_fallback() : nullptr;

        auto const outcome = trace_layer(
            cmd, l.settings,
            {.frame = frame,
             .background = background,
             .instances = resolved.instances,
             .output = output,
             .guide_normal = guide_normal,
             .guide_depth = guide_depth,
             .guide_albedo = guide_albedo,
             .guide_specular_albedo = guide_specular_albedo,
             .guide_roughness = guide_roughness,
             .frame_output = frame_output,
             .guide_motion = guide_motion,
             .frame_diffuse = frame_diffuse,
             .frame_specular = frame_specular,
             .guide_hit_distance = guide_hit_distance,
             .primary_depth = depth_slot != nullptr ? depth_slot->texture : sg::texture_2d(),
             .sample_counts = noise_on ? slot->noise_counts : sample_counts,
             .pixel_stats = noise_on ? slot->pixel_stats : sg::texture_2d(),
             .counters = noise_on ? slot->noise_counters : sg::buffer<u32>(),
             .instance_table = instance_table,
             .lights = light_buffer,
             .emitters = emitter_buffer,
             .hit_groups = resolved.hit_groups,
             // One material still compiling, or one that does not compile, degrades to gray
             // shading on its own meshes rather than costing the view its whole image.
             // Two stand-ins rather than one: a substitution has to keep the hit group's kind, or a quadric batch
             // traced by a triangle group reports no hits and disappears instead of shading flat.
             // The procedural one only where the view actually has a procedural instance -- acquiring it starts a
             // compile, and a triangle-only scene must not owe a drain for a group it can never select.
             .fallback = &resources.shaders.acquire_fallback(),
             .quadric_fallback = quadric_fallback,
             .bindless = &bindless,
             .decals = decal_buffers.decals,
             .decal_shapes = decal_buffers.shapes,
             .decal_atlas = decal_buffers.atlas,
             .stand_ins = &stand_ins},
            fc, *slot, scaled, budget_ms, target);

        // The count of pixels still tracing reads back as a sample floor's shortfall does; `land_sample_floors` takes it in.
        // One at a time: a frame waits for the last count to land before it asks for another, so none is ever dropped in flight.
        if (noise_on && outcome == sg::routine_outcome::executed && slot->sample_floors.empty())
        {
            auto const counts
                = ctx.transient.create_buffer<u32>(2, sg::buffer_usage::copy_dst | sg::buffer_usage::copy_src);
            cmd.copy.buffer_data_region<u32>(
                {.src = slot->noise_counters.raw(), .dst = counts.raw(), .count = 2, .src_offset = noise_counter_floor});
            slot->sample_floors.push_back(cmd.download.data_from_buffer(counts));
        }
        return outcome;
    };

    // A frame that traces nothing stands as executed: the mean already is the image.
    auto const traced = traces ? dispatch() : sg::routine_outcome::executed;

    // Only a frame that actually dispatched advances the accumulation: counting a declined one would make the blend
    // weight say more samples had landed than did, and the estimate would stop moving toward the answer.
    if (traced == sg::routine_outcome::declined)
        return sg::routine_outcome::declined;

    // The next frame's hash carries what this one stood in, so a substitution that changes restarts the image once.
    // One still compiling restarts it every frame instead, since it ends by itself and a mean of it must not read as converged.
    // So does an emitter whose triangles are still loading, which leaves the scene's light short of what it will be.
    if (traces)
        slot->stand_in_hash = stand_ins.hash;
    if (traces && (stand_ins.pending || resolved.emitters_pending))
    {
        slot->reset_hash = cc::combine_hash(hash, u64(1));
        ++slot->loading_frames;
    }

    // A scaled frame is no sample of the mean, which the next still frame restarts at full size.
    // A hybrid pack counts each sample it blended, as that many frames would have.
    if (traces && !scaled)
        for (auto i = 0; i < packed; ++i)
            count_traced_frame(*slot, l.settings);
    slot->traced_scaled = scaled;

    // Every frame that traces writes the whole primary depth, the wavefront's from what its tallies kept since the restart.
    if (traces && depth_slot != nullptr)
        depth_slot->accum_frame = 1;

    // Whether this frame wrote the hit distance on its own, which the denoise reads it only where it did: a traced frame's goes with the split.
    if (ds.hit_distance != nullptr)
        ds.hit_distance->accum_frame = traces && has_hit_distance ? 1u : 0u;

    // Every frame, the temporal member's or not: the first one it runs after a still period reprojects from here.
    if (has_temporal)
    {
        ds.motion->last_camera = fc.camera;
        ds.motion->has_last_camera = true;
    }

    // A frame drawn through a stand-in still compiling, or short of an emitter still loading, is an image of another scene:
    // the lamp it waits on may be the scene's only light, which leaves it black.
    // It declines, since it is not the image asked for.
    // The image presented before it stays up instead, as `resolve` already chose it, while it was made from this camera:
    // a compile can outlast a move, and an image that does not follow the camera is worse than a stand-in's that does.
    auto const incomplete = traces && (stand_ins.pending || resolved.emitters_pending);
    auto const held = has_guides && ds.denoised->accum_frame > 0 && ds.denoised->has_last_camera
                   && same_camera(ds.denoised->last_camera, fc.camera);
    if (incomplete && held)
    {
        if (!scaled && ds.normal->accum_frame < frame_count_cap)
            ++ds.normal->accum_frame;
        return sg::routine_outcome::declined;
    }

    if (has_guides)
    {
        if (traces && !scaled && ds.normal->accum_frame < frame_count_cap)
            ++ds.normal->accum_frame;
        // A denoiser still compiling declines the frame, as a tracer still compiling does: a capture that saved it
        // would hold the raw mean where the caller asked for a denoised image.
        // The trace itself landed, so the accumulation above stands.
        // Built from the same `shaders::tracer::camera_record` the motion guide was, so the matrices and the vectors describe one
        // camera; a first frame has no previous one and reads as a camera that did not move.
        auto const near_plane = f32(v.camera.projection.near_plane);
        auto const cameras = denoise_cameras{
            .current = matrices_of(fc.camera, near_plane),
            .previous
            = matrices_of(has_temporal && ds.motion->has_last_camera ? ds.motion->last_camera : fc.camera, near_plane),
            .jitter = jitter,
            .previous_jitter = previous_jitter,
        };

        if (scaled)
        {
            slot->last_jitter = jitter;
            auto const inputs = sr::reconstruct_inputs{
                .color = has_split ? frame_diffuse : frame_output,
                .specular = frame_specular,
                .guides = {.albedo = guide_albedo,
                           .specular_albedo = guide_specular_albedo,
                           .normal = guide_normal,
                           .roughness = guide_roughness,
                           .depth = guide_depth,
                           .motion = guide_motion,
                           .hit_distance = guide_hit_distance,
                           .jitter = jitter,
                           .previous_jitter = previous_jitter,
                           .view_to_clip = cameras.current.view_to_clip,
                           .previous_view_to_clip = cameras.previous.view_to_clip,
                           .world_to_view = cameras.current.world_to_view,
                           .previous_world_to_view = cameras.previous.world_to_view},
                .output = ds.denoised->texture,
            };
            auto const outcome
                = sr::reconstruct_routine::execute(cmd, inputs, ds.frame->scaled_denoise, upscale_settings);
            impl::gpu_mark(cmd, "reconstruct (scaled)");

            // The mean is not this frame's image, so a frame the upscale did not land presents the last one that did.
            if (outcome.is_denoised())
                mark_denoised(*ds.denoised, fc.camera);
            res.traces[trace_index] = ds.denoised->texture;
            if (outcome.status == sr::reconstruct_status::pending || incomplete)
                return sg::routine_outcome::declined;
            return sg::routine_outcome::executed;
        }

        // A frame that traced nothing keeps what the last one denoised, which `resolve` already presents.
        // The temporal member must not read the last frame's samples again as fresh ones, since its history would count them twice.
        // It denoises only while nothing has denoised the current mean, such as a member still compiling when the target was reached:
        // the samples it reads are then ones it has not seen.
        // Changed settings count as nothing having denoised it, since the denoised slot's declaration covers them and `resolve` restarts it.
        if (!traces && ds.denoised->accum_frame > 0)
            return sg::routine_outcome::executed;

        // Short of the target under `denoise_at_target_only`, a spatial member is skipped and the raw mean presented.
        // The denoised slot is marked as holding nothing, so the frame at the target denoises, and no stale image from an earlier mean shows.
        // That is the frame reaching it for a megakernel, whose samples count as it traces, and the first frame tracing nothing for a wavefront,
        // whose samples land a frame or more later; convergence reads that same count, so a capture never settles before the denoise.
        // A frame target is reached by the frame that traces it, under either integrator.
        if (def.denoise_at_target_only && traces && !slot->is_at_target() && !schedule.runs_temporal())
        {
            ds.denoised->accum_frame = 0;
            res.traces[trace_index] = slot->texture;
            return sg::routine_outcome::executed;
        }

        // Under a noise target each pixel holds its own count, which the denoiser judges it by.
        auto const& counts = noise && slot->noise_counts.raw() != nullptr ? slot->noise_counts : sample_counts;
        auto const denoised = _denoise(cmd, schedule, *slot, ds, cameras, counts, res.traces[trace_index]);
        if (ds.denoised->accum_frame > 0)
            mark_denoised(*ds.denoised, fc.camera);
        if (denoised == sr::reconstruct_status::pending)
            return sg::routine_outcome::declined;
    }
    return incomplete ? sg::routine_outcome::declined : sg::routine_outcome::executed;
}

f32 view_renderer::crossfade_weight(u32 accum_frame, u32 temporal_frames, u32 fade_frames)
{
    if (accum_frame <= temporal_frames)
        return 0.0f;
    if (fade_frames == 0)
        return 1.0f;

    // The first frame past the window is already one frame into the fade, so the curve leaves 0 immediately and the
    // hand-off has no frame that is neither one thing nor the other.
    auto const into = accum_frame - temporal_frames;
    return cc::min(1.0f, f32(into) / f32(fade_frames));
}

view_renderer::denoise_schedule view_renderer::_schedule_denoise(sg::context const& ctx,
                                                                 render_settings const& settings,
                                                                 denoise_slots const& ds,
                                                                 u32 accum_frame,
                                                                 f32 display_scale)
{
    // Temporal while the mean is young, on this frame's own samples: a young mean is barely less noisy than one frame,
    // and a temporal member's history is what makes up the difference.
    // Once the mean has more frames than that, a spatial member on the mean takes over, which keeps the converged image
    // unbiased.
    // `fresh_samples` says which of the two a call carries, and it lives in the settings so that resolving a method and
    // running it cannot disagree — so each member takes its own settings value rather than a flag per call.
    auto const with_fresh_samples = [&settings, display_scale](bool fresh)
    {
        auto copy = settings.reconstruct;
        copy.fresh_samples = fresh;
        // A member judging noise by how bright a pixel ends up on screen is told the display's scale, or it denoises an
        // image many stops brighter or darker than the one shown.
        copy.exposure = settings.reconstruct.exposure * display_scale;
        // These two run at the view's own size, without a jitter, so an upscaler would run 1:1 on an image it cannot
        // reconstruct from; only a frame traced at the input extent upscales, and `trace` asks for that itself.
        copy.upscaler = sr::upscale_method::none;
        return copy;
    };
    auto schedule = denoise_schedule{
        .temporal_settings = with_fresh_samples(true),
        .spatial_settings = with_fresh_samples(false),
    };

    // A NAMED temporal member names the temporal phase and nothing else.
    // Carrying it into the spatial phase asks a temporal member to run on the converging mean, with no motion and no
    // split — which the front refuses, so the layer presents the raw mean from the hand-off onward.
    // `automatic` is what the spatial phase wanted in the first place: it walks the spatial members.
    if (settings.reconstruct.denoiser != sr::denoise_method::automatic && sr::is_temporal(settings.reconstruct.denoiser))
        schedule.spatial_settings.denoiser = sr::denoise_method::automatic;

    auto const may_run_temporally
        = ds.frame != nullptr && ds.motion != nullptr
       && sr::is_temporal(sr::resolve_denoise_method(ctx, schedule.temporal_settings, traceable_guides()));

    // 0 while the temporal member owns the frame, 1 once the spatial one does, and the fade in between — so the two
    // members are never both idle.
    // A layer with no crossfade slot has nowhere to run both at once, so it hands over in one frame whatever it asked for.
    // OIDN hands over in one frame too, since a crossfade would run it on every frame of the fade.
    auto const oidn = settings.still_denoise == still_denoise::oidn;
    auto const fade_frames = ds.crossfade != nullptr && !oidn ? settings.temporal_denoise_fade_frames : 0;
    if (may_run_temporally)
        schedule.blend = crossfade_weight(accum_frame, settings.temporal_denoise_frames, fade_frames);

    if (oidn)
    {
        schedule.spatial_settings.denoiser = sr::denoise_method::oidn;
        schedule.spatial_on_doubling = true;
    }
    // The temporal member never hands over; past where it would have, it is fed the mean, which carries on converging.
    if (settings.still_denoise == still_denoise::temporal_on_mean && may_run_temporally)
    {
        schedule.temporal_on_mean = accum_frame > settings.temporal_denoise_frames;
        schedule.blend = 0.0f;
    }
    return schedule;
}

sr::reconstruct_status view_renderer::_denoise(sg::command_list& cmd,
                                               denoise_schedule const& schedule,
                                               impl::temporal_slot const& accumulator,
                                               denoise_slots const& ds,
                                               denoise_cameras const& cameras,
                                               sg::texture_2d const& sample_counts,
                                               sg::texture_2d& presented)
{
    auto& denoised = *ds.denoised;
    auto const guides = sr::reconstruct_guides{
        .albedo = ds.albedo->texture,
        .specular_albedo = ds.specular_albedo->texture,
        .normal = ds.normal->texture,
        .roughness = ds.roughness->texture,
        .depth = ds.depth->texture,

        // The per-frame offset every pixel shares, which a temporal member reconstructs sub-pixel detail from: a hybrid frame's.
        // A traced frame's is zero, which is not the same as no jitter: each of its primary rays draws its own within the
        // pixel, so the samples spread over it and their mean is its center, which is what a zero says.
        .jitter = cameras.jitter,
        .previous_jitter = cameras.previous_jitter,
        .view_to_clip = cameras.current.view_to_clip,
        .previous_view_to_clip = cameras.previous.view_to_clip,
        .world_to_view = cameras.current.world_to_view,
        .previous_world_to_view = cameras.previous.world_to_view,
    };

    auto const temporal = schedule.runs_temporal();
    auto const spatial = schedule.runs_spatial();

    // Mid-fade both members run, so the frame carries two denoises.
    // That is the whole price of the fade, and it is why the window is counted in frames rather than seconds: it is
    // bounded by the accumulation, which a still view leaves behind within a second of settling.
    auto outcome = sr::reconstruct_outcome();
    if (temporal)
    {
        auto temporal_guides = guides;
        temporal_guides.motion = ds.motion->texture;

        // A split-signal member reads the two lobes as its colour pair; everything else reads their sum, which is
        // what `frame` already holds — or, fed the mean, what the accumulator holds, which has no lobes apart.
        auto const split = !schedule.temporal_on_mean && ds.frame_diffuse != nullptr && ds.frame_specular != nullptr
                        && ds.hit_distance != nullptr;
        if (split)
            temporal_guides.hit_distance = ds.hit_distance->texture;
        else if (!schedule.temporal_on_mean && ds.hit_distance != nullptr && ds.hit_distance->accum_frame != 0)
            temporal_guides.hit_distance = ds.hit_distance->texture;

        auto const inputs = sr::reconstruct_inputs{
            .color = schedule.temporal_on_mean ? accumulator.texture
                   : split                     ? ds.frame_diffuse->texture
                                               : ds.frame->texture,
            .specular = split ? ds.frame_specular->texture : sg::texture_2d(),
            .guides = temporal_guides,
            .output = denoised.texture,
        };
        // Its own history, not the spatial member's: each would otherwise throw the other's away on every switch, and
        // the temporal one must survive a still period to be worth anything when the camera moves again.
        outcome = sr::reconstruct_routine::execute(cmd, inputs, ds.frame->denoise, schedule.temporal_settings);
    }

    if (spatial)
    {
        // Spatial, on the mean: the denoiser backs off as the sample count grows, so the converged image stays close to
        // itself.
        // Into the crossfade slot while the temporal image still holds `denoised`, and straight into `denoised` once it
        // does not — so the fade costs a texture and the steady state costs nothing.
        auto const& target = temporal ? ds.crossfade->texture : denoised.texture;
        auto const inputs = sr::reconstruct_inputs{
            .color = accumulator.texture,
            .guides = guides,
            .output = target,
            // Samples rather than frames, since a wavefront frame holds no fixed number; its pixels' own counts are `sample_counts`.
            .sample_count = accumulator.samples,
            .sample_counts = sample_counts,
        };

        // On doubling, the denoised slot's `samples` holds the mean's count as of the run that wrote it.
        // It runs again once the mean has twice that, or has restarted below it, or when the slot holds no image at all.
        auto const ran_at = denoised.samples;
        auto const due = !schedule.spatial_on_doubling || denoised.accum_frame == 0 || ran_at == 0
                      || accumulator.samples < ran_at || accumulator.samples >= 2 * ran_at;
        auto spatial_outcome = sr::reconstruct_outcome{.status = sr::reconstruct_status::denoised,
                                                       .denoiser = schedule.spatial_settings.denoiser};
        if (due)
        {
            spatial_outcome = sr::reconstruct_routine::execute(cmd, inputs, denoised.denoise, schedule.spatial_settings);
            if (schedule.spatial_on_doubling && spatial_outcome.is_denoised())
                denoised.samples = cc::max(accumulator.samples, 1u);
        }

        // Mid-fade the frame is only as good as its worse half: a spatial member that declined leaves the crossfade
        // slot holding an older image, and mixing that in would be a visible jump backwards.
        if (!temporal || !spatial_outcome.is_denoised())
            outcome = spatial_outcome;
        else if (!sr::mix_routine::execute(cmd, denoised.texture, ds.crossfade->texture, schedule.blend))
            outcome.status = sr::reconstruct_status::pending; // the mix is still compiling, so the fade cannot be applied
    }

    // Presented only when this frame produced it.
    // A denoiser still compiling, or one this machine cannot run, leaves the raw mean on screen rather than a denoised
    // image of an older one.
    // `accum_frame` on the denoised slot only means "holds a current image", which is what resolve() presents on a
    // throttled frame.
    if (outcome.is_denoised())
    {
        denoised.accum_frame = 1;
        presented = denoised.texture;
    }
    else
    {
        denoised.accum_frame = 0;
        presented = accumulator.texture;
    }
    return outcome.status;
}

sg::texture_2d view_renderer::execute(sg::command_list& cmd,
                                      view_data const& v,
                                      gpu_resource_manager& resources,
                                      view_store& store)
{
    // One span per view, which is the unit a viewer with several panes is actually spending its frame on.
    CC_RECORD_SCOPE("sv.view.render");

    auto& ctx = cmd.context();

    auto const* const scene = primary_scene_3d(v);
    CC_ASSERT(scene != nullptr, "a traced view needs a scene_3d layer");

    // Held for the whole trace because the reload generation is read under it.
    auto self = try_acquire_exclusive(cmd);

    // The slot is resolved either way, so a view whose renderer is not up yet still has a texture to re-present
    // rather than the caller getting nothing back.
    auto const shader_generation = self.is_ready() ? self->_shader_generation : 0;

    // No plan here to size the view's temporal inputs, so this path resolves the ones it needs itself.
    // The layer index is the primary scene_3d's, which `primary_scene_3d` already found.
    auto const layer = u8(scene - v.layers.begin());
    auto const acc = ensure_temporal(ctx, store, v.id, temporal_id::accumulation(layer), v.resolution,
                                     sg::pixel_format::rgba32_float);
    auto& slot = *acc.slot;

    // Nothing here denoises, so there is no history to drop — but the request must not outlive the frame that made it.
    // Left standing it would be consumed by whichever plan-path frame reaches this view next, cutting on a camera
    // nobody remembers.
    store.get_or_create(v.id).camera_cut_pending = false;

    // Resolved against the device here, since this path ensures a raster layer's slots itself.
    auto const mode = resolve_render_mode(ctx, scene->settings.mode);
    auto const is_traced = mode != render_mode::raster;
    if (is_traced && !can_trace(ctx, mode, store.get_or_create(v.id), slot))
        return slot.texture;

    // resolve_scene() touches the layer's meshes and instances, keeping this frame's working set resident, and mints every
    // bindless index this trace reads.
    auto resolved = resolve_scene(cmd, *scene, resources, mode);

    auto const lights = traced_lights(*scene, resolved);
    link_emitters(resolved, lights);
    auto const decals = decals_of(*scene, resources.drawings.decal_atlas());
    auto fc = make_frame_constants(v, *scene, lights, v.resolution);
    fc.decal_count = u32(decals.decals.size());
    if (mode != render_mode::megakernel)
        fc.samples_per_pixel = 0;
    auto const bg = background_gpu::from(scene->background.seen_from(v.camera));
    auto const hash
        = cc::combine_hash(trace_hash(fc, bg, lights, decals, resolved, v.resolution, shader_generation),
                           cc::make_hash(u32(mode), u32(scene->settings.hybrid_lighting), slot.stand_in_hash));

    if (!self.is_ready())
        return slot.texture; // nothing traced this frame; the caller re-presents what the slot already holds

    if (acc.resized)
        store.set_payload_bytes(v.id, texture_bytes(v.resolution, sg::pixel_format::rgba32_float));

    // As on the plan path, and this path never scales.
    if (mode != render_mode::wavefront)
        slot.paths = {};
    slot.scaled_paths = {};

    if (!is_traced)
    {
        // No plan sized the G-buffer either, so this path ensures it beside the image.
        for (auto const& s : impl::raster_slots)
            (void)ensure_temporal(ctx, store, v.id, s.id(layer), v.resolution, s.format);
        (void)raster_layer(cmd, v, *scene, layer, v.resolution,
                           {.resolved = &resolved, .lights = &lights, .decals = &decals, .background = &bg, .hash = hash},
                           store.get_or_create(v.id), slot, resources);
        return slot.texture;
    }

    // A fresh target holds no history, and a different image must not be averaged into the old one.
    if (acc.resized || slot.reset_hash != hash)
        impl::restart_accumulation(slot);
    slot.reset_hash = hash;
    slot.seed_key = seed_key_of(fc, bg, lights, resolved, v.resolution);

    if (mode == render_mode::hybrid)
    {
        for (auto const& s : impl::raster_slots)
            (void)ensure_temporal(ctx, store, v.id, s.id(layer), v.resolution, s.format);
        (void)hybrid_layer(cmd, v, *scene, layer, v.resolution,
                           {.resolved = &resolved, .lights = &lights, .decals = &decals, .background = &bg, .hash = hash},
                           fc, effective_sample_target(0), store.get_or_create(v.id), slot, resources);
        return slot.texture;
    }

    // As `trace`: a mean whose pixels all hold the cap is the image, and a frame there traces nothing.
    slot.sample_target = effective_sample_target(0);
    if (!frame_samples(slot, false))
        return slot.texture;

    // The shader reads accum_frame == 0 as "overwrite", anything above it as "blend in place".
    fc.accum_frame = slot.accum_frame;
    fc.rng_seed = seed_of(slot);

    auto const frame = ctx.transient.create_buffer<shaders::tracer::frame_constants>(
        1, sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
    cmd.upload.pod_to_buffer(frame, fc);

    // The view's SH environment probe, packed into its GPU lane layout.
    // The miss reconstructs the radiance an escaped ray sees from it.
    auto const background
        = ctx.transient.create_buffer<background_gpu>(1, sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
    cmd.upload.pod_to_buffer(background, bg);

    auto const instance_table = upload_instances(cmd, resolved);
    auto const light_buffer = upload_lights(cmd, lights);
    auto const emitter_buffer = upload_emitters(cmd, resolved);
    auto const decal_buffers = upload_decals(cmd, decals, resources.drawings.decal_atlas());
    auto const bindless = resources.freeze();

    // Acquired as soon as the view holds a quadric batch, rather than when one is caught uncompiled: the trace
    // declines entirely while a quadric permutation is unready and this is not, so a late acquire blanks the view for
    // however long the stand-in takes to compile.
    // A triangle-only view still never asks for it.
    auto const* const quadric_fallback = resolved.has_quadrics ? &resources.shaders.acquire_quadric_fallback() : nullptr;

    // Called under our own guard; the leaf takes its own, which is a different routine and so nests no lock.
    auto stand_ins = stand_in_report{};
    auto const traced = trace_layer(
        cmd, scene->settings,
        {.frame = frame,
         .background = background,
         .instances = resolved.instances,
         .output = slot.texture,
         .instance_table = instance_table,
         .lights = light_buffer,
         .emitters = emitter_buffer,
         .hit_groups = resolved.hit_groups,
         // One material still compiling, or one that does not compile, degrades to gray
         // shading on its own meshes rather than costing the view its whole image.
         // Two stand-ins rather than one: a substitution has to keep the hit group's kind, or a quadric batch
         // traced by a triangle group reports no hits and disappears instead of shading flat.
         // The procedural one only where the view actually has a procedural instance -- acquiring it starts a
         // compile, and a triangle-only scene must not owe a drain for a group it can never select.
         .fallback = &resources.shaders.acquire_fallback(),
         .quadric_fallback = quadric_fallback,
         .bindless = &bindless,
         .decals = decal_buffers.decals,
         .decal_shapes = decal_buffers.shapes,
         .decal_atlas = decal_buffers.atlas,
         .stand_ins = &stand_ins},
        fc, slot, false, 0.0f, effective_sample_target(0));

    // The store's budget counts the path state beside the texture, and the trace may just have created or grown it.
    auto bytes = texture_bytes(v.resolution, sg::pixel_format::rgba32_float);
    if (slot.paths.has_value())
        bytes += slot.paths.value().size_in_bytes();
    if (slot.scaled_paths.has_value())
        bytes += slot.scaled_paths.value().size_in_bytes();
    store.set_payload_bytes(v.id, bytes);

    // As above: a declined trace recorded nothing, so it must not count as a sample, and a frame shaded by a stand-in
    // still compiling, or lit by emitters still loading, must not be averaged with the next.
    if (traced == sg::routine_outcome::executed)
        slot.stand_in_hash = stand_ins.hash;
    if (traced == sg::routine_outcome::executed && (stand_ins.pending || resolved.emitters_pending))
    {
        slot.reset_hash = cc::combine_hash(hash, u64(1));
        ++slot.loading_frames;
    }
    if (traced == sg::routine_outcome::executed)
        count_traced_frame(slot, scene->settings);
    return slot.texture;
}
} // namespace sv
