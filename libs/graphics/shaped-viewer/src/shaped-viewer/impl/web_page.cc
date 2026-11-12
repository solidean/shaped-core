#include <shaped-viewer/impl/web_page.hh>

#ifdef __EMSCRIPTEN__
#include <clean-core/common/utility.hh> // cc::move, cc::exchange
#include <clean-core/string/string.hh>
#include <clean-core/thread/mutex.hh>
#include <emscripten/em_js.h>
#include <emscripten/emscripten.h>
#endif

using namespace cc::primitive_defines;

#ifdef __EMSCRIPTEN__
namespace
{
/// The canvas's events since the frame loop last took them; filled from JavaScript.
cc::mutex<cc::vector<sr::input_event>> g_events;

/// The drawing-buffer size the canvas last changed to, until the frame loop takes it.
cc::mutex<cc::optional<tg::vec2i>> g_resized;

/// The browser's next frame callback, settled by `sv_page_animation_frame`.
cc::shared_async<cc::unit> g_animation_frame;

bool g_input_installed = false;

void push_event(sr::input_event e)
{
    g_events.lock([&](cc::vector<sr::input_event>& events) { events.push_back(cc::move(e)); });
}
} // namespace

// The canvas's listeners call these; positions are canvas pixels, `density` canvas pixels per CSS pixel.
extern "C"
{
    EMSCRIPTEN_KEEPALIVE void sv_page_mouse_move(float x, float y, float dx, float dy, float density, int held)
    {
        push_event({.pixel_density = density,
                    .payload = sr::mouse_move_event{.cursor_pos = tg::pos2f(x, y),
                                                    .delta = tg::vec2f(dx, dy),
                                                    .pixel_density = density,
                                                    .held_buttons = u8(held)}});
    }

    EMSCRIPTEN_KEEPALIVE void sv_page_mouse_button(int button, int down, float x, float y, int modifiers, int clicks)
    {
        push_event({.payload = sr::mouse_button_event{.button = sr::mouse_button(button),
                                                      .modifiers = sr::key_modifiers(modifiers),
                                                      .is_down = down != 0,
                                                      .clicks = clicks,
                                                      .cursor_pos = tg::pos2f(x, y)}});
    }

    EMSCRIPTEN_KEEPALIVE void sv_page_mouse_wheel(float dx, float dy, float x, float y)
    {
        push_event({.payload = sr::mouse_wheel_event{.delta = tg::vec2f(dx, dy), .cursor_pos = tg::pos2f(x, y)}});
    }

    EMSCRIPTEN_KEEPALIVE void sv_page_key(int scancode, int character, int down, int repeat, int modifiers)
    {
        push_event({.payload = sr::key_event{.scancode = sr::scancode(scancode),
                                             .character = char32_t(character),
                                             .modifiers = sr::key_modifiers(modifiers),
                                             .is_down = down != 0,
                                             .is_repeat = repeat != 0}});
    }

    EMSCRIPTEN_KEEPALIVE void sv_page_resize(int width, int height)
    {
        g_resized.lock([&](cc::optional<tg::vec2i>& size) { size = tg::vec2i(width, height); });
    }

    EMSCRIPTEN_KEEPALIVE void sv_page_animation_frame()
    {
        auto settled = cc::exchange(g_animation_frame, nullptr);
        if (settled != nullptr)
            settled->push_value({});
    }
}

EM_JS_DEPS(sv_page, "$UTF8ToString");

EM_JS(int, sv_page_has_document, (), { return typeof document == "undefined" ? 0 : 1; });

EM_JS(int, sv_page_canvas_width, (), {
    var canvas = document.querySelector("#canvas");
    return canvas ? canvas.width : 0;
});

EM_JS(int, sv_page_canvas_height, (), {
    var canvas = document.querySelector("#canvas");
    return canvas ? canvas.height : 0;
});

EM_JS(void, sv_page_request_animation_frame, (), { requestAnimationFrame(function() { _sv_page_animation_frame(); }); });

EM_JS(void, sv_page_status, (char const* text), {
    var label = document.getElementById("loading-text");
    if (label)
        label.textContent = UTF8ToString(text);
});

EM_JS(void, sv_page_loaded, (), {
    var overlay = document.getElementById("loading");
    if (overlay)
        overlay.style.display = "none";
});

// The canvas's pointer, wheel and key events, translated to sr's input as a window system would report them.
// Pointer events cover a mouse, a pen and one finger alike; a pinch is not translated yet.
EM_JS(void, sv_page_install_input, (), {
    var canvas = document.querySelector("#canvas");
    if (!canvas)
        return;
    canvas.tabIndex = 0;
    var where = function(e)
    {
        var r = canvas.getBoundingClientRect();
        var density = canvas.width / Math.max(r.width, 1);
        return {x : (e.clientX - r.left) * density, y : (e.clientY - r.top) * density, density : density};
    };
    var modifiers = function(e)
    {
        return (e.shiftKey ? 1 : 0) | (e.ctrlKey ? 2 : 0) | (e.altKey ? 4 : 0) | (e.metaKey ? 8 : 0);
    };
    // The DOM's buttons mask holds left, right, middle; sr's holds left, middle, right.
    var held = function(b)
    {
        return (b & 1) | ((b & 2) << 1) | ((b & 4) >> 1) | (b & 24);
    };
    canvas.addEventListener(
        "pointerdown", function(e) {
            canvas.setPointerCapture(e.pointerId);
            canvas.focus();
            var p = where(e);
            _sv_page_mouse_button(e.button, 1, p.x, p.y, modifiers(e), Math.max(e.detail, 1));
            e.preventDefault();
        });
    canvas.addEventListener(
        "pointerup", function(e) {
            var p = where(e);
            _sv_page_mouse_button(e.button, 0, p.x, p.y, modifiers(e), Math.max(e.detail, 1));
            e.preventDefault();
        });
    canvas.addEventListener(
        "pointermove", function(e) {
            var p = where(e);
            _sv_page_mouse_move(p.x, p.y, e.movementX * p.density, e.movementY * p.density, p.density, held(e.buttons));
        });
    canvas.addEventListener("wheel",
                            function(e) {
                                // A tick is about 100 pixels or 3 lines; sr's positive y is away from the user, the DOM's toward.
                                var per_tick = e.deltaMode == 0 ? 100 : (e.deltaMode == 1 ? 3 : 1);
                                var p = where(e);
                                _sv_page_mouse_wheel(e.deltaX / per_tick, -e.deltaY / per_tick, p.x, p.y);
                                e.preventDefault();
                            },
                            {passive : false});
    canvas.addEventListener("contextmenu", function(e) { e.preventDefault(); });

    // sr::scancode's order, by KeyboardEvent.code.
    var codes = {};
    for (var i = 0; i < 26; ++i)
        codes["Key" + String.fromCharCode(65 + i)] = 1 + i;
    for (var d = 0; d < 10; ++d)
        codes["Digit" + d] = 27 + d;
    var named = [
        "Enter",       "Escape",       "Backspace", "Tab",         "Space",        "Insert",     "Delete",
        "Home",        "End",          "PageUp",    "PageDown",    "ArrowLeft",    "ArrowRight", "ArrowUp",
        "ArrowDown",   "Minus",        "Equal",     "BracketLeft", "BracketRight", "Backslash",  "Semicolon",
        "Quote",       "Backquote",    "Comma",     "Period",      "Slash",        "ShiftLeft",  "ShiftRight",
        "ControlLeft", "ControlRight", "AltLeft",   "AltRight",    "MetaLeft",     "MetaRight"
    ];
    for (var n = 0; n < named.length; ++n)
        codes[named[n]] = 37 + n;
    var key = function(e, down)
    {
        var scancode = codes[e.code] || 0;
        var character = e.key.length == 1 ? e.key.codePointAt(0) : 0;
        _sv_page_key(scancode, character, down, e.repeat ? 1 : 0, modifiers(e));
        if (scancode != 0)
            e.preventDefault();
    };
    var toggle_fullscreen = function()
    {
        if (document.fullscreenElement)
            document.exitFullscreen();
        else
            (canvas.parentElement || canvas).requestFullscreen();
    };
    canvas.addEventListener(
        "keydown", function(e) {
            if (e.code == "KeyF" && !e.repeat && !e.ctrlKey && !e.altKey && !e.metaKey)
            {
                toggle_fullscreen();
                e.preventDefault();
                return;
            }
            key(e, 1);
        });
    canvas.addEventListener("keyup", function(e) { key(e, 0); });
    // A key held while the canvas loses focus never sends its release, so a flying camera would keep going.
    canvas.addEventListener(
        "blur", function() {
            for (var c in codes)
                _sv_page_key(codes[c], 0, 0, 0, 0);
        });
    var button = document.getElementById("fullscreen");
    if (button)
        button.addEventListener(
            "click", function() {
                toggle_fullscreen();
                canvas.focus();
            });

    // The drawing buffer follows the canvas's size on the page, at the screen's density, so fullscreen is sharp.
    var follow_layout = function()
    {
        var density = Math.min(window.devicePixelRatio || 1, 2);
        var width = Math.max(1, Math.round(canvas.clientWidth * density));
        var height = Math.max(1, Math.round(canvas.clientHeight * density));
        if (width == canvas.width && height == canvas.height)
            return;
        canvas.width = width;
        canvas.height = height;
        _sv_page_resize(width, height);
    };
    new ResizeObserver(follow_layout).observe(canvas);
    follow_layout();
});
#endif

namespace sv::impl
{
#ifdef __EMSCRIPTEN__
bool page_present()
{
    return sv_page_has_document() != 0;
}

void install_page_input()
{
    if (g_input_installed)
        return;
    g_input_installed = true;
    sv_page_install_input();
}

tg::vec2i page_canvas_size()
{
    return tg::vec2i(sv_page_canvas_width(), sv_page_canvas_height());
}

cc::vector<sr::input_event> take_page_input()
{
    return g_events.lock([](cc::vector<sr::input_event>& events) { return cc::exchange(events, {}); });
}

cc::optional<tg::vec2i> take_page_resize()
{
    return g_resized.lock([](cc::optional<tg::vec2i>& size) { return cc::exchange(size, {}); });
}

cc::shared_async<cc::unit> next_animation_frame()
{
    g_animation_frame = cc::make_async_manual<cc::unit>();
    auto waiting = g_animation_frame;
    sv_page_request_animation_frame();
    return waiting;
}

void set_page_status(cc::string_view text)
{
    sv_page_status(cc::string(text).c_str_materialize());
}

void set_page_loaded()
{
    sv_page_loaded();
}
#else
bool page_present()
{
    return false;
}

void install_page_input()
{
}

tg::vec2i page_canvas_size()
{
    return tg::vec2i(0, 0);
}

cc::vector<sr::input_event> take_page_input()
{
    return {};
}

cc::optional<tg::vec2i> take_page_resize()
{
    return {};
}

cc::shared_async<cc::unit> next_animation_frame()
{
    return cc::make_async_from_value(cc::unit{});
}

void set_page_status(cc::string_view)
{
}

void set_page_loaded()
{
}
#endif
} // namespace sv::impl
