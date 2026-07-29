#include <clean-core/common/asserts.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/blit_routine.hh>
#include <shaped-viewer/light.hh>
#include <shaped-viewer/render_settings.hh>
#include <shaped-viewer/rendering/pathtrace_routine.hh>
#include <shaped-viewer/rendering/view_renderer.hh>
#include <shaped-viewer/resources/resource_managers.hh>
#include <shaped-viewer/view.hh>
#include <shaped-viewer/viewer_definition.hh>

namespace sv
{
namespace
{
/// Packs a column-major tg::mat4f into the TLAS instance's row-major 3x4 affine wire layout.
void pack_transform(sg::tlas_instance& inst, tg::mat4f const& m)
{
    for (auto r = 0; r < 3; ++r)
        for (auto c = 0; c < 4; ++c)
            inst.transform[r * 4 + c] = m[c, r];
}

/// One view resolved to what the path tracer binds: the TLAS instances plus the first item's mesh/materials.
struct resolved_view
{
    cc::vector<sg::tlas_instance> instances;
    mesh_record const* mesh = nullptr;
    material_record const* materials = nullptr;
};

resolved_view resolve(view const& v, scene_resources& resources)
{
    // The bound Materials/Vertices come from the first mesh: with one item (this slice) that is exact.
    // Multiple meshes want per-instance indexing — a flagged seam.
    auto out = resolved_view{};
    auto instance_index = u32(0);

    for (auto const& item : v.items)
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

/// The area light the path tracer integrates: the view's first, or a default so a light-less view is still lit.
/// The tracer samples one rect for now.
/// Further area lights are the multi-light seam.
area_light primary_light(view const& v)
{
    return v.area_lights.empty() ? area_light{} : v.area_lights.front();
}

pt_frame_constants_gpu make_pt_frame_constants_gpu(view const& v, area_light const& light, mesh_record const& mesh)
{
    auto fc = pt_frame_constants_gpu{};
    // The trace binds this one mesh, so its geometry layout is what the closest-hit must read by.
    fc.mesh_is_indexed = mesh.is_indexed;
    // The projection carries the aspect ratio.
    // Set it from this view's target size before baking the basis.
    auto cam = v.camera;
    cam.projection.aspect_ratio = f64(v.size[0]) / f64(v.size[1] > 0 ? v.size[1] : 1);
    fc.camera = camera_gpu::from(cam);

    fc.light = area_light_gpu::from(light);

    fc.samples_per_pixel = v.settings.samples_per_pixel;
    fc.max_bounces = v.settings.max_bounces;

    // A transient target is recreated each frame, so there is no image to blend into: every frame restarts.
    // Progressive accumulation is the view_id-keyed persistent-cache seam — until then, a fixed seed and frame 0.
    fc.seed = 1;
    fc.accum_frame = 0;
    return fc;
}
} // namespace

void view_renderer::init_declare(sg::context& ctx)
{
    // The renderer draws through the leaf routines, so warm their shader compiles when it is first initialized rather than stalling on the first frame.
    // It keeps no state of its own here — the persistent cache is scene resources, not shader-derived, so a reload must not clear it.
    pathtrace_routine::prewarm(ctx);
    sr::blit_routine::prewarm(ctx);
}

void view_renderer::execute(sg::command_list& cmd,
                            viewer_definition const& def,
                            scene_resources& resources,
                            sg::color_target const& output)
{
    auto& self = acquire(cmd);
    auto& ctx = cmd.context();

    // Reclaim stale / over-budget resources, then advance the managers to this frame's epoch.
    // resolve() below touches each view's meshes/materials (get_ptr), keeping this frame's working set resident.
    resources.begin_frame(ctx.current_epoch());

    if (def.views.empty())
        return;

    // One transient target per view; the first is what reaches `output` (multi-view compositing is the seam).
    auto targets = cc::vector<sg::texture_2d>();

    self._state.lock(
        [&](state& s)
        {
            for (auto const& v : def.views)
            {
                auto const resolved = resolve(v, resources);

                // Reserve the view's persistent slot — temporal accumulators land here (empty payload for now).
                (void)s.persistent[v.id];

                auto const frame = ctx.transient.create_buffer<pt_frame_constants_gpu>(
                    1, sg::buffer_usage::uniform_buffer | sg::buffer_usage::copy_dst);
                cmd.upload.pod_to_buffer(frame, make_pt_frame_constants_gpu(v, primary_light(v), *resolved.mesh));

                // The view's SH environment probe, packed into its GPU lane layout.
                // The miss reconstructs the radiance an escaped ray sees from it.
                auto const background = ctx.transient.create_buffer<background_gpu>(
                    1, sg::buffer_usage::uniform_buffer | sg::buffer_usage::copy_dst);
                cmd.upload.pod_to_buffer(background, background_gpu::from(v.background));

                auto const target = ctx.transient.create_texture_2d(
                    {.format = sg::pixel_format::rgba16_float, // UAV-written by the raygen, sampled by the blit
                     .width = v.size[0],
                     .height = v.size[1],
                     .usage = sg::texture_usage::readonly_texture | sg::texture_usage::readwrite_texture});

                pathtrace_routine::execute(cmd, {.frame = frame,
                                                 .background = background,
                                                 .instances = resolved.instances,
                                                 .output = target,
                                                 .materials = resolved.materials->materials,
                                                 .vertices = resolved.mesh->vertices,
                                                 .indices = resolved.mesh->indices});

                targets.push_back(target);
            }
        });

    // Bind the caller's output and blit the view across it.
    // The trace's UAV writes transition to a sampled read on this same command list, handled by sg's automatic barriers.
    auto scope = cmd.raster.render_to({.color_targets = {output}});
    sr::blit_routine::execute(scope, targets.front());
}
} // namespace sv
