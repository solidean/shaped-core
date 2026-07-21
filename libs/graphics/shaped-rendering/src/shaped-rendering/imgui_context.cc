#include <clean-core/common/asserts.hh>
#include <clean-core/memory/allocation.hh> // cc::default_memory_resource
#include <imgui.h>
#include <shaped-rendering/imgui_context.hh>

namespace sr
{
namespace
{
// imgui's free callback gets only a pointer, but cc::memory_resource needs the size back. So each block
// carries its byte count in a header immediately before the pointer imgui sees. The header is a full
// `alignment` bytes rather than sizeof(isize), which is what keeps the returned pointer aligned — imgui
// allocates types up to 16-byte alignment.
constexpr auto imgui_alloc_alignment = cc::isize(16);
static_assert(imgui_alloc_alignment >= cc::isize(sizeof(cc::isize)), "the header must fit in one alignment unit");

void* imgui_alloc(size_t size, void*)
{
    auto const total = cc::isize(size) + imgui_alloc_alignment;

    cc::byte* base = nullptr;
    auto const& res = *cc::default_memory_resource;
    (void)res.allocate_bytes(&base, total, total, imgui_alloc_alignment, res.userdata);
    CC_ASSERT(base != nullptr, "imgui allocation failed");

    *reinterpret_cast<cc::isize*>(base) = total;
    return base + imgui_alloc_alignment;
}

void imgui_free(void* ptr, void*)
{
    if (ptr == nullptr)
        return; // imgui frees null freely

    auto* const base = reinterpret_cast<cc::byte*>(ptr) - imgui_alloc_alignment;
    auto const total = *reinterpret_cast<cc::isize*>(base);

    auto const& res = *cc::default_memory_resource;
    res.deallocate_bytes(base, total, imgui_alloc_alignment, res.userdata);
}
} // namespace

imgui_context imgui_context::create()
{
    CC_ASSERT(ImGui::GetCurrentContext() == nullptr, "an ImGui context already exists — only one at a time");

    // Must precede CreateContext: the context itself is the first thing allocated. Routing here rather
    // than by editing the vendored imconfig.h is what lets that file stay byte-identical to upstream.
    ImGui::SetAllocatorFunctions(&imgui_alloc, &imgui_free);

    auto* const ctx = ImGui::CreateContext();
    auto& io = ImGui::GetIO();

    // Docking needs nothing from a windowing system — it works within the single viewport we already have.
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // TODO(windowing): the platform half of this backend is not wired yet. When the windowing system
    // lands it must, each frame or at setup:
    //   - set io.BackendPlatformName, and the ImGuiBackendFlags_HasMouseCursors / _HasSetMousePos flags
    //     it can honour
    //   - feed input events: io.AddMousePosEvent / AddMouseButtonEvent / AddMouseWheelEvent,
    //     io.AddKeyEvent (ImGuiKey), io.AddInputCharacter, io.AddFocusEvent
    //   - drive the OS cursor from ImGui::GetMouseCursor()
    //   - install the clipboard hooks on ImGui::GetPlatformIO() (Platform_GetClipboardTextFn /
    //     Platform_SetClipboardTextFn)
    // Until then begin_frame supplies only size and timestep, so the UI renders but does not respond.

    // TODO(windowing): multi-viewport (ImGuiConfigFlags_ViewportsEnable) is deliberately left off. It
    // needs the platform layer to create real OS windows and a swapchain per viewport, plus the renderer
    // side of ImGuiPlatformIO (Renderer_CreateWindow / SetWindowSize / RenderWindow / SwapBuffers).
    // sr::imgui_draw_routine already folds draw_data->DisplayPos into its projection and scissor math,
    // which is the part that would otherwise have to change.

    return imgui_context(ctx);
}

imgui_context::~imgui_context()
{
    if (_ctx != nullptr)
        ImGui::DestroyContext(_ctx);
}

imgui_context::imgui_context(imgui_context&& other) noexcept : _ctx(other._ctx)
{
    other._ctx = nullptr;
}

imgui_context& imgui_context::operator=(imgui_context&& other) noexcept
{
    if (this != &other)
    {
        if (_ctx != nullptr)
            ImGui::DestroyContext(_ctx);
        _ctx = other._ctx;
        other._ctx = nullptr;
    }
    return *this;
}

void imgui_context::begin_frame(frame_info const& info)
{
    CC_ASSERT(_ctx != nullptr, "imgui context is not valid");
    CC_ASSERT(info.delta_time > 0.0f, "delta time must be positive — imgui divides by it");
    CC_ASSERT(info.display_size[0] > 0 && info.display_size[1] > 0, "display size must be positive");

    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(info.display_size[0]), float(info.display_size[1]));
    io.DisplayFramebufferScale = ImVec2(info.framebuffer_scale[0], info.framebuffer_scale[1]);
    io.DeltaTime = info.delta_time;

    ImGui::NewFrame();
}

void imgui_context::end_frame()
{
    CC_ASSERT(_ctx != nullptr, "imgui context is not valid");
    ImGui::Render(); // builds the draw data ImGui::GetDrawData() returns
}
} // namespace sr
