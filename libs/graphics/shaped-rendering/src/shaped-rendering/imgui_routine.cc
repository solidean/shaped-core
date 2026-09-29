#include <clean-core/common/asserts.hh>
#include <clean-core/error/result.hh>
#include <clean-core/fwd.hh> // offsetof
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <imgui/imgui.h>
#include <shaped-graphics/binding/binding_group.hh>
#include <shaped-graphics/binding/pipeline_layout.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/command_list/raster.hh> // sg::rendering_scope — execute() reads its format + size
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/present/swapchain.hh>
#include <shaped-graphics/raster/raster_pipeline.hh>
#include <shaped-graphics/raster/vertex_input.hh>
#include <shaped-rendering/imgui_context.hh> // render_imgui drives update_viewports
#include <shaped-rendering/imgui_routine.hh>
#include <shaped-rendering/impl/imgui_draw_math.hh>
#include <shaped-rendering/window.hh>
#include <sr_sgl_shaders.hh>

// imgui.sgl's constant block reaches C++ as a generated struct, so what the routine sends is checked against the shader.
// impl::imgui_ortho_constants stays, because the draw math is tested without the generated header.
static_assert(sizeof(sr::impl::imgui_ortho_constants) == sr::sgl_shaders::imgui_constants::block_size,
              "the ortho constants are not the size imgui.sgl's block states");
static_assert(offsetof(sr::sgl_shaders::imgui_constants, scale) == offsetof(sr::impl::imgui_ortho_constants, scale),
              "scale moved in imgui.sgl");
static_assert(offsetof(sr::sgl_shaders::imgui_constants, translate)
                  == offsetof(sr::impl::imgui_ortho_constants, translate),
              "translate moved in imgui.sgl");

// The routine binds imgui's own vertex buffer, so the pipeline reads ImDrawVert through imgui.sgl's `imgui_vertex`.
static_assert(sizeof(ImDrawVert) == sizeof(sr::sgl_shaders::imgui_vertex), "ImDrawVert is not imgui.sgl's imgui_vertex");
static_assert(offsetof(ImDrawVert, pos) == offsetof(sr::sgl_shaders::imgui_vertex, position),
              "imgui_vertex.position moved");
static_assert(offsetof(ImDrawVert, uv) == offsetof(sr::sgl_shaders::imgui_vertex, uv), "imgui_vertex.uv moved");
static_assert(offsetof(ImDrawVert, col) == offsetof(sr::sgl_shaders::imgui_vertex, color), "imgui_vertex.color moved");

static_assert(sizeof(ImDrawIdx) == 4,
              "imgui_routine binds a u32 index buffer — see extern/imgui/shaped/imgui/imgui_config.hh");

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
    // PlatformHandle rather than PlatformHandleRaw: the raw one is imgui's own win32 slot, and a swapchain needs the
    // display alongside the handle on the two X11 platforms.
    auto const* const win = static_cast<sr::window const*>(viewport->PlatformHandle);
    if (win == nullptr || !win->native_window().is_valid())
        return nullptr;

    // Fallible rather than throwing, for the same reason the pipeline build is: this runs inside the caller's frame, and a viewport that cannot get a swapchain should simply not draw.
    auto created = ctx.try_create_swapchain({.window = win->native_window(), //
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
    // it is rendered by the caller's own prepare() and execute(), and presenting it twice would be a second present on the same frame.
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
        auto const frame = prepare(*cmd, viewport->DrawData);
        {
            // A viewport window shows nothing but imgui, so it is cleared unless imgui says it owns the clear itself (a viewport merged into another's swapchain sets that).
            auto const target = (viewport->Flags & ImGuiViewportFlags_NoRendererClear) != 0
                                  ? rt.preserved()
                                  : rt.cleared(tg::vec4f(0.0f, 0.0f, 0.0f, 1.0f));
            auto pass = cmd->raster.render_to({.color_targets = {target}});
            // Declined means imgui's pipeline is not up yet; the viewport shows its clear this frame.
            (void)execute(pass, frame);
        }
        ctx.submit_command_list_and_present(*chain, cc::move(cmd));
    }
}

cc::shared_async<cc::unit> imgui_routine::init(sg::routine_init_scope scope)
{
    auto& ctx = scope.context();

    // The atlas lives on its own routine and deliberately survives a reload — it has nothing to do with our shaders.
    // Minted before the first await, so a routine that turns out not to be able to draw still has its token and
    // services textures.
    _textures = depend_on<impl::imgui_texture_routine>(ctx);

    auto const vs = sgl_shaders::imgui.main_vs->acquire(ctx);
    auto const ps = sgl_shaders::imgui.main_ps->acquire(ctx);

    co_await cc::async_settled(vs);
    co_await cc::async_settled(ps);

    auto const* const compiled_vs = vs->try_value();
    auto const* const compiled_ps = ps->try_value();

    _group_layout = nullptr;
    _pipeline = {};
    if (compiled_vs == nullptr || compiled_ps == nullptr)
    {
        fail_init(); // not pending: this will not come good until a reload, and a caller should be able to tell
        co_return;
    }

    // The group is what the shader declared, so the atlas sampler's state lives in imgui.sgl alone.
    _group_layout = ctx.cached.acquire_binding_group_layout<sgl_shaders::imgui_atlas>();

    // The vertex stage lists the inline constants and the pixel stage the atlas, so the pipeline takes both.
    auto const pipeline_layout
        = ctx.cached.acquire_pipeline_layout<sgl_shaders::imgui_atlas, sgl_shaders::imgui_constants>();

    // imgui emits both windings so culling is off, and it is drawn in list order so there is no depth test.
    // Alpha blending is imgui's standard straight-alpha equation;
    // the alpha channel uses one/inv-src-alpha so compositing onto a transparent target accumulates coverage correctly rather than saturating.
    _pipeline = ctx.cached.acquire_raster_pipeline(
        sg::raster_pipeline_description{.layout = pipeline_layout,
                                        .vertex_shader = *compiled_vs,
                                        .fragment_shader = *compiled_ps,
                                        .vertex_input = sgl_shaders::imgui_vertex::layout(),
                                        .topology = sg::primitive_topology::triangle_list,
                                        .rasterization = {.cull = sg::cull_mode::none},
                                        .color_targets = {{.format = params(), .blend = sg::blend_alpha}}});

    // Awaited here rather than polled in execute, so `ready` means ready.
    co_await cc::async_settled(_pipeline);
    co_return;
}

imgui_routine::prepared_frame imgui_routine::prepare(sg::command_list& cmd, ImDrawData* draw_data)
{
    CC_ASSERT(draw_data != nullptr, "draw data must not be null — call ImGui::Render() first");
    auto& ctx = cmd.context();
    auto frame = prepared_frame{.draw_data = draw_data};

    // A new texture's bytes go out on ctx.upload's copy queue, and the barrier tracker makes this list wait on them at
    // submit; an update is a copy on this list, because by then the atlas has been sampled and the copy queue cannot
    // move it out of `shader_texture` for itself.
    auto textures = impl::imgui_texture_routine::try_acquire_exclusive(cmd);
    if (textures.is_ready())
        textures->service_requests(cmd, draw_data);

    if (draw_data->TotalVtxCount == 0 || draw_data->TotalIdxCount == 0)
        return frame;

    frame.vertices = ctx.transient.create_buffer<ImDrawVert>(
        isize(draw_data->TotalVtxCount), sg::buffer_usage::vertex_buffer | sg::buffer_usage::copy_dst);
    frame.indices = ctx.transient.create_buffer<u32>(isize(draw_data->TotalIdxCount),
                                                     sg::buffer_usage::index_buffer | sg::buffer_usage::copy_dst);

    // imgui keeps one vertex/index buffer per draw list; we concatenate them into one pair, and the draw loop offsets each list's commands accordingly.
    auto vertex_offset = isize(0);
    auto index_offset = isize(0);
    for (auto const* const list : draw_data->CmdLists)
    {
        cmd.upload.data_to_buffer(
            frame.vertices, cc::span<ImDrawVert const>(list->VtxBuffer.Data, list->VtxBuffer.Size), vertex_offset);
        cmd.upload.data_to_buffer(frame.indices, cc::span<u32 const>(list->IdxBuffer.Data, list->IdxBuffer.Size),
                                  index_offset);
        vertex_offset += isize(list->VtxBuffer.Size);
        index_offset += isize(list->IdxBuffer.Size);
    }
    return frame;
}

sg::routine_outcome imgui_routine::execute(sg::rendering_scope& scope, prepared_frame const& frame)
{
    CC_ASSERT(frame.draw_data != nullptr, "execute draws what prepare returned; call prepare before the scope opens");
    auto* const draw_data = frame.draw_data;

    auto& cmd = scope.command_list();
    CC_ASSERT(!scope.color_formats().empty(), "imgui must be drawn into a scope with a color target");
    auto const target_format = scope.color_formats()[0];
    auto const target_size = scope.render_target_size();
    CC_ASSERT(!sg::is_srgb_format(target_format), "imgui colors are already sRGB-encoded; bind a non-srgb view of "
                                                  "the target instead");

    // The target's format picks the instance, and is only knowable once the caller's scope is open.
    auto self = try_acquire_exclusive(cmd, target_format);
    if (!self.is_ready())
        return sg::routine_outcome::declined;
    auto& ctx = cmd.context();
    auto textures = self.acquire_exclusive(self->_textures);

    // Polled rather than waited on: execute runs inside the caller's rendering scope, so nothing here may block, and
    // a throw would leave their command list unsubmitted.
    auto const* const pipeline = self->_pipeline != nullptr ? self->_pipeline->try_value() : nullptr;
    if (pipeline == nullptr || *pipeline == nullptr)
        return sg::routine_outcome::declined;
    if (draw_data->TotalVtxCount == 0 || draw_data->TotalIdxCount == 0)
        return sg::routine_outcome::executed; // nothing to draw is not a refusal

    scope.bind_pipeline(**pipeline);
    scope.bind_vertex_buffer(frame.vertices.as_vertex_buffer());
    scope.bind_index_buffer(frame.indices.as_index_buffer());
    scope.set_viewport({.offset = tg::pos2f(0.0f, 0.0f), .size = tg::vec2f(float(target_size[0]), float(target_size[1]))});
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
                auto const texture = textures->try_texture_of(dc.GetTexID());
                if (texture.has_error())
                    continue; // imgui named a texture we never created; skip rather than bind garbage

                // Transient: one descriptor allocation per texture switch, recycled with the epoch.
                // With a single font atlas that is one group for the whole frame.
                //
                // The layout comes from init rather than from the create: this is the frame path, and
                // acquiring would hash the declared table and take the pipeline cache's lock per switch.
                bound_group = ctx.transient.create_binding_group(
                    cmd, self->_group_layout, sgl_shaders::imgui_atlas{.texture = texture.value().as_texture_view()});
                scope.bind_group(0, *bound_group);
                bound_texture = dc.GetTexID();
            }

            scope.set_scissor(scissor.value());
            scope.draw_indexed(
                {.index_range = {.offset = global_index_offset + isize(dc.IdxOffset), .size = isize(dc.ElemCount)},
                 .vertex_offset = global_vertex_offset + int(dc.VtxOffset)});
        }

        global_vertex_offset += list->VtxBuffer.Size;
        global_index_offset += isize(list->IdxBuffer.Size);
    }
    return sg::routine_outcome::executed;
}

void render_imgui(imgui_context& imgui, sg::context& ctx, sg::swapchain& main, tg::vec4f clear_color)
{
    // The viewport windows are moved, drawn and presented before the main window's present — see render_viewports for why that order matters.
    // They go first rather than between the main list's recording and its submit:
    // a viewport list submitted while the main list is still open makes the two concurrent, and the font atlas both sample is then reverted to its canonical layout at every viewport submit.
    imgui.update_viewports();
    imgui_routine::render_viewports(ctx);

    auto rt = main.acquire_backbuffer();
    auto cmd = ctx.create_command_list();
    auto const frame = imgui_routine::prepare(*cmd, ImGui::GetDrawData());
    {
        auto pass = cmd->raster.render_to({.color_targets = {rt.cleared(clear_color)}});
        // The frame is presented either way: a cleared target is the honest "nothing drawn yet" while the shaders
        // build, and skipping the present would stall the window instead.
        (void)imgui_routine::execute(pass, frame);
    }
    ctx.submit_command_list_and_present(main, cc::move(cmd));
}
} // namespace sr
