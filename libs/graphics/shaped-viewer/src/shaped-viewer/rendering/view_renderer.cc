#include <clean-core/common/asserts.hh>
#include <clean-core/common/hash.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/span.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/rendering/pathtrace_routine.hh>
#include <shaped-viewer/rendering/view_renderer.hh>
#include <shaped-viewer/resources/gpu_resource_manager.hh>
#include <shaped-viewer/resources/material_shader_cache.hh>
#include <shaped-viewer/resources/resource_managers.hh>
#include <shaped-viewer/scene/light.hh>
#include <shaped-viewer/view/render_settings.hh>
#include <shaped-viewer/view/view_data.hh>
#include <shaped-viewer/view/view_store.hh>
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

/// One view resolved to what the path tracer binds: a TLAS instance and an `instance_gpu` per item, over the set of
/// permutations those items shade with.
///
/// Two index relations, which together are the shape of the trace:
/// `instances[i]` and `records[i]` describe the same item, and `instances[i].hit_group_offset` indexes `hit_groups`.
struct resolved_view
{
    cc::vector<sg::tlas_instance> instances;
    cc::vector<instance_gpu> records;

    /// the parameter block each item shades with, parallel to `records` — its CONTENT identity, for the trace hash
    cc::vector<instance_id> parameter_blocks;
    cc::vector<material_permutation const*> hit_groups;

    /// the shader key of `hit_groups[i]`, kept for the trace hash rather than for the pipeline
    cc::vector<cc::hash128> permutations;
};

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

resolved_view resolve_scene(sg::command_list& cmd, layer const& l, gpu_resource_manager& resources)
{
    auto out = resolved_view{};

    for (auto const& item : l.items)
    {
        if (item.kind != scene_item_kind::triangle_mesh)
            continue;

        auto const* const mesh = resources.meshes.get_ptr(item.mesh);
        CC_ASSERT(mesh != nullptr, "scene_item references an unknown mesh_id");
        CC_ASSERT(resources.contains_instance(item.instance), "scene_item references an unknown instance_id");

        // The TLAS instance's own id is the row of the instance table this item occupies, which is what `InstanceID()` reads.
        auto inst = sg::tlas_instance{.blas = mesh->blas,
                                      .instance_id = u32(out.instances.size()),
                                      .hit_group_offset = hit_group_of(out, item.shader_key, resources)};
        pack_transform(inst, item.transform);
        out.instances.push_back(cc::move(inst));

        // This is where every index a hit reads is minted, so it must stay ahead of `freeze()` and on the list that traces.
        out.records.push_back(resources.describe_instance(cmd, item.mesh, item.instance));
        out.parameter_blocks.push_back(item.instance);
    }

    CC_ASSERT(!out.instances.empty(), "a view needs at least one triangle-mesh item to render");
    return out;
}

/// The instance table `r` describes, uploaded for this recording.
[[nodiscard]] sg::buffer<instance_gpu> upload_instances(sg::command_list& cmd, resolved_view const& r)
{
    auto const buffer = cmd.context().transient.create_buffer<instance_gpu>(
        r.records.size(), sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
    cmd.upload.data_to_buffer(buffer, r.records);
    return buffer;
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
                                                   tg::vec2i resolution)
{
    auto fc = pt_frame_constants_gpu{};
    // The projection carries the aspect ratio.
    // It comes from the resolution the frame settled on rather than the view's own field, since a layout-following
    // view is sized by the rect it landed in.
    auto cam = v.camera;
    cam.projection.aspect_ratio = f64(resolution[0]) / f64(resolution[1] > 0 ? resolution[1] : 1);
    fc.camera = camera_gpu::from(cam);

    fc.light = area_light_gpu::from(light);

    fc.samples_per_pixel = l.settings.samples_per_pixel;
    fc.max_bounces = l.settings.max_bounces;
    fc.debug_view = l.settings.debug_accumulation ? 1u : 0u;

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
///
/// `shader_generation` is the second: the shaders are not uploaded bytes but they decide what the numbers mean, so a
/// reload has to restart the estimator rather than average two different integrators together.
///
/// **The camera is deliberately excluded**, along with everything that describes the history rather than the image.
/// Moving the eye no longer makes the accumulated image wrong — the raygen reprojects it and rejects per pixel what
/// no longer matches — so hashing the camera would throw away exactly the convergence reprojection exists to keep.
/// What is left means "the scene changed", which is the only thing a whole-image restart is still the right answer to.
[[nodiscard]] u64 trace_hash(pt_frame_constants_gpu fc,
                             background_gpu const& bg,
                             resolved_view const& r,
                             tg::vec2i resolution,
                             u64 shader_generation)
{
    // Fields that legitimately differ every frame, or that describe the history rather than the image it holds.
    // Hashing any of them would restart the estimator forever.
    fc.seed = 0;
    fc.accum_frame = 0;
    fc.camera = {};
    fc.prev_camera = {};
    fc.has_history = false;
    fc.history_max_frames = 0;

    auto h = cc::make_hash_of_bytes(cc::span<pt_frame_constants_gpu const>(&fc, 1).as_bytes());
    h = cc::combine_hash(h, cc::make_hash_of_bytes(cc::span<background_gpu const>(&bg, 1).as_bytes()));
    h = cc::combine_hash(h, cc::make_hash(resolution[0], resolution[1], shader_generation));

    // tlas_instance holds a handle and an optional, so its padding is not hashable — take the fields the build reads.
    for (auto const& inst : r.instances)
    {
        h = cc::combine_hash(h, cc::make_hash_of_bytes(cc::span<float const>(inst.transform, 12).as_bytes()));
        h = cc::combine_hash(h, cc::make_hash(inst.instance_id, inst.hit_group_offset, inst.blas.get()));
    }

    // The instance table is the whole of what a hit reads, geometry and material parameters alike, so hashing the
    // records covers content identity for both: a re-upload under one id lands on a different bindless element.
    //
    // `param_buffer` is the one field taken out.
    // The block lives in a transient buffer rebuilt every epoch, so its index is new every frame and says nothing about the
    // image; leaving it in would restart the estimator forever.
    // The `instance_id` beside it is the content identity that field stood for, since one is minted per `parameter_key`.
    for (auto const& rec : r.records)
    {
        auto anonymized = rec;
        anonymized.param_buffer = 0;
        h = cc::combine_hash(h, cc::make_hash_of_bytes(cc::span<instance_gpu const>(&anonymized, 1).as_bytes()));
    }
    for (auto const block : r.parameter_blocks)
        h = cc::combine_hash(h, cc::make_hash(u32(block)));

    // Which shader reads those records is not in them, and a different permutation is a different image.
    for (auto const& key : r.permutations)
        h = cc::combine_hash(h, cc::make_hash(key.low, key.high));
    return h;
}

/// Frames a view may accumulate before it stops weighting new samples in.
/// The running mean is kept in half floats, so its weight has to stay well inside their precision.
constexpr u32 accumulation_frame_cap = 4096;

/// Bytes one accumulation target costs, for the cache's budget: rgba16_float is 8 bytes a pixel.
[[nodiscard]] isize accumulation_bytes(tg::vec2i size)
{
    return isize(size[0]) * isize(size[1]) * 8;
}

/// How far a reprojected pixel's sample count may be carried once the camera has moved.
///
/// The hybrid: a still camera reprojects every pixel onto itself, so leaving the count uncapped keeps the exact
/// running mean and lets a static view converge to ground truth exactly as it did before reprojection existed.
/// Once the eye moves, a carried sample is an approximation — it came from a slightly different direction — so the
/// cap bounds how long one can persist, trading a little noise for an image that stops smearing.
constexpr u32 moving_history_frames = 32;

/// Whether `a` and `b` describe the same eye, byte for byte.
///
/// A pure comparison of what was uploaded, so it cannot disagree with what the shader reprojects through.
/// Padding counts, which is safe here only because every `camera_gpu` is built by `camera_gpu::from` with its pad
/// lanes explicitly zeroed.
[[nodiscard]] bool same_camera(camera_gpu const& a, camera_gpu const& b)
{
    return cc::memcmp(&a, &b, sizeof(camera_gpu)) == 0;
}

/// Fills in everything a trace needs to reuse its history, and reports the camera forward for the next frame.
void apply_history(pt_frame_constants_gpu& fc, impl::view_state& rec, impl::temporal_slot const& slot)
{
    fc.has_history = slot.has_history && rec.has_last_traced_camera;
    fc.prev_camera = rec.last_traced_camera;

    // A still camera keeps the exact mean; a moved one ages its carried samples out.
    fc.history_max_frames
        = fc.has_history && same_camera(fc.camera, rec.last_traced_camera) ? u32(-1) : moving_history_frames;
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
        auto const desc = sg::texture_2d_description{
            .format = format,
            .width = resolution[0],
            .height = resolution[1],
            .usage = sg::texture_usage::readonly_texture | sg::texture_usage::readwrite_texture};

        if (slot.texture.raw() != nullptr)
            slot.texture.raw()->expire();
        if (slot.history.raw() != nullptr)
            slot.history.raw()->expire();

        slot.texture = ctx.persistent.create_texture_2d(desc);
        slot.history = ctx.persistent.create_texture_2d(desc);
        slot.has_history = false; // neither half holds a frame yet
    }

    return {.slot = &slot, .resized = resized};
}
} // namespace

void view_renderer::init_declare(sg::context& ctx)
{
    // The renderer traces through the leaf routine, so warm its shader compiles when it is first initialized rather than stalling on the first frame.
    pathtrace_routine::prewarm(ctx);

    // This runs again on every reload, which is exactly when an accumulated image stops being comparable to a fresh one.
    ++_shader_generation;
}

plan_resources view_renderer::resolve(sg::command_list& cmd, render_plan const& plan, view_store& store)
{
    auto& ctx = cmd.context();

    auto out = plan_resources{};
    out.targets.resize_to_defaulted(plan.targets.size());
    out.traces.resize_to_defaulted(plan.traces.size());

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
                 .usage = sg::texture_usage::readonly_texture | sg::texture_usage::render_target});
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
        // stamped later in the frame by `trace`. Comparing or overwriting it here would pit the two against each
        // other and restart the accumulation on every frame.
        if (e.resized || e.slot->declared_hash != t.reset_hash)
            e.slot->accum_frame = 0;
        e.slot->declared_hash = t.reset_hash;
    }

    // Rotate the pair on exactly the traces that record this frame.
    // A reprojecting read must not alias its own write, and a throttled trace must be left alone or the image its
    // parent re-presents would be the one from two frames ago.
    for (auto const& tr : plan.traces)
    {
        if (!tr.refresh)
            continue;

        auto& rec = store.get_or_create(tr.id);

        // Every slot this trace writes rotates together, on the *trace's* progress — which only the accumulator counts.
        // Asking each slot for its own `accum_frame` freezes the G-buffer at zero forever, since nothing increments
        // that one: its pair never rotates, the history half stays the texture nothing ever wrote, and every pixel
        // fails the disocclusion test against garbage geometry.
        auto const* const accumulator = rec.temporal.get_ptr(temporal_id::accumulation(tr.layer));
        auto const carries_history = accumulator != nullptr && accumulator->accum_frame > 0;

        for (auto const tid : {temporal_id::accumulation(tr.layer), temporal_id::gbuffer(tr.layer)})
        {
            auto* const slot = rec.temporal.get_ptr(tid);
            if (slot == nullptr)
                continue;

            slot->has_history = carries_history;
            if (carries_history)
            {
                auto const written = slot->texture;
                slot->texture = slot->history;
                slot->history = written;
            }
        }
    }

    for (auto i = u32(0); i < plan.traces.size(); ++i)
    {
        auto const& tr = plan.traces[i];
        auto const* const slot = store.get_or_create(tr.id).temporal.get_ptr(temporal_id::accumulation(tr.layer));

        // The loop above allocated it, so a miss means the plan named a trace it declared no accumulator for.
        CC_ASSERT(slot != nullptr, "a plan trace has no accumulator among its view's temporal inputs");
        out.traces[i] = slot->texture;
    }

    // One budget entry per view, covering everything it holds — the store reclaims a view's textures whole or not at all.
    for (auto const id : plan.reachable)
    {
        auto const* const rec = store.peek_ptr(id);
        if (rec == nullptr)
            continue;

        auto bytes = isize(0);
        if (rec->composite.raw() != nullptr)
            bytes += accumulation_bytes(tg::vec2i(rec->composite.width(), rec->composite.height()));
        for (auto const& [temporal_id, slot] : rec->temporal)
        {
            // Both halves of the pair, since both stay resident for as long as the view does.
            if (slot.texture.raw() != nullptr)
                bytes += accumulation_bytes(tg::vec2i(slot.texture.width(), slot.texture.height()));
            if (slot.history.raw() != nullptr)
                bytes += accumulation_bytes(tg::vec2i(slot.history.width(), slot.history.height()));
        }
        store.set_payload_bytes(id, bytes);
    }

    return out;
}

void view_renderer::trace(sg::command_list& cmd,
                          viewer_definition const& def,
                          render_plan const& plan,
                          u32 trace_index,
                          plan_resources const& res,
                          gpu_resource_manager& resources,
                          view_store& store)
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

    // Held for the whole trace because the reload generation is read under it; nothing rasters here, so no scope is open across the lock.
    auto self = acquire_exclusive(cmd);

    auto const resolved = resolve_scene(cmd, l, resources);

    // The aspect comes from the resolution the plan settled on, not the definition's own field: a layout-following
    // view's resolution is decided by the rect it landed in.
    auto fc = make_pt_frame_constants_gpu(v, l, primary_light(l), tr.resolution);
    auto const bg = background_gpu::from(l.background);
    auto const hash = trace_hash(fc, bg, resolved, tr.resolution, self->_shader_generation);

    auto& rec = store.get_or_create(v.id);
    auto* const slot = rec.temporal.get_ptr(temporal_id::accumulation(tr.layer));
    auto const* const gbuffer = rec.temporal.get_ptr(temporal_id::gbuffer(tr.layer));
    if (slot == nullptr || gbuffer == nullptr)
        return; // resolve() refused it; nothing to accumulate into

    // A different image must not be averaged into the old one.
    // The tracer publishes its own hash as this slot's reset rule: it covers the bytes actually uploaded, so it
    // cannot drift away from what the shader reads the way a hand-declared one could.
    if (slot->reset_hash != hash)
        slot->accum_frame = 0;
    slot->reset_hash = hash;

    // The shader reads accum_frame == 0 as "overwrite", anything above it as "blend in place".
    // The seed rides one above it so each accumulated frame draws a different sample sequence, and is never 0.
    fc.accum_frame = slot->accum_frame;
    fc.seed = slot->accum_frame + 1;
    apply_history(fc, rec, *slot);

    auto const frame = ctx.transient.create_buffer<pt_frame_constants_gpu>(
        1, sg::buffer_usage::uniform_buffer | sg::buffer_usage::copy_dst);
    cmd.upload.pod_to_buffer(frame, fc);

    auto const background
        = ctx.transient.create_buffer<background_gpu>(1, sg::buffer_usage::uniform_buffer | sg::buffer_usage::copy_dst);
    cmd.upload.pod_to_buffer(background, bg);

    auto const instance_table = upload_instances(cmd, resolved);

    // Held across the dispatch: the tables are what the closest-hit reaches every mesh and texture through, and
    // nothing may mint a descriptor the bound snapshot would not contain while it is being recorded against.
    auto const bindless = resources.freeze();

    pathtrace_routine::execute(cmd, {.frame = frame,
                                     .background = background,
                                     .instances = resolved.instances,
                                     .output = output,
                                     .gbuffer = gbuffer->texture,
                                     .history_color = slot->history,
                                     .history_gbuffer = gbuffer->history,
                                     .instance_table = instance_table,
                                     .hit_groups = resolved.hit_groups,
                                     .bindless = &bindless});

    // What the next frame reprojects through.
    rec.last_traced_camera = fc.camera;
    rec.has_last_traced_camera = true;

    if (slot->accum_frame < accumulation_frame_cap)
        ++slot->accum_frame;
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

    // Held for the whole trace because the reload generation is read under it; nothing rasters here, so no scope is open across the lock.
    auto self = acquire_exclusive(cmd);

    // resolve_scene() touches the layer's meshes and instances, keeping this frame's working set resident, and mints every
    // bindless index this trace reads.
    auto const resolved = resolve_scene(cmd, *scene, resources);

    auto fc = make_pt_frame_constants_gpu(v, *scene, primary_light(*scene), v.resolution);
    auto const bg = background_gpu::from(scene->background);
    auto const hash = trace_hash(fc, bg, resolved, v.resolution, self->_shader_generation);

    // No plan here to size the view's temporal inputs, so this path resolves the ones it needs itself.
    // The layer index is the primary scene_3d's, which `primary_scene_3d` already found.
    auto const layer = u8(scene - v.layers.begin());
    auto const acc = ensure_temporal(ctx, store, v.id, temporal_id::accumulation(layer), v.resolution,
                                     sg::pixel_format::rgba16_float);
    auto const gb
        = ensure_temporal(ctx, store, v.id, temporal_id::gbuffer(layer), v.resolution, sg::pixel_format::rgba16_float);
    auto& slot = *acc.slot;

    if (acc.resized || gb.resized)
        store.set_payload_bytes(v.id, 4 * accumulation_bytes(v.resolution)); // two slots, a pair each

    // A fresh target holds no history, and a different image must not be averaged into the old one.
    if (acc.resized || slot.reset_hash != hash)
        slot.accum_frame = 0;
    slot.reset_hash = hash;

    // No plan drove a rotation here either, so this path rotates its own pair — see resolve() for why it is
    // conditional on having recorded something.
    auto& rec = store.get_or_create(v.id);
    for (auto* const s : {acc.slot, gb.slot})
    {
        s->has_history = slot.accum_frame > 0;
        if (s->has_history)
        {
            auto const written = s->texture;
            s->texture = s->history;
            s->history = written;
        }
    }

    // The shader reads accum_frame == 0 as "overwrite", anything above it as "blend in place".
    // The seed rides one above it so each accumulated frame draws a different sample sequence, and is never 0.
    fc.accum_frame = slot.accum_frame;
    fc.seed = slot.accum_frame + 1;
    apply_history(fc, rec, slot);

    auto const frame = ctx.transient.create_buffer<pt_frame_constants_gpu>(
        1, sg::buffer_usage::uniform_buffer | sg::buffer_usage::copy_dst);
    cmd.upload.pod_to_buffer(frame, fc);

    // The view's SH environment probe, packed into its GPU lane layout.
    // The miss reconstructs the radiance an escaped ray sees from it.
    auto const background
        = ctx.transient.create_buffer<background_gpu>(1, sg::buffer_usage::uniform_buffer | sg::buffer_usage::copy_dst);
    cmd.upload.pod_to_buffer(background, bg);

    auto const instance_table = upload_instances(cmd, resolved);
    auto const bindless = resources.freeze();

    // Called under our own guard; the leaf takes its own, which is a different routine and so nests no lock.
    pathtrace_routine::execute(cmd, {.frame = frame,
                                     .background = background,
                                     .instances = resolved.instances,
                                     .output = slot.texture,
                                     .gbuffer = gb.slot->texture,
                                     .history_color = slot.history,
                                     .history_gbuffer = gb.slot->history,
                                     .instance_table = instance_table,
                                     .hit_groups = resolved.hit_groups,
                                     .bindless = &bindless});

    // What the next frame reprojects through.
    rec.last_traced_camera = fc.camera;
    rec.has_last_traced_camera = true;

    if (slot.accum_frame < accumulation_frame_cap)
        ++slot.accum_frame;
    return slot.texture;
}
} // namespace sv
