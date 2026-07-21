#include <clean-core/common/asserts.hh>
#include <clean-core/thread/async.hh>
#include <imgui.h>
#include <shaped-graphics/binding_group.hh>
#include <shaped-graphics/command_list.hh>
#include <shaped-graphics/context.hh>
#include <shaped-graphics/pipeline_layout.hh>
#include <shaped-graphics/raster_pipeline.hh>
#include <shaped-graphics/vertex_input.hh>
#include <shaped-rendering/imgui_draw_routine.hh>
#include <shaped-rendering/impl/imgui_draw_math.hh>
#include <sr_shaders.hh>

#include <cstddef> // offsetof

// ImDrawVert is {ImVec2 pos; ImVec2 uv; ImU32 col;} — 20 bytes, matching imgui.hlsl's vs_input.
// Kept in the .cc: this is the routine's private wiring, not a layout to impose on a consumer that might
// reasonably want a different one. rgba8_unorm is what decodes the packed ImU32 to [0,1] with no transfer
// function applied, which is exactly right for imgui's already-sRGB-encoded colors.
template <>
struct sg::vertex_layout_of<ImDrawVert>
{
    static sg::vertex_type_layout get()
    {
        return {
            .stride = sizeof(ImDrawVert),
            .attributes = {
                {.semantic = "POSITION", .format = sg::vertex_attribute_format::vec2f, .offset = offsetof(ImDrawVert, pos)},
                {.semantic = "TEXCOORD", .format = sg::vertex_attribute_format::vec2f, .offset = offsetof(ImDrawVert, uv)},
                {.semantic = "COLOR",
                 .format = sg::vertex_attribute_format::rgba8_unorm,
                 .offset = offsetof(ImDrawVert, col)},
            }};
    }
};

static_assert(sizeof(ImDrawIdx) == 2, "imgui_draw_routine binds a u16 index buffer");

namespace sr
{
void imgui_draw_routine::init_declare(sg::context& ctx)
{
    // A reload rebuilds the layouts below, so every pipeline built against the old ones is now stale.
    // Dropping them here is what makes hot-reloading imgui.hlsl actually work rather than silently
    // binding a pipeline whose root signature no longer matches.
    _pipelines.clear();
    _pipeline_layout = nullptr;

    auto vs = sr::shaders::imgui.vertex.main_vs->acquire(ctx);
    auto ps = sr::shaders::imgui.fragment.main_ps->acquire(ctx);

    // No async pool is guaranteed here, so drive the compiles inline.
    (void)cc::try_async_blocking_get_singlethreaded(vs);
    (void)cc::try_async_blocking_get_singlethreaded(ps);

    auto const* const compiled_vs = vs->try_value();
    auto const* const compiled_ps = ps->try_value();
    if (compiled_vs == nullptr || compiled_ps == nullptr)
        return; // a broken edit, or a context accepting no format we can produce — execute() then no-ops

    _vertex_shader = *compiled_vs;
    _fragment_shader = *compiled_ps;

    // Group 0 is built from the *fragment* bindings alone. That is what keeps the vertex stage's b0 out of
    // it — inline constants must be excluded from every group layout (see pipeline_layout.hh). gSampler is
    // name-matched here as a static sampler, so it is baked into the layout and costs no per-group
    // descriptor; clamp-to-edge stops the atlas bleeding across glyph edges.
    _group_layout = ctx.cached.acquire_binding_group_layout(
        compiled_ps->bindings, {sg::named_sampler{.name = "gSampler",
                                                  .sampler = {.address_u = sg::sampler_address_mode::clamp_edge,
                                                              .address_v = sg::sampler_address_mode::clamp_edge,
                                                              .address_w = sg::sampler_address_mode::clamp_edge}}});

    // The vertex stage's only binding is the 16-byte ortho block, which rides as root constants.
    auto const* const constants_binding = [&]() -> sg::binding const*
    {
        for (auto const& b : compiled_vs->bindings)
            if (b.type == sg::binding_type::uniform_buffer)
                return &b;
        return nullptr;
    }();
    CC_ASSERT(constants_binding != nullptr, "imgui.hlsl must declare the imgui_constants cbuffer");

    _pipeline_layout
        = ctx.cached.acquire_pipeline_layout({.groups = {_group_layout}, .inline_constants = *constants_binding});
}

sg::raster_pipeline const* imgui_draw_routine::pipeline_for(sg::context& ctx, sg::pixel_format format) const
{
    CC_ASSERT(!sg::is_srgb_format(format), "imgui colors are already sRGB-encoded; bind a non-srgb view of the target "
                                           "instead");

    for (auto const& e : _pipelines)
        if (e.format == format)
            return e.pipeline.get();

    // Blocking build — see the TODO on _pipelines. imgui emits both windings so culling is off, and it is
    // drawn in list order so there is no depth test. Alpha blending is imgui's standard straight-alpha
    // equation; the alpha channel uses one/inv-src-alpha so compositing onto a transparent target
    // accumulates coverage correctly rather than saturating.
    //
    // Fallible rather than throwing on purpose: execute() runs inside the caller's rendering scope, and an
    // exception unwinding out of there would leave their command list unsubmitted.
    auto pipeline = ctx.uncached.try_create_raster_pipeline(
        {.layout = _pipeline_layout,
         .vertex_shader = _vertex_shader,
         .fragment_shader = _fragment_shader,
         .vertex_input = sg::vertex_input_layout::create<ImDrawVert>(),
         .topology = sg::primitive_topology::triangle_list,
         .rasterization = {.cull = sg::cull_mode::none},
         .color_targets
         = {{.format = format,
             .blend = sg::blend_state{
                 .color = {.source = sg::blend_factor::src_alpha, .target = sg::blend_factor::one_minus_src_alpha},
                 .alpha = {.source = sg::blend_factor::one, .target = sg::blend_factor::one_minus_src_alpha}}}}});
    if (!pipeline.has_value())
        return nullptr;

    _pipelines.push_back({.format = format, .pipeline = cc::move(pipeline).value()});
    return _pipelines.back().pipeline.get();
}

void imgui_draw_routine::execute(sg::command_list& cmd, ImDrawData* draw_data, params const& p)
{
    CC_ASSERT(draw_data != nullptr, "draw data must not be null");

    auto const& self = acquire(cmd);
    if (self._pipeline_layout == nullptr)
        return; // shaders did not compile; nothing to draw until the next reload

    // Nothing to draw is normal — a frame with every window collapsed still produces draw data.
    if (draw_data->TotalVtxCount == 0 || draw_data->TotalIdxCount == 0)
        return;

    auto const* const pipeline = self.pipeline_for(cmd.context(), p.target_format);
    if (pipeline == nullptr)
        return;

    cmd.raster.bind_pipeline(*pipeline);
    cmd.raster.bind_vertex_buffer(p.vertices.as_vertex_buffer());
    cmd.raster.bind_index_buffer(p.indices.as_index_buffer());
    cmd.raster.set_viewport(
        {.offset = tg::pos2f(0.0f, 0.0f), .size = tg::vec2f(float(p.target_size[0]), float(p.target_size[1]))});
    cmd.raster.set_inline_constants(
        impl::compute_ortho_constants(tg::pos2f(draw_data->DisplayPos.x, draw_data->DisplayPos.y),
                                      tg::vec2f(draw_data->DisplaySize.x, draw_data->DisplaySize.y)));

    auto const display_pos = tg::pos2f(draw_data->DisplayPos.x, draw_data->DisplayPos.y);
    auto const framebuffer_scale = tg::vec2f(draw_data->FramebufferScale.x, draw_data->FramebufferScale.y);

    // imgui's draw lists are concatenated into one vertex and one index buffer, so each list's commands are
    // offset by everything before it.
    auto global_vertex_offset = 0;
    auto global_index_offset = isize(0);

    // The bound group must outlive every draw that uses it: bind_group records a pointer, and the draw is
    // what dereferences it. Holding the handle here (rather than inside the rebind block) is what keeps it
    // alive until it is replaced or the recording ends.
    auto bound_group = sg::binding_group_handle{};
    auto bound_texture = ImTextureID_Invalid;

    for (auto const* const list : draw_data->CmdLists)
    {
        for (auto const& dc : list->CmdBuffer)
        {
            // TODO(sr): user callbacks are not dispatched. Supporting them also means supporting
            // ImDrawCallback_ResetRenderState, which needs the bind block above factored out of this
            // function. No imgui core path emits one, so nothing is lost until a caller adds their own.
            if (dc.UserCallback != nullptr)
                continue;

            auto const scissor = impl::compute_scissor(
                tg::aabb2f(tg::pos2f(dc.ClipRect.x, dc.ClipRect.y), tg::pos2f(dc.ClipRect.z, dc.ClipRect.w)),
                display_pos, framebuffer_scale, p.target_size);
            if (!scissor.has_value())
                continue; // entirely outside the target

            if (dc.GetTexID() != bound_texture)
            {
                auto const* const slot = p.textures.try_slot_of(dc.GetTexID());
                if (slot == nullptr)
                    continue; // imgui named a texture we never created; skip rather than bind garbage

                // Transient: one descriptor allocation per texture switch, recycled with the epoch. With a
                // single font atlas that is one group for the whole frame.
                bound_group = cmd.context().transient.create_binding_group(
                    self._group_layout, {sg::named_view{.name = "gTexture", .view = slot->texture.as_readonly_view()}});
                cmd.raster.bind_group(0, *bound_group);
                bound_texture = dc.GetTexID();
            }

            cmd.raster.set_scissor(scissor.value());
            cmd.raster.draw_indexed(
                {.index_range = {.offset = global_index_offset + isize(dc.IdxOffset), .size = isize(dc.ElemCount)},
                 .vertex_offset = global_vertex_offset + int(dc.VtxOffset)});
        }

        global_vertex_offset += list->VtxBuffer.Size;
        global_index_offset += isize(list->IdxBuffer.Size);
    }
}
} // namespace sr
