#pragma once

#include <clean-core/string/string_view.hh>
#include <shaped-rendering/capture.hh> // the protocol itself: the env vars, the request, the readback
#include <shaped-viewer/fwd.hh>
#include <typed-geometry/linalg/vec.hh>

/// sv's half of the capture protocol.
///
/// The protocol is `sr::capture_request` and lives one layer down, because a program that owns its own loop answers
/// the same variables without any of sv — `examples/vdoc/cube-editor` does exactly that.
/// What sv adds is that a caller needs to do NOTHING: `sv::interactive` reads the request, brings the viewer up
/// headless, waits for the image to settle, writes it, and ends the loop.
/// So a body written for the interactive case is also the body that produces a reference image.
///
/// **Only `sv::interactive` installs a capture.** A caller driving `viewer::begin_frame` / `end_frame` never gets one,
/// whatever the environment says, so an application embedding the viewer cannot have its loop ended out from under it.

/// What a registered capture's callback is handed.
///
/// The callback runs inline, where it was declared in the frame body, on every frame of the capture — so it may simply
/// force whatever it wants, and whatever the body writes after it still wins.
///
/// **It must be idempotent after the first frame.**
/// Any change to what the image depends on restarts the accumulation, so a callback writing a slightly different value
/// every frame never converges: the run burns its whole timeout and then fails.
struct sv::capture_context
{
    /// True on the first frame this capture is applied to, for one-shot setup.
    bool first_frame = false;

    /// The name this capture was registered under.
    cc::string_view name;

    /// The size of the image being captured, in pixels.
    tg::vec2i size = tg::vec2i(0, 0);
};
