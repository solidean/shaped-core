#include <clean-core/common/asserts.hh>
#include <clean-core/common/hash.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/span.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/rendering/pathtrace_routine.hh>
#include <shaped-viewer/rendering/view_renderer.hh>
#include <shaped-viewer/resources/resource_managers.hh>
#include <shaped-viewer/scene/light.hh>
#include <shaped-viewer/view/render_settings.hh>
#include <shaped-viewer/view/view_data.hh>
#include <shaped-viewer/view/viewer_definition.hh>

namespace sv
{
namespace
{
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

/// One view resolved to what the path tracer binds: the TLAS instances plus the first item's mesh/materials.
struct resolved_view
{
    cc::vector<sg::tlas_instance> instances;
    mesh_record const* mesh = nullptr;
    material_record const* materials = nullptr;
};

resolved_view resolve_scene(layer const& l, scene_resources& resources)
{
    // The bound Materials/Vertices come from the first mesh: with one item (this slice) that is exact.
    // Multiple meshes want per-instance indexing — a flagged seam.
    auto out = resolved_view{};
    auto instance_index = u32(0);

    for (auto const& item : l.items)
    {
        if (item.kind != scene_item_kind::triangle_mesh)
            continue;

        auto const* const mesh = resources.meshes.get_ptr(item.mesh);
        auto const* const mats = resources.materials.get_ptr(item.materials);
        CC_ASSERT(mesh != nullptr, "scene_item references an unknown mesh_id");
        CC_ASSERT(mats != nullptr, "scene_item references an unknown material_set_id");

        auto inst = sg::tlas_instance{.blas = mesh->blas, .instance_id = instance_index};
        pack_transform(inst, item.transform);
        out.instances.push_back(cc::move(inst));

        if (out.mesh == nullptr)
        {
            out.mesh = mesh;
            out.materials = mats;
        }
        ++instance_index;
    }

    CC_ASSERT(!out.instances.empty() && out.mesh != nullptr && out.materials != nullptr,
              "a view needs at least one triangle-mesh item to render");
    return out;
}

/// The area light the path tracer integrates: the view's first, or the fallback below so a light-less view is still lit.
/// The tracer samples one rect for now.
/// Further area lights are the multi-light seam.
area_light primary_light(layer const& l)
{
    // A 1.5 x 1.5 rect three units overhead facing down (cross(+x, +z) is -y), a key light for a scene near the origin.
    auto const fallback = area_light{.center = tg::pos3f(0, 3, 0),
                                     .half_extent_u = tg::vec3f(0.75f, 0, 0),
                                     .half_extent_v = tg::vec3f(0, 0, 0.75f),
                                     .emission = tg::vec3f(12.0f, 12.0f, 12.0f)};
    return l.area_lights.empty() ? fallback : l.area_lights.front();
}

pt_frame_constants_gpu make_pt_frame_constants_gpu(view_data const& v,
                                                   layer const& l,
                                                   area_light const& light,
                                                   mesh_record const& mesh,
                                                   tg::vec2i resolution)
{
    auto fc = pt_frame_constants_gpu{};
    // The trace binds this one mesh, so its geometry layout is what the closest-hit must read by.
    fc.mesh_is_indexed = mesh.is_indexed;
    // The projection carries the aspect ratio.
    // It comes from the resolution the frame settled on rather than the view's own field, since a layout-following
    // view is sized by the rect it landed in.
    auto cam = v.camera;
    cam.projection.aspect_ratio = f64(resolution[0]) / f64(resolution[1] > 0 ? resolution[1] : 1);
    fc.camera = camera_gpu::from(cam);

    fc.light = area_light_gpu::from(light);

    fc.samples_per_pixel = l.settings.samples_per_pixel;
    fc.max_bounces = l.settings.max_bounces;

    // `seed` and `accum_frame` are stamped by execute from the view's persistent record, once it knows whether this frame restarts.
    return fc;
}

/// Everything this trace uploads, folded into one value — the signal that decides whether the accumulated image is still valid.
///
/// It hashes the *uploaded* bytes rather than the view's fields, so it cannot drift away from what the shader actually reads.
///
/// Where the view lands is structurally absent — a view_data carries no position — so relayout cannot discard a converged image.
/// Keeping that discipline is load-bearing for interaction, not just tidiness: a leaf's fit mode, sampler, zoom and
/// post-process parameters never reach an upload, which is exactly why dragging a wipe slider cannot restart a trace.
///
/// `resolution` is the one deliberate exception to "uploaded bytes only".
/// It reaches the upload only through the camera's aspect ratio, so 960x540 and 1920x1080 hash identically; without it
/// correctness would rest entirely on the resize check noticing, and a same-size texture handed back by a pool would blend two views.
[[nodiscard]] u64 trace_hash(pt_frame_constants_gpu fc, background_gpu const& bg, resolved_view const& r, tg::vec2i resolution)
{
    // The accumulation counter is the one field that legitimately differs every frame; hashing it would restart forever.
    fc.seed = 0;
    fc.accum_frame = 0;

    auto h = cc::make_hash_of_bytes(cc::span<pt_frame_constants_gpu const>(&fc, 1).as_bytes());
    h = cc::combine_hash(h, cc::make_hash_of_bytes(cc::span<background_gpu const>(&bg, 1).as_bytes()));
    h = cc::combine_hash(h, cc::make_hash(resolution[0], resolution[1]));

    // tlas_instance holds a handle and an optional, so its padding is not hashable — take the fields the build reads.
    for (auto const& inst : r.instances)
    {
        h = cc::combine_hash(h, cc::make_hash_of_bytes(cc::span<float const>(inst.transform, 12).as_bytes()));
        h = cc::combine_hash(h, cc::make_hash(inst.instance_id, inst.blas.get()));
    }

    // Geometry identity: a re-upload under the same mesh_id is different content, and these buffers are what the trace binds.
    return cc::combine_hash(h, cc::make_hash(r.mesh->vertices.raw().get(), r.materials->materials.raw().get()));
}

/// Frames a view may accumulate before it stops weighting new samples in.
/// The running mean is kept in half floats, so its weight has to stay well inside their precision.
constexpr u32 accumulation_frame_cap = 4096;

/// Bytes one accumulation target costs, for the cache's budget: rgba16_float is 8 bytes a pixel.
[[nodiscard]] isize accumulation_bytes(tg::vec2i size)
{
    return isize(size[0]) * isize(size[1]) * 8;
}
} // namespace

void view_renderer::init_declare(sg::context& ctx)
{
    // The renderer traces through the leaf routine, so warm its shader compiles when it is first initialized rather than stalling on the first frame.
    // It keeps no shader-derived state of its own here, so a reload must leave the accumulation cache alone.
    pathtrace_routine::prewarm(ctx);

    // A view's accumulation target is megabytes and its identity is a handful of bytes, so they expire on different schedules.
    _persistent.set_limits({.max_idle_frames_payload = sv::impl::view_payload_idle_frames,
                            .max_idle_frames_entry = sv::impl::view_idle_frames,
                            .max_payload_bytes = isize(256) << 20});
}

void view_renderer::begin_frame(sg::command_list& cmd)
{
    auto self = acquire_exclusive(cmd);
    self->_persistent.begin_frame(u64(cmd.context().current_epoch()),
                                  [](view_id, persistent_view_resources& r)
                                  {
                                      for (auto& slot : r.accumulation)
                                          if (slot.texture.raw() != nullptr)
                                              slot.texture.raw()->expire();
                                      if (r.composite.raw() != nullptr)
                                          r.composite.raw()->expire();
                                  });
}

u32 view_renderer::accumulated_frames(sg::command_list& cmd, view_id id)
{
    auto self = acquire_exclusive(cmd);
    auto const* const r = self->_persistent.peek(id); // a query must not keep a view alive
    return r == nullptr || r->accumulation.empty() ? 0 : r->accumulation.front().accum_frame;
}

plan_resources view_renderer::resolve(sg::command_list& cmd, render_plan const& plan)
{
    auto& ctx = cmd.context();
    auto self = acquire_exclusive(cmd);

    auto out = plan_resources{};
    out.targets.resize_to_defaulted(plan.targets.size());
    out.traces.resize_to_defaulted(plan.traces.size());

    // Touch first, and touch everything: a view that is throttled this frame is still sampled by its parent, so
    // letting the idle reclaim pass over it would release a texture that is about to be read.
    for (auto const id : plan.reachable)
        (void)self->_persistent.get_or_create(id);

    for (auto i = u32(0); i < plan.targets.size(); ++i)
    {
        auto const& t = plan.targets[i];
        if (t.is_output)
            continue; // the caller's texture; nothing of ours allocates or keeps it

        auto& rec = self->_persistent.get_or_create(t.id);
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
                 .usage = sg::texture_usage::readonly_texture | sg::texture_usage::render_target});
        }
        out.targets[i] = rec.composite;
    }

    for (auto i = u32(0); i < plan.traces.size(); ++i)
    {
        auto const& tr = plan.traces[i];
        auto& rec = self->_persistent.get_or_create(tr.id);

        if (isize(tr.layer) >= rec.accumulation.size())
            rec.accumulation.resize_to_defaulted(isize(tr.layer) + 1);

        auto& slot = rec.accumulation[tr.layer];
        auto const stale = slot.texture.raw() == nullptr || slot.texture.width() != tr.resolution[0]
                        || slot.texture.height() != tr.resolution[1];
        if (stale)
        {
            if (slot.texture.raw() != nullptr)
                slot.texture.raw()->expire();
            slot.texture = ctx.persistent.create_texture_2d(
                {.format = sg::pixel_format::rgba16_float, // UAV-written by the raygen, sampled by the layout routine
                 .width = tr.resolution[0],
                 .height = tr.resolution[1],
                 .usage = sg::texture_usage::readonly_texture | sg::texture_usage::readwrite_texture});

            // A fresh texture holds no history to blend into.
            slot.accum_frame = 0;
        }
        out.traces[i] = slot.texture;
    }

    // One budget entry per view, covering everything it holds — the cache reclaims a view whole or not at all.
    for (auto const id : plan.reachable)
    {
        auto const* const rec = self->_persistent.peek(id);
        if (rec == nullptr)
            continue;

        auto bytes = isize(0);
        if (rec->composite.raw() != nullptr)
            bytes += accumulation_bytes(tg::vec2i(rec->composite.width(), rec->composite.height()));
        for (auto const& slot : rec->accumulation)
            if (slot.texture.raw() != nullptr)
                bytes += accumulation_bytes(tg::vec2i(slot.texture.width(), slot.texture.height()));
        self->_persistent.set_payload_bytes(id, bytes);
    }

    return out;
}

void view_renderer::trace(sg::command_list& cmd,
                          viewer_definition const& def,
                          render_plan const& plan,
                          u32 trace_index,
                          plan_resources const& res,
                          scene_resources& resources)
{
    auto& ctx = cmd.context();

    auto const& tr = plan.traces[trace_index];
    CC_ASSERT(isize(u32(tr.view)) < def.views.size(), "a plan trace names a view the definition does not hold");
    auto const& v = def[tr.view];
    CC_ASSERT(isize(tr.layer) < v.layers.size(), "a plan trace names a layer the view does not hold");
    auto const& l = v.layers[tr.layer];

    auto const& output = res.traces[trace_index];
    if (output.raw() == nullptr)
        return; // resolve() refused it; nothing to trace into

    // Held for the whole trace because _persistent is touched under it; nothing rasters here, so no scope is open across the lock.
    auto self = acquire_exclusive(cmd);

    auto const resolved = resolve_scene(l, resources);

    // The aspect comes from the resolution the plan settled on, not the definition's own field: a layout-following
    // view's resolution is decided by the rect it landed in.
    auto fc = make_pt_frame_constants_gpu(v, l, primary_light(l), *resolved.mesh, tr.resolution);
    auto const bg = background_gpu::from(l.background);
    auto const hash = trace_hash(fc, bg, resolved, tr.resolution);

    auto& rec = self->_persistent.get_or_create(v.id);
    if (isize(tr.layer) >= rec.accumulation.size())
        return;
    auto& slot = rec.accumulation[tr.layer];

    // A different image must not be averaged into the old one.
    if (slot.content_hash != hash)
        slot.accum_frame = 0;
    slot.content_hash = hash;

    // The shader reads accum_frame == 0 as "overwrite", anything above it as "blend in place".
    // The seed rides one above it so each accumulated frame draws a different sample sequence, and is never 0.
    fc.accum_frame = slot.accum_frame;
    fc.seed = slot.accum_frame + 1;

    auto const frame = ctx.transient.create_buffer<pt_frame_constants_gpu>(
        1, sg::buffer_usage::uniform_buffer | sg::buffer_usage::copy_dst);
    cmd.upload.pod_to_buffer(frame, fc);

    auto const background
        = ctx.transient.create_buffer<background_gpu>(1, sg::buffer_usage::uniform_buffer | sg::buffer_usage::copy_dst);
    cmd.upload.pod_to_buffer(background, bg);

    pathtrace_routine::execute(cmd, {.frame = frame,
                                     .background = background,
                                     .instances = resolved.instances,
                                     .output = output,
                                     .materials = resolved.materials->materials,
                                     .vertices = resolved.mesh->vertices,
                                     .indices = resolved.mesh->indices});

    if (slot.accum_frame < accumulation_frame_cap)
        ++slot.accum_frame;
}

sg::texture_2d view_renderer::execute(sg::command_list& cmd, view_data const& v, scene_resources& resources)
{
    auto& ctx = cmd.context();

    auto const* const scene = primary_scene_3d(v);
    CC_ASSERT(scene != nullptr, "a traced view needs a scene_3d layer");

    // Held for the whole trace because _persistent is touched under it; nothing rasters here, so no scope is open across the lock.
    auto self = acquire_exclusive(cmd);

    // resolve_scene() touches the layer's meshes/materials (get_ptr), keeping this frame's working set resident.
    auto const resolved = resolve_scene(*scene, resources);

    auto fc = make_pt_frame_constants_gpu(v, *scene, primary_light(*scene), *resolved.mesh, v.resolution);
    auto const bg = background_gpu::from(scene->background);
    auto const hash = trace_hash(fc, bg, resolved, v.resolution);

    auto& rec = self->_persistent.get_or_create(v.id);
    if (rec.accumulation.empty())
        rec.accumulation.resize_to_defaulted(1);
    auto& slot = rec.accumulation.front();

    // The target outlives the frame so the trace has an image to blend into; only a size change forces a new one.
    auto const resized = slot.texture.raw() == nullptr || slot.texture.width() != v.resolution[0]
                      || slot.texture.height() != v.resolution[1];
    if (resized)
    {
        if (slot.texture.raw() != nullptr)
            slot.texture.raw()->expire();
        slot.texture = ctx.persistent.create_texture_2d(
            {.format = sg::pixel_format::rgba16_float, // UAV-written by the raygen, sampled by whatever composites it
             .width = v.resolution[0],
             .height = v.resolution[1],
             .usage = sg::texture_usage::readonly_texture | sg::texture_usage::readwrite_texture});
        self->_persistent.set_payload_bytes(v.id, accumulation_bytes(v.resolution));
    }

    // A fresh target holds no history, and a different image must not be averaged into the old one.
    if (resized || slot.content_hash != hash)
        slot.accum_frame = 0;
    slot.content_hash = hash;

    // The shader reads accum_frame == 0 as "overwrite", anything above it as "blend in place".
    // The seed rides one above it so each accumulated frame draws a different sample sequence, and is never 0.
    fc.accum_frame = slot.accum_frame;
    fc.seed = slot.accum_frame + 1;

    auto const frame = ctx.transient.create_buffer<pt_frame_constants_gpu>(
        1, sg::buffer_usage::uniform_buffer | sg::buffer_usage::copy_dst);
    cmd.upload.pod_to_buffer(frame, fc);

    // The view's SH environment probe, packed into its GPU lane layout.
    // The miss reconstructs the radiance an escaped ray sees from it.
    auto const background
        = ctx.transient.create_buffer<background_gpu>(1, sg::buffer_usage::uniform_buffer | sg::buffer_usage::copy_dst);
    cmd.upload.pod_to_buffer(background, bg);

    // Called under our own guard, and takes none of its own.
    pathtrace_routine::execute(cmd, {.frame = frame,
                                     .background = background,
                                     .instances = resolved.instances,
                                     .output = slot.texture,
                                     .materials = resolved.materials->materials,
                                     .vertices = resolved.mesh->vertices,
                                     .indices = resolved.mesh->indices});

    if (slot.accum_frame < accumulation_frame_cap)
        ++slot.accum_frame;
    return slot.texture;
}
} // namespace sv
