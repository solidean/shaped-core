#include "dx12-test-common.hh"

#include <nexus/test.hh>
#include <shaped-graphics/all.hh>

// Swapchain smoke tests driven through the sg:: API against the WARP context. A swapchain needs a real
// OS window, so these create a hidden top-level window and SKIP when that isn't possible (a headless /
// session-0 host with no interactive window station, where either window or swapchain creation fails) —
// so the suite still runs clean on CI. Win32 windowing is reached through the sanitized <Windows.h> that
// dx12-test-common.hh pulls in.

namespace
{
namespace dx12 = sg::backend::dx12;

// A hidden overlapped window for swapchain tests. Message-only (HWND_MESSAGE) windows cannot back a
// swapchain, so this is a real window that is simply never shown. `hwnd` is null when creation fails.
struct test_window
{
    HWND hwnd = nullptr;
    int client_w = 0;
    int client_h = 0;

    explicit test_window(int w, int h)
    {
        static ATOM const cls = []
        {
            WNDCLASSEXW wc = {};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"sg_dx12_swapchain_test_window";
            return RegisterClassExW(&wc);
        }();
        (void)cls;

        RECT rc = {0, 0, w, h};
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        hwnd = CreateWindowExW(0, L"sg_dx12_swapchain_test_window", L"sg swapchain test", WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
        if (hwnd != nullptr)
        {
            RECT cr = {};
            GetClientRect(hwnd, &cr);
            client_w = int(cr.right - cr.left);
            client_h = int(cr.bottom - cr.top);
        }
    }

    ~test_window()
    {
        if (hwnd != nullptr)
            DestroyWindow(hwnd);
    }

    // Resizes the client area to (w, h); records and returns the resulting client size (which may differ
    // slightly from the request once the non-client frame is accounted for).
    tg::vec2i resize_client(int w, int h)
    {
        RECT rc = {0, 0, w, h};
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        RECT cr = {};
        GetClientRect(hwnd, &cr);
        client_w = int(cr.right - cr.left);
        client_h = int(cr.bottom - cr.top);
        return tg::vec2i(client_w, client_h);
    }

    test_window(test_window const&) = delete;
    test_window& operator=(test_window const&) = delete;
};
} // namespace

TEST("sg dx12 - swapchain create, describe, and present on a hidden window")
{
    auto ctx = dx12::acquire_warp_context();
    REQUIRE(ctx != nullptr);

    test_window win(256, 256);
    if (win.hwnd == nullptr)
        SKIP("no interactive window station (headless host) — cannot create a window");

    auto sc_result = ctx->try_create_swapchain({
        .native_window_handle = win.hwnd,
        .buffer_count = 2,
        .format = sg::pixel_format::bgra8_unorm,
        .present_mode = sg::present_mode::vsync,
    });
    if (sc_result.has_error())
        SKIP("WARP could not create a swapchain for the window (likely a headless / session-0 host)");

    sg::swapchain_handle const sc = sc_result.value();
    REQUIRE(sc != nullptr);

    // Description getters reflect what was requested.
    CHECK(sc->buffer_count() == 2);
    CHECK(sc->format() == sg::pixel_format::bgra8_unorm);
    CHECK(sc->present_mode() == sg::present_mode::vsync);
    CHECK(!sc->is_hdr_enabled());
    CHECK(sc->native_window_handle() == win.hwnd);

    // A few frames: acquire -> clear the back buffer -> present. The acquired view carries this frame's
    // size (the swapchain has no size getter). buffer_count 2 means the third frame exercises the
    // present-fence reuse wait. WARP's debug behaviour is the oracle — a bad barrier / present would fault.
    for (int frame = 0; frame < 3; ++frame)
    {
        auto rt = sc->acquire_backbuffer();
        CHECK(rt.width() == win.client_w);
        CHECK(rt.height() == win.client_h);
        CHECK(rt.format() == sg::pixel_format::bgra8_unorm);

        auto cmd = ctx->create_command_list();
        {
            auto pass
                = cmd->raster.render_to({.color_targets = {rt.cleared(tg::vec4f(0.0f, float(frame) * 0.25f, 1, 1))}});
        }
        ctx->submit_command_list_and_present(*sc, cc::move(cmd));
    }

    ctx->advance_epoch_and_wait_for_idle();
}

TEST("sg dx12 - swapchain auto-resizes to its window")
{
    auto ctx = dx12::acquire_warp_context();
    REQUIRE(ctx != nullptr);

    test_window win(200, 150);
    if (win.hwnd == nullptr)
        SKIP("no interactive window station (headless host) — cannot create a window");

    auto sc_result = ctx->try_create_swapchain({.native_window_handle = win.hwnd});
    if (sc_result.has_error())
        SKIP("WARP could not create a swapchain for the window (likely a headless / session-0 host)");
    sg::swapchain_handle const sc = sc_result.value();

    // Auto-resize is checked on the first acquire of each epoch, so drive it like a real frame loop:
    // acquire -> render -> present -> advance_epoch, resizing the window between epochs.
    auto frame = [&](tg::vec2i expected, tg::vec4f color)
    {
        auto rt = sc->acquire_backbuffer();
        CHECK(rt.width() == expected[0]);
        CHECK(rt.height() == expected[1]);
        auto cmd = ctx->create_command_list();
        {
            auto pass = cmd->raster.render_to({.color_targets = {rt.cleared(color)}});
        }
        ctx->submit_command_list_and_present(*sc, cc::move(cmd));
        ctx->advance_epoch(sc->buffer_count());
    };

    frame(tg::vec2i(win.client_w, win.client_h), tg::vec4f(0, 0, 1, 1)); // initial size
    frame(win.resize_client(320, 240), tg::vec4f(1, 0, 0, 1));           // grow — the chain follows
    frame(win.resize_client(128, 96), tg::vec4f(0, 1, 0, 1));            // shrink — and again

    ctx->advance_epoch_and_wait_for_idle();
}
