#include <clean-core/common/asserts.hh>
#include <clean-core/thread/async.hh>
#include <imgui/imgui.h>
#include <shaped-graphics/binding_group.hh>
#include <shaped-graphics/command_list.hh>
#include <shaped-graphics/command_list.raster.hh> // sg::rendering_scope — execute() reads its format + size
#include <shaped-graphics/context.hh>
#include <shaped-graphics/pipeline_layout.hh>
#include <shaped-graphics/raster_pipeline.hh>
#include <shaped-graphics/swapchain.hh>
#include <shaped-graphics/vertex_input.hh>
#include <shaped-rendering/imgui_context.hh> // render_imgui drives update_viewports
#include <shaped-rendering/imgui_routine.hh>
#include <shaped-rendering/impl/imgui_draw_math.hh>
#include <sr_shaders.hh>

#include <cstddef> // offsetof

// ImDrawVert is {ImVec2 pos; ImVec2 uv; ImU32 col;} — 20 bytes, matching imgui.hlsl's vs_input.
// Kept in the .cc: this is the routine's private wiring, not a layout to impose on a consumer that might reasonably want a different one.
// rgba8_unorm is what decodes the packed ImU32 to [0,1] with no transfer function applied, which is exactly right for imgui's already-sRGB-encoded colors.
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

static_assert(sizeof(ImDrawIdx) == 2, "imgui_routine binds a u16 index buffer");

namespace sr
{
namespace
{
/// What a secondary viewport's RendererUserData points at: the swapchain presenting that OS window.
/// Created lazily on the viewport's first frame and released from Renderer_DestroyWindow.
struct viewport_swapchain
{
    sg::swapchain_handle chain;
};

/// The format every viewport swapchain is created with.
/// Not the main swapchain's — the routine never learns that one — but it must satisfy the same rule execute() states:
/// a non-sRGB format, because imgui's colors are already sRGB-encoded.
constexpr auto viewport_format = sg::pixel_format::bgra8_unorm;

/// Frees a viewport's swapchain when imgui closes it.
/// Registered instead of the full renderer-callback set:
/// sr drives the drawing itself in render_viewports, but imgui is the only thing that knows when a viewport dies, so this one hook is worth taking.
void install_renderer_callbacks()
{
    auto& platform_io = ImGui::GetPlatformIO();
    if (platform_io.Renderer_DestroyWindow != nullptr)
        return;

    platform_io.Renderer_DestroyWindow = [](ImGuiViewport* viewport)
    {
        if (auto* const owned = static_cast<viewport_swapchain*>(viewport->RendererUserData))
            IM_DELETE(owned);
        viewport->RendererUserData = nullptr;
    };
}

/// The swapchain presenting `viewport`, created on first use, or null when one could not be made.
sg::swapchain* swapchain_for(sg::context& ctx, ImGuiViewport* viewport)
{
    if (auto* const existing = static_cast<viewport_swapchain*>(viewport->RendererUserData))
        return existing->chain.get();

    // The platform side created the window hidden, so it already has a native handle to present against.
    if (viewport->PlatformHandleRaw == nullptr)
        return nullptr;

    // Fallible rather than throwing, for the same reason pipeline_for is: this runs inside the caller's frame, and a viewport that cannot get a swapchain should simply not draw.
    auto created = ctx.try_create_swapchain({.native_window_handle = viewport->PlatformHandleRaw, //
                                             .format = viewport_format});
    if (!created.has_value())
        return nullptr;

    auto* const owned = IM_NEW(viewport_swapchain)();
    owned->chain = cc::move(created.value());
    viewport->RendererUserData = owned;
    return owned->chain.get();
}
} // namespace

void imgui_routine::render_viewports(sg::context& ctx)
{
    if ((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) == 0)
        return;

    install_renderer_callbacks();

    // Index 0 is the main viewport, whose target, submit and present the caller owns —
    // it is rendered by the caller's own execute() call, and presenting it twice would be a second present on the same frame.
    auto& platform_io = ImGui::GetPlatformIO();
    for (auto i = 1; i < platform_io.Viewports.Size; ++i)
    {
        auto* const viewport = platform_io.Viewports[i];
        if ((viewport->Flags & ImGuiViewportFlags_IsMinimized) != 0)
            continue; // no drawable area, exactly as for a minimized main window

        auto* const chain = swapchain_for(ctx, viewport);
        if (chain == nullptr)
            continue;

        // acquire_backbuffer resizes the chain to the window's current client size, so a viewport the user is dragging the edge of needs nothing further from us.
        auto rt = chain->acquire_backbuffer();
        auto cmd = ctx.create_command_list();
        {
            // A viewport window shows nothing but imgui, so it is cleared unless imgui says it owns the clear itself (a viewport merged into another's swapchain sets that).
            auto const target = (viewport->Flags & ImGuiViewportFlags_NoRendererClear) != 0
                                  ? rt.preserved()
                                  : rt.cleared(tg::vec4f(0.0f, 0.0f, 0.0f, 1.0f));
            auto pass = cmd->raster.render_to({.color_targets = {target}});
            execute(pass, viewport->DrawData);
        }
        ctx.submit_command_list_and_present(*chain, cc::move(cmd));
    }
}

void imgui_routine::init_declare(sg::context& ctx)
{
    auto vs = sr::shaders::imgui.vertex.main_vs->acquire(ctx);
    auto ps = sr::shaders::imgui.fragment.main_ps->acquire(ctx);

    // No async pool is guaranteed here, so drive the compiles inline.
    (void)cc::try_async_blocking_get_singlethreaded(vs);
    (void)cc::try_async_blocking_get_singlethreaded(ps);

    auto const* const compiled_vs = vs->try_value();
    auto const* const compiled_ps = ps->try_value();

    _state.lock(
        [&](state& s)
        {
            // A reload rebuilds the layouts below, so every pipeline built against the old ones is now stale.
            // Dropping them here is what makes hot-reloading imgui.hlsl actually work rather than silently binding a pipeline whose root signature no longer matches.
            // The texture registry deliberately survives — the atlas has nothing to do with our shaders.
            s.pipelines.clear();
            s.pipeline_layout = nullptr;

            if (compiled_vs == nullptr || compiled_ps == nullptr)
                return; // a broken edit, or a context accepting no format we can produce — execute no-ops

            s.vertex_shader = *compiled_vs;
            s.fragment_shader = *compiled_ps;

            // Group 0 is built from the *fragment* bindings alone.
            // That is what keeps the vertex stage's b0 out of it — inline constants must be excluded from every group layout (see pipeline_layout.hh).
            // gSampler is name-matched here as a static sampler, so it is baked into the layout and costs no per-group descriptor;
            // clamp-to-edge stops the atlas bleeding across glyph edges.
            s.group_layout = ctx.cached.acquire_binding_group_layout(
                compiled_ps->bindings,
                {sg::named_sampler{.name = "gSampler",
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

            s.pipeline_layout = ctx.cached.acquire_pipeline_layout(
                {.groups = {s.group_layout}, .inline_constants = *constants_binding});
        });
}

sg::raster_pipeline const* imgui_routine::pipeline_for(state& s, sg::context& ctx, sg::pixel_format format)
{
    CC_ASSERT(!sg::is_srgb_format(format), "imgui colors are already sRGB-encoded; bind a non-srgb view of the target "
                                           "instead");

    for (auto const& e : s.pipelines)
        if (e.format == format)
            return e.pipeline.get();

    // Blocking build — see the TODO on state::pipelines.
    // imgui emits both windings so culling is off, and it is drawn in list order so there is no depth test.
    // Alpha blending is imgui's standard straight-alpha equation;
    // the alpha channel uses one/inv-src-alpha so compositing onto a transparent target accumulates coverage correctly rather than saturating.
    //
    // Fallible rather than throwing on purpose: execute() runs inside the caller's rendering scope, and an exception unwinding out of there would leave their command list unsubmitted.
    auto pipeline = ctx.uncached.try_create_raster_pipeline(
        {.layout = s.pipeline_layout,
         .vertex_shader = s.vertex_shader,
         .fragment_shader = s.fragment_shader,
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

    s.pipelines.push_back({.format = format, .pipeline = cc::move(pipeline).value()});
    return s.pipelines.back().pipeline.get();
}

imgui_routine::geometry imgui_routine::upload_geometry(sg::command_list& cmd, ImDrawData* draw_data)
{
    auto& ctx = cmd.context();

    auto const geo
        = geometry{.vertices = ctx.transient.create_buffer<ImDrawVert>(
                       isize(draw_data->TotalVtxCount), sg::buffer_usage::vertex_buffer | sg::buffer_usage::copy_dst),
                   .indices = ctx.transient.create_buffer<u16>(
                       isize(draw_data->TotalIdxCount), sg::buffer_usage::index_buffer | sg::buffer_usage::copy_dst)};

    // imgui keeps one vertex/index buffer per draw list; we concatenate them into one pair, and the draw loop offsets each list's commands accordingly.
    auto vertex_offset = isize(0);
    auto index_offset = isize(0);
    for (auto const* const list : draw_data->CmdLists)
    {
        cmd.upload.data_to_buffer(geo.vertices, cc::span<ImDrawVert const>(list->VtxBuffer.Data, list->VtxBuffer.Size),
                                  vertex_offset);
        cmd.upload.data_to_buffer(geo.indices, cc::span<u16 const>(list->IdxBuffer.Data, list->IdxBuffer.Size),
                                  index_offset);
        vertex_offset += isize(list->VtxBuffer.Size);
        index_offset += isize(list->IdxBuffer.Size);
    }

    return geo;
}

void imgui_routine::execute(sg::rendering_scope& scope, ImDrawData* draw_data)
{
    CC_ASSERT(draw_data != nullptr, "draw data must not be null — call ImGui::Render() first");

    auto& cmd = scope.command_list();
    CC_ASSERT(!scope.color_formats().empty(), "imgui must be drawn into a scope with a color target");
    auto const target_format = scope.color_formats()[0];
    auto const target_size = scope.render_target_size();

    auto& self = acquire(cmd);
    auto& ctx = cmd.context();

    self._state.lock(
        [&](state& s)
        {
            // Textures first: a draw below may sample an atlas imgui only just grew. These go out on
            // ctx.upload's copy queue, and the barrier tracker makes this list wait on them at submit.
            s.textures.service_requests(ctx, draw_data);

            if (s.pipeline_layout == nullptr)
                return; // shaders did not compile; nothing to draw until the next reload
            if (draw_data->TotalVtxCount == 0 || draw_data->TotalIdxCount == 0)
                return;

            auto const* const pipeline = pipeline_for(s, ctx, target_format);
            if (pipeline == nullptr)
                return;

            auto const geo = upload_geometry(cmd, draw_data);

            scope.bind_pipeline(*pipeline);
            scope.bind_vertex_buffer(geo.vertices.as_vertex_buffer());
            scope.bind_index_buffer(geo.indices.as_index_buffer());
            scope.set_viewport(
                {.offset = tg::pos2f(0.0f, 0.0f), .size = tg::vec2f(float(target_size[0]), float(target_size[1]))});
            scope.set_inline_constants(
                impl::compute_ortho_constants(tg::pos2f(draw_data->DisplayPos.x, draw_data->DisplayPos.y),
                                              tg::vec2f(draw_data->DisplaySize.x, draw_data->DisplaySize.y)));

            auto const display_pos = tg::pos2f(draw_data->DisplayPos.x, draw_data->DisplayPos.y);
            auto const framebuffer_scale = tg::vec2f(draw_data->FramebufferScale.x, draw_data->FramebufferScale.y);

            // imgui's draw lists are concatenated into one vertex and one index buffer, so each list's commands are offset by everything before it.
            auto global_vertex_offset = 0;
            auto global_index_offset = isize(0);

            // The bound group must outlive every draw that uses it: bind_group records a pointer, and the draw is what dereferences it.
            // Holding the handle out here (rather than inside the rebind block) is what keeps it alive until it is replaced or the recording ends.
            auto bound_group = sg::binding_group_handle{};
            auto bound_texture = ImTextureID_Invalid;

            for (auto const* const list : draw_data->CmdLists)
            {
                for (auto const& dc : list->CmdBuffer)
                {
                    // TODO(sr): user callbacks are not dispatched.
                    // Supporting them also means supporting ImDrawCallback_ResetRenderState, which needs the bind block above factored out of this loop.
                    // No imgui core path emits one, so nothing is lost until a caller adds one.
                    if (dc.UserCallback != nullptr)
                        continue;

                    auto const scissor = impl::compute_scissor(
                        tg::aabb2f(tg::pos2f(dc.ClipRect.x, dc.ClipRect.y), tg::pos2f(dc.ClipRect.z, dc.ClipRect.w)),
                        display_pos, framebuffer_scale, target_size);
                    if (!scissor.has_value())
                        continue; // entirely outside the target

                    if (dc.GetTexID() != bound_texture)
                    {
                        auto const texture = s.textures.try_texture_of(dc.GetTexID());
                        if (texture.has_error())
                            continue; // imgui named a texture we never created; skip rather than bind garbage

                        // Transient: one descriptor allocation per texture switch, recycled with the epoch.
                        // With a single font atlas that is one group for the whole frame.
                        bound_group = ctx.transient.create_binding_group(
                            s.group_layout,
                            {sg::named_view{.name = "gTexture", .view = texture.value().as_readonly_view()}});
                        scope.bind_group(0, *bound_group);
                        bound_texture = dc.GetTexID();
                    }

                    scope.set_scissor(scissor.value());
                    scope.draw_indexed({.index_range = {.offset = global_index_offset + isize(dc.IdxOffset),
                                                        .size = isize(dc.ElemCount)},
                                        .vertex_offset = global_vertex_offset + int(dc.VtxOffset)});
                }

                global_vertex_offset += list->VtxBuffer.Size;
                global_index_offset += isize(list->IdxBuffer.Size);
            }
        });
}

void render_imgui(imgui_context& imgui, sg::context& ctx, sg::swapchain& main, tg::vec4f clear_color)
{
    auto rt = main.acquire_backbuffer();
    auto cmd = ctx.create_command_list();
    {
        auto pass = cmd->raster.render_to({.color_targets = {rt.cleared(clear_color)}});
        imgui_routine::execute(pass, ImGui::GetDrawData());
    }

    // The viewport windows are moved and drawn after the main window's content is recorded but before it is presented — see render_viewports for why that order matters.
    imgui.update_viewports();
    imgui_routine::render_viewports(ctx);

    ctx.submit_command_list_and_present(main, cc::move(cmd));
}
} // namespace sr
