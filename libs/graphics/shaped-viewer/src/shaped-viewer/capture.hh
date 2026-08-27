#pragma once

#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-viewer/fwd.hh>
#include <typed-geometry/linalg/vec.hh>

/// Capturing a viewer run to an image file, driven entirely by the environment.
///
/// The point is that an example costs nothing: `sv::interactive` reads these variables, brings the viewer up headless,
/// waits for the image to settle, writes it, and ends the loop — so a body written for the interactive case is also the
/// body that produces a reference image.
/// `docs/guides/examples.md` is the workflow around it.
///
/// Environment rather than API because the program being driven is one `dev.py` did not write, and neither nexus nor
/// the example is in the conversation — the same reason `sr::background_request_env_var` exists.
/// Naming them here, in one place, is what lets a real API replace them later without hunting for `getenv` calls.
///
/// **Only `sv::interactive` installs a capture.** A caller driving `viewer::begin_frame` / `end_frame` never gets one,
/// whatever the environment says, so an application embedding the viewer cannot have its loop ended out from under it.

namespace sv
{
/// Turns capture mode on.
/// It IMPLIES headless: no window, no swapchain, nothing presented.
inline constexpr cc::string_view capture_request_env_var = "SC_CAPTURE";

/// Which registered capture to take, by the name `frame::register_capture` was given.
/// Unset takes the view as the example's own body leaves it, which is the common case.
inline constexpr cc::string_view capture_name_env_var = "SC_CAPTURE_NAME";

/// Where to write the image.
/// The file's extension picks the format — `.jpg` or `.png`.
inline constexpr cc::string_view capture_output_env_var = "SC_CAPTURE_OUT";

/// The offscreen resolution, as `<width>x<height>`.
inline constexpr cc::string_view capture_size_env_var = "SC_CAPTURE_SIZE";

/// Print every registered capture name, one per line, and exit without writing anything.
/// It runs one frame to find out, since registration happens while a frame is authored.
inline constexpr cc::string_view capture_list_env_var = "SC_CAPTURE_LIST";

/// How many frames every traced view must have accumulated before the image counts as settled.
inline constexpr cc::string_view capture_accumulate_env_var = "SC_CAPTURE_ACCUMULATE";

/// How long a capture may take before it gives up, in seconds.
inline constexpr cc::string_view capture_timeout_env_var = "SC_CAPTURE_TIMEOUT";
} // namespace sv

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

/// What the environment asked a capture run for, read once at `sv::interactive`.
struct sv::capture_request
{
    /// Whether a capture was asked for at all.
    /// Everything below is meaningless when this is false.
    bool active = false;

    /// List the registered names and exit, rather than writing an image.
    bool list_only = false;

    /// The registered capture to take; empty means the default view.
    cc::string name;

    /// Where the image goes.
    /// Empty with `active` set is an error the run reports rather than a silent no-op.
    cc::string output_path;

    tg::vec2i size = tg::vec2i(1920, 1080);

    /// Accumulated frames every traced view must reach.
    u32 accumulate_frames = 60;

    /// The wall clock a capture may spend before it gives up and writes what it has.
    double timeout_seconds = 60.0;

    /// Reads every variable above, applying the defaults for the ones that are unset.
    [[nodiscard]] static capture_request from_environment();
};
