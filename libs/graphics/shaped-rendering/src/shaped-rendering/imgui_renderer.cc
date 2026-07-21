#include <clean-core/common/asserts.hh>
#include <imgui.h>
#include <shaped-graphics/command_list.hh>
#include <shaped-graphics/context.hh>
#include <shaped-rendering/imgui_draw_routine.hh>
#include <shaped-rendering/imgui_renderer.hh>

namespace sr
{
imgui_renderer imgui_renderer::create(sg::context& ctx)
{
    CC_ASSERT(ImGui::GetCurrentContext() != nullptr, "create an sr::imgui_context before the renderer");
    (void)ctx; // no GPU work here; the routine builds its pipelines lazily against the actual target

    auto& io = ImGui::GetIO();

    // We drain ImDrawData::Textures ourselves (imgui_texture_registry), which is what lets imgui grow and
    // patch the atlas at runtime instead of baking it once.
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

    // We honour ImDrawCmd::VtxOffset, so imgui may keep 16-bit indices past 64k vertices in one draw list
    // rather than splitting it.
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;

    io.BackendRendererName = "shaped-rendering";

    // imgui.hlsl samples .rgba unconditionally; an Alpha8 atlas would come out red-on-transparent.
    io.Fonts->TexDesiredFormat = ImTextureFormat_RGBA32;

    return {};
}

void imgui_renderer::prepare(sg::command_list& cmd, ImDrawData* draw_data)
{
    CC_ASSERT(draw_data != nullptr, "draw data must not be null — call ImGui::Render() first");
    CC_ASSERT(!_prepared, "prepare() called twice for one frame; each prepare() needs a matching render()");

    auto& ctx = cmd.context();

    // Textures first: a draw recorded this frame may sample an atlas imgui only just grew.
    _textures.service_requests(ctx, draw_data);

    _prepared = true;

    if (draw_data->TotalVtxCount == 0 || draw_data->TotalIdxCount == 0)
        return; // nothing drawn this frame — normal with every window collapsed

    _vertices = ctx.transient.create_buffer<ImDrawVert>(isize(draw_data->TotalVtxCount),
                                                        sg::buffer_usage::vertex_buffer | sg::buffer_usage::copy_dst);
    _indices = ctx.transient.create_buffer<u16>(isize(draw_data->TotalIdxCount),
                                                sg::buffer_usage::index_buffer | sg::buffer_usage::copy_dst);

    // imgui keeps one vertex/index buffer per draw list; we concatenate them into one pair, and the draw
    // loop offsets each list's commands accordingly.
    auto vertex_offset = isize(0);
    auto index_offset = isize(0);
    for (auto const* const list : draw_data->CmdLists)
    {
        cmd.upload.data_to_buffer(_vertices, cc::span<ImDrawVert const>(list->VtxBuffer.Data, list->VtxBuffer.Size),
                                  vertex_offset);
        cmd.upload.data_to_buffer(_indices, cc::span<u16 const>(list->IdxBuffer.Data, list->IdxBuffer.Size),
                                  index_offset);
        vertex_offset += isize(list->VtxBuffer.Size);
        index_offset += isize(list->IdxBuffer.Size);
    }
}

void imgui_renderer::render(sg::command_list& cmd, ImDrawData* draw_data, target_info const& target)
{
    CC_ASSERT(draw_data != nullptr, "draw data must not be null");
    CC_ASSERT(_prepared, "render() needs a prepare() for this frame, recorded before the rendering scope");

    _prepared = false;

    if (draw_data->TotalVtxCount == 0 || draw_data->TotalIdxCount == 0)
        return;

    imgui_draw_routine::execute(cmd, draw_data,
                                {.target_format = target.target_format,
                                 .target_size = target.target_size,
                                 .vertices = _vertices,
                                 .indices = _indices,
                                 .textures = _textures});
}
} // namespace sr
