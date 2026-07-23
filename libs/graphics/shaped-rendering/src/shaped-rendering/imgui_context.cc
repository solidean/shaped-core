#include <clean-core/common/asserts.hh>
#include <clean-core/memory/allocation.hh> // cc::default_memory_resource
#include <imgui/imgui.h>
#include <shaped-rendering/imgui_context.hh>
#include <shaped-rendering/impl/imgui_input_translation.hh>
#include <shaped-rendering/window.hh>

#include <type_traits>
#include <variant>

namespace sr
{
namespace
{
// imgui's free callback gets only a pointer, but cc::memory_resource needs the size back.
// So each block carries its byte count in a header immediately before the pointer imgui sees.
// The header is a full `alignment` bytes rather than sizeof(isize), which is what keeps the returned pointer aligned — imgui allocates types up to 16-byte alignment.
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

/// What a secondary viewport's PlatformUserData points at.
/// imgui stores only the pointer, so the window it asked us to open is owned here and closed in Platform_DestroyWindow.
/// The main viewport has none of this — its window belongs to the caller.
struct owned_viewport_window
{
    cc::unique_ptr<window> win;
};

/// The system every platform callback acts on, parked in imgui's own backend slot by install_viewports.
window_system& platform_system()
{
    auto* const wsys = static_cast<window_system*>(ImGui::GetIO().BackendPlatformUserData);
    CC_ASSERT(wsys != nullptr, "no window_system installed — begin_frame(window&) does that");
    return *wsys;
}

/// The sr::window behind a viewport, main or secondary alike.
window& window_of(ImGuiViewport* viewport)
{
    CC_ASSERT(viewport != nullptr && viewport->PlatformHandle != nullptr, "viewport has no window");
    return *static_cast<window*>(viewport->PlatformHandle);
}

// imgui's viewport geometry is in its own logical units, while sr::window is in pixels.
// The two coincide while DisplayFramebufferScale is 1, which is every path here today — begin_frame(window&) does no DPI scaling of its own.
// A backend that starts scaling has to convert in both directions right here.

/// Republishes the desktop's monitors into imgui.
/// Refreshed every frame rather than once: monitors come and go while an application runs, and imgui uses the list to keep a viewport window on-screen.
/// It also refuses a frame outright while the list is empty.
void refresh_monitors(window_system const& wsys)
{
    auto& platform_io = ImGui::GetPlatformIO();
    platform_io.Monitors.resize(0);

    // ImGuiPlatformMonitor has a constructor, so it is filled field by field rather than braced.
    for (auto const& display : wsys.displays())
    {
        auto monitor = ImGuiPlatformMonitor();
        monitor.MainPos = ImVec2(float(display.position[0]), float(display.position[1]));
        monitor.MainSize = ImVec2(float(display.size[0]), float(display.size[1]));
        monitor.WorkPos = ImVec2(float(display.work_position[0]), float(display.work_position[1]));
        monitor.WorkSize = ImVec2(float(display.work_size[0]), float(display.work_size[1]));
        monitor.DpiScale = display.content_scale;
        platform_io.Monitors.push_back(monitor);
    }
}

/// Routes a viewport window's close request to imgui.
///
/// TODO(windowing): PlatformRequestMove / PlatformRequestResize are still unset,
/// so imgui never learns when the *window manager* — rather than imgui — moves or resizes a viewport window (a snap, a clamp, a drag by an OS title bar).
/// imgui then keeps its own idea of the geometry and re-commands it.
///
/// It cannot be done by comparing here:
/// sr::window::position holds the position last written through, while the platform reports the last one its event queue confirmed,
/// and during any motion those differ for reasons that have nothing to do with the window manager.
/// Telling the two apart needs the *event* — SDL_EVENT_WINDOW_MOVED — which sr::input_event does not model, since it carries user input rather than window-state changes.
/// The fix is a moved/resized latch on sr::window, set in poll_events and consumed here, which is a window-API change rather than something this file can paper over.
void sync_viewport_requests()
{
    auto& platform_io = ImGui::GetPlatformIO();

    // From 1: the main viewport's window belongs to the caller, including its close request.
    for (auto i = 1; i < platform_io.Viewports.Size; ++i)
    {
        auto* const viewport = platform_io.Viewports[i];
        if (viewport->PlatformHandle == nullptr)
            continue;

        // A viewport window's close request is imgui's to act on — it closes the imgui window that owns the viewport — so it is consumed here rather than left latched for a caller that will never look.
        auto& win = window_of(viewport);
        if (win.is_close_requested())
        {
            viewport->PlatformRequestClose = true;
            win.clear_close_request();
        }
    }
}

void install_viewport_callbacks()
{
    auto& platform_io = ImGui::GetPlatformIO();

    platform_io.Platform_CreateWindow = [](ImGuiViewport* viewport)
    {
        auto const flags = viewport->Flags;

        // Created hidden: imgui calls Platform_ShowWindow once it has positioned the window, and a window that appeared first would flash at whatever spot the window manager picked.
        auto created
            = platform_system().try_create_window({.title = "imgui viewport",
                                                   .width = int(viewport->Size.x),
                                                   .height = int(viewport->Size.y),
                                                   .is_resizable = true,
                                                   .is_visible = false,
                                                   .has_decoration = (flags & ImGuiViewportFlags_NoDecoration) == 0,
                                                   .is_always_on_top = (flags & ImGuiViewportFlags_TopMost) != 0,
                                                   .has_taskbar_icon = (flags & ImGuiViewportFlags_NoTaskBarIcon) == 0,
                                                   .is_focusable = (flags & ImGuiViewportFlags_NoFocusOnAppearing) == 0});

        // Nothing a caller could act on from inside an imgui callback, and returning without a window would hand imgui a viewport it goes on to dereference.
        CC_ASSERT(created.has_value(), "could not open a window for an imgui viewport");

        auto* const owned = IM_NEW(owned_viewport_window)();
        owned->win = cc::move(created.value());
        owned->win->set_position(tg::pos2i(int(viewport->Pos.x), int(viewport->Pos.y)));

        viewport->PlatformUserData = owned;
        viewport->PlatformHandle = owned->win.get();
        viewport->PlatformHandleRaw = owned->win->native_window_handle();
    };

    platform_io.Platform_DestroyWindow = [](ImGuiViewport* viewport)
    {
        // Null for the main viewport, whose window the caller owns — imgui still routes it through here.
        if (auto* const owned = static_cast<owned_viewport_window*>(viewport->PlatformUserData))
            IM_DELETE(owned);

        viewport->PlatformUserData = nullptr;
        viewport->PlatformHandle = nullptr;
        viewport->PlatformHandleRaw = nullptr;
    };

    platform_io.Platform_ShowWindow = [](ImGuiViewport* viewport) { window_of(viewport).show(); };

    platform_io.Platform_SetWindowPos = [](ImGuiViewport* viewport, ImVec2 pos)
    { window_of(viewport).set_position(tg::pos2i(int(pos.x), int(pos.y))); };

    // The cached position, deliberately — SDL_GetWindowPosition reports the last position the *event queue* confirmed (SDL_video.c: it only consults the pending one for hidden windows),
    // so during a drag it lags by a pump.
    // window::position holds what set_position last wrote through, which on a synchronous platform is where the window already is.
    platform_io.Platform_GetWindowPos = [](ImGuiViewport* viewport) -> ImVec2
    {
        auto const pos = window_of(viewport).position();
        return ImVec2(float(pos[0]), float(pos[1]));
    };

    platform_io.Platform_SetWindowSize = [](ImGuiViewport* viewport, ImVec2 size)
    { window_of(viewport).set_size(tg::vec2i(int(size.x), int(size.y))); };

    platform_io.Platform_GetWindowSize = [](ImGuiViewport* viewport) -> ImVec2
    {
        auto const& win = window_of(viewport);
        return ImVec2(float(win.width()), float(win.height()));
    };

    platform_io.Platform_SetWindowFocus = [](ImGuiViewport* viewport) { window_of(viewport).focus(); };
    platform_io.Platform_GetWindowFocus = [](ImGuiViewport* viewport) { return window_of(viewport).is_focused(); };
    platform_io.Platform_GetWindowMinimized = [](ImGuiViewport* viewport) { return window_of(viewport).is_minimized(); };

    platform_io.Platform_SetWindowTitle = [](ImGuiViewport* viewport, char const* title)
    { window_of(viewport).set_title(title != nullptr ? title : ""); };
}
} // namespace

imgui_context imgui_context::create(imgui_context_description const& desc)
{
    CC_ASSERT(ImGui::GetCurrentContext() == nullptr, "an ImGui context already exists — only one at a time");

    // Must precede CreateContext: the context itself is the first thing allocated.
    // Routing here rather than by editing the vendored imconfig.h is what lets that file stay byte-identical to upstream.
    ImGui::SetAllocatorFunctions(&imgui_alloc, &imgui_free);

    auto* const ctx = ImGui::CreateContext();
    auto& io = ImGui::GetIO();

    // Docking needs nothing from a windowing system — it works within the single viewport we already have.
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // The renderer capabilities sr::imgui_routine implements.
    // Declared here rather than in the routine because imgui reads them from the first NewFrame() onward,
    // and the routine is not acquired until the first execute() — by which time frame one has already been built the legacy way.
    //
    //   RendererHasTextures  we drain ImDrawData::Textures ourselves, which is what lets imgui grow and patch the atlas at runtime instead of baking it once
    //   RendererHasVtxOffset we honour ImDrawCmd::VtxOffset, so imgui may keep 16-bit indices past 64k vertices in one draw list rather than splitting it
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.BackendRendererName = "shaped-rendering";

    // imgui.hlsl samples .rgba unconditionally; an Alpha8 atlas would come out red-on-transparent.
    io.Fonts->TexDesiredFormat = ImTextureFormat_RGBA32;

    io.BackendPlatformName = "shaped-rendering (sr::window)";

    // We set the cursor shape from ImGui::GetMouseCursor each frame — see begin_frame(window&, float).
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;

    // The config flag itself is set in install_viewports, not here: viewports need a window_system, and
    // begin_frame(window&) is the first point one is in reach.
    auto result = imgui_context(ctx);
    result._viewports_requested = desc.enable_viewports;
    return result;
}

namespace
{
// The ImGuiContext deleter, held by imgui_context's unique_ptr.
// Captureless, so it decays to the plain function pointer that unique_ptr stores — which is what lets ImGuiContext stay incomplete in the header.
// Null-safe: a moved-from or default-constructed context holds no pointer, so this is never called on null.
constexpr auto destroy_imgui_context = [](ImGuiContext* ctx) noexcept
{
    if (ctx == nullptr)
        return;

    // imgui checks at shutdown that whoever installed a backend also removed it, and reports a leaked platform backend rather than tolerating it.
    // Closing the platform windows first is what destroys the sr::windows behind any open viewport —
    // so the window_system has to still be alive here, which is why the header says it must outlive the context.
    if (ImGui::GetCurrentContext() == ctx)
    {
        ImGui::DestroyPlatformWindows();

        auto* const main_viewport = ImGui::GetMainViewport();
        main_viewport->PlatformHandle = nullptr;
        main_viewport->PlatformHandleRaw = nullptr;

        auto& io = ImGui::GetIO();
        io.BackendPlatformUserData = nullptr;
        io.BackendFlags &= ~ImGuiBackendFlags_PlatformHasViewports;
        io.BackendFlags &= ~ImGuiBackendFlags_RendererHasViewports;
    }

    ImGui::DestroyContext(ctx);
};
} // namespace

// A function-pointer deleter has no default, so both ctors install `destroy_imgui_context` explicitly; the null default-constructed context never invokes it.
// Destroy + move are then plain defaults.
imgui_context::imgui_context() : _ctx(nullptr, destroy_imgui_context)
{
}
imgui_context::imgui_context(ImGuiContext* ctx) : _ctx(ctx, destroy_imgui_context)
{
}

imgui_context::~imgui_context() = default;
imgui_context::imgui_context(imgui_context&&) noexcept = default;
imgui_context& imgui_context::operator=(imgui_context&&) noexcept = default;

void imgui_context::install_clipboard(window_system& wsys)
{
    auto& platform_io = ImGui::GetPlatformIO();

    // Idempotent: begin_frame runs this every frame, and re-pointing the same hooks at the same system is a no-op.
    // Re-checked rather than latched on a flag so a caller that drives two systems in sequence gets the live one.
    if (platform_io.Platform_ClipboardUserData == &wsys)
        return;

    platform_io.Platform_ClipboardUserData = &wsys;

    platform_io.Platform_GetClipboardTextFn = [](ImGuiContext*) -> char const*
    {
        auto& system = *static_cast<window_system*>(ImGui::GetPlatformIO().Platform_ClipboardUserData);

        // imgui borrows the returned pointer and reads it before it calls back in again, so the text has to outlive the return.
        // Every backend parks it somewhere for exactly this reason; the alternative is leaking a copy per paste.
        // Only ever touched from the one thread imgui is driven on.
        static cc::string held;
        held = cc::string::create_copy_c_str_materialized(system.clipboard_text());
        return held.c_str_if_terminated();
    };

    platform_io.Platform_SetClipboardTextFn = [](ImGuiContext*, char const* text)
    {
        auto& system = *static_cast<window_system*>(ImGui::GetPlatformIO().Platform_ClipboardUserData);
        system.set_clipboard_text(text != nullptr ? cc::string_view(text) : cc::string_view());
    };
}

void imgui_context::install_viewports(window_system& wsys, window& main_window)
{
    if (!_viewports_requested)
        return;

    auto& io = ImGui::GetIO();

    // Idempotent for the same system, like install_clipboard — but the main viewport's handles are restated every frame regardless, because the caller may drive a different window than it did last frame.
    if (io.BackendPlatformUserData != &wsys)
    {
        io.BackendPlatformUserData = &wsys;

        io.BackendFlags |= ImGuiBackendFlags_PlatformHasViewports;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;

        // Turned on here rather than in create(): viewports need a window_system, and this is the first point one is in reach.
        // A caller driving begin_frame(frame_info) alone never gets here and so stays single-viewport, which is the only thing it could be.
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

        install_viewport_callbacks();
    }

    refresh_monitors(wsys);

    // Before NewFrame, which is where imgui consumes these.
    sync_viewport_requests();

    // The main viewport is the caller's window, which sr does not own — so PlatformUserData stays null and Platform_DestroyWindow has nothing to free for it.
    auto* const main_viewport = ImGui::GetMainViewport();
    main_viewport->PlatformHandle = &main_window;
    main_viewport->PlatformHandleRaw = main_window.native_window_handle();
}

void imgui_context::update_viewports()
{
    CC_ASSERT(_ctx != nullptr, "imgui context is not valid");

    _viewport_update_pending = false;

    // Creates, moves, resizes and closes the OS windows behind imgui's viewports, through the platform callbacks above.
    // Pairs with sr::imgui_routine::render_viewports, which draws and presents them.
    if ((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0)
        ImGui::UpdatePlatformWindows();
}

void imgui_context::process_events(window_system const& wsys)
{
    CC_ASSERT(_ctx != nullptr, "imgui context is not valid");
    auto const events = wsys.events();
    for (auto i = isize(0); i < events.size(); ++i)
    {
        // Collapse a run of consecutive motion events into its last.
        // imgui's ConfigInputTrickleEventQueue (on by default) applies at most one mouse-position change per frame and defers the rest,
        // so a fast mouse producing several motions per frame builds a backlog that only drains once it stops moving.
        //
        // Only *consecutive* ones, and only within the same window:
        // that never moves a position across the button or wheel event that follows it, which is the ordering the trickle exists to preserve.
        if (std::holds_alternative<mouse_move_event>(events[i].payload)        //
            && i + 1 < events.size()                                           //
            && std::holds_alternative<mouse_move_event>(events[i + 1].payload) //
            && events[i].window == events[i + 1].window)
            continue;

        process_event(events[i]);
    }
}

void imgui_context::process_event(input_event const& event)
{
    CC_ASSERT(_ctx != nullptr, "imgui context is not valid");
    auto& io = ImGui::GetIO();

    // Mouse positions arrive relative to the window the event went to, but once viewports are on imgui wants them in desktop space:
    // it hit-tests the position against every viewport's rectangle to decide which one the pointer is over, and a window-relative coordinate would name the wrong one.
    // With viewports off there is a single viewport at the origin and the two spaces coincide, so this is the identity.
    //
    auto const to_imgui_space = [&](tg::pos2f cursor_pos)
    {
        if ((io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) == 0 || event.window == nullptr)
            return cursor_pos;

        auto const origin = event.window->position();
        return cursor_pos + tg::vec2f(float(origin[0]), float(origin[1]));
    };

    std::visit(
        [&](auto const& e)
        {
            using event_type = std::decay_t<decltype(e)>;

            if constexpr (std::is_same_v<event_type, key_event>)
            {
                // Modifiers first: imgui reads them as of the key event that follows, so a ctrl+Z arriving in one frame must set ctrl before Z or the shortcut is missed.
                io.AddKeyEvent(ImGuiMod_Ctrl, has_all(e.modifiers, key_modifiers::ctrl));
                io.AddKeyEvent(ImGuiMod_Shift, has_all(e.modifiers, key_modifiers::shift));
                io.AddKeyEvent(ImGuiMod_Alt, has_all(e.modifiers, key_modifiers::alt));
                io.AddKeyEvent(ImGuiMod_Super, has_all(e.modifiers, key_modifiers::super));

                // A position imgui does not model is dropped rather than reported as a neighbour.
                if (auto const key = impl::imgui_key_from_scancode(e.scancode); key != ImGuiKey_None)
                    io.AddKeyEvent(key, e.is_down);
            }
            else if constexpr (std::is_same_v<event_type, text_event>)
            {
                // Text goes in as committed UTF-8, never reconstructed from key events — that is the only path that gets IME composition, dead keys and paste right.
                // cc::string is not null-terminated and imgui takes a C string, so materialize one.
                auto terminated = cc::string::create_copy_c_str_materialized(e.text);
                io.AddInputCharactersUTF8(terminated.c_str_if_terminated());
            }
            else if constexpr (std::is_same_v<event_type, mouse_move_event>)
            {
                auto const pos = to_imgui_space(e.cursor_pos);
                io.AddMousePosEvent(pos[0], pos[1]);
            }
            else if constexpr (std::is_same_v<event_type, mouse_button_event>)
            {
                // The position first: a click carries where it happened, and imgui decides what was hit from the position it holds when the button event arrives.
                auto const pos = to_imgui_space(e.cursor_pos);
                io.AddMousePosEvent(pos[0], pos[1]);
                io.AddMouseButtonEvent(impl::imgui_mouse_button_from(e.button), e.is_down);
            }
            else if constexpr (std::is_same_v<event_type, mouse_wheel_event>)
            {
                // Deliberately no position here, unlike the button case above.
                // imgui already holds the cursor position from the motion events, so e.cursor_pos would only restate it —
                // and with ConfigInputTrickleEventQueue on (imgui's default) a position change followed by a wheel is split across two frames,
                // so restating it costs a frame of scroll latency for nothing.
                io.AddMouseWheelEvent(e.delta[0], e.delta[1]);
            }
        },
        event.payload);
}

bool imgui_context::wants_keyboard() const
{
    CC_ASSERT(_ctx != nullptr, "imgui context is not valid");
    return ImGui::GetIO().WantCaptureKeyboard;
}

bool imgui_context::wants_mouse() const
{
    CC_ASSERT(_ctx != nullptr, "imgui context is not valid");
    return ImGui::GetIO().WantCaptureMouse;
}

void imgui_context::begin_frame(window& win, float delta_time)
{
    CC_ASSERT(_ctx != nullptr, "imgui context is not valid");

    auto& wsys = win.system();
    install_clipboard(wsys);
    install_viewports(wsys, win);

    // Both of these act on the frame just ended, which is what a backend does:
    // the widget that was hovered or took focus last frame is the one whose cursor and IME state apply now.
    if (!win.is_minimized())
    {
        auto const wants_text = ImGui::GetIO().WantTextInput;
        if (wants_text && !win.is_text_input_active())
            win.start_text_input();
        else if (!wants_text && win.is_text_input_active())
            win.stop_text_input();

        // imgui asks for no cursor at all over a widget that draws its own; everything else is a shape.
        // Left alone entirely while imgui does not want the mouse, so an application drawing its own cursor over the 3D view is not fought over every frame.
        if (ImGui::GetIO().WantCaptureMouse)
        {
            auto const requested = ImGui::GetMouseCursor();
            if (requested == ImGuiMouseCursor_None)
            {
                wsys.set_cursor_visible(false);
            }
            else
            {
                wsys.set_cursor(impl::cursor_shape_from_imgui(requested));
                wsys.set_cursor_visible(true);
            }
        }
    }

    // A minimized window is 0x0, which begin_frame(frame_info) rejects.
    // Substitute a 1x1 display so imgui still gets a well-formed frame — the caller skips rendering it anyway.
    auto const size = win.is_minimized() ? tg::vec2i(1, 1) : tg::vec2i(win.width(), win.height());
    begin_frame({.display_size = size, .delta_time = delta_time});
}

void imgui_context::begin_frame(frame_info const& info)
{
    CC_ASSERT(_ctx != nullptr, "imgui context is not valid");
    CC_ASSERT(info.delta_time > 0.0f, "delta time must be positive — imgui divides by it");
    CC_ASSERT(info.display_size[0] > 0 && info.display_size[1] > 0, "display size must be positive");

    // Caught here rather than left to surface as "the mouse hovers nothing":
    // with viewports on, imgui only hit-tests the mouse against viewports UpdatePlatformWindows has published.
    CC_ASSERT(!_viewport_update_pending, "update_viewports() must run once per frame after end_frame() while viewports "
                                         "are enabled");

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

    _viewport_update_pending = (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0;
}
} // namespace sr
