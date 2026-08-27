#pragma once

#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-rendering/fwd.hh>
#include <typed-geometry/linalg/vec.hh>

/// The capture protocol: how a tool asks a program that renders to produce an image instead of a window.
///
/// It lives here, rather than in whatever draws, because two very different programs answer it.
/// `sv::interactive` implements it for a caller who wrote no capture code at all, and an application owning its own
/// loop implements it itself — `examples/vdoc/cube-editor` is the worked example of the second.
/// What they share is this vocabulary and the readback below.
///
/// Environment rather than API because the program being driven is one `dev.py` did not write, and neither nexus nor
/// the program is in the conversation — the same reason `sr::background_request_env_var` exists.
/// Naming the variables here, in one place, is what lets a real API replace them later without hunting for `getenv`.
///
/// **Which programs are asked is not decided here.** A `.capture.json` beside an example declares that, so a program
/// that ignores these variables is never launched with them set; see docs/guides/examples.md.

namespace sr
{
/// Turns capture mode on.
/// It means headless: whatever the program would have shown, it must render without a window and without presenting.
inline constexpr cc::string_view capture_request_env_var = "SC_CAPTURE";

/// Which named capture to take, for a program offering more than one.
/// Unset means whatever the program shows by default.
inline constexpr cc::string_view capture_name_env_var = "SC_CAPTURE_NAME";

/// Where to write the image.
/// The file's extension picks the format — `.jpg` or `.png`.
inline constexpr cc::string_view capture_output_env_var = "SC_CAPTURE_OUT";

/// The offscreen resolution, as `<width>x<height>`.
inline constexpr cc::string_view capture_size_env_var = "SC_CAPTURE_SIZE";

/// How many frames the program should be satisfied for before it captures.
///
/// What "satisfied" means is the program's own: a path tracer counts accumulated samples, a raster app counts frames drawn.
/// Either way it is the knob a scene needing longer reaches for.
inline constexpr cc::string_view capture_accumulate_env_var = "SC_CAPTURE_ACCUMULATE";

/// How long a capture may take before it gives up, in seconds.
inline constexpr cc::string_view capture_timeout_env_var = "SC_CAPTURE_TIMEOUT";
} // namespace sr

/// What the environment asked a capture run for, read once at start-up.
struct sr::capture_request
{
    /// Whether a capture was asked for at all.
    /// Everything below is meaningless when this is false.
    bool active = false;

    /// The named capture to take; empty means the program's default view.
    ///
    /// A name the program does not offer must be an ERROR rather than a quiet fall back to that default view.
    /// Nothing discovers these names, so a renamed capture would otherwise keep producing an image under the old
    /// name's filename, and nobody would look.
    cc::string name;

    /// Where the image goes.
    /// Empty with `active` set is an error the run reports rather than a silent no-op.
    cc::string output_path;

    tg::vec2i size = tg::vec2i(1920, 1080);

    /// Frames to be satisfied for before capturing; see `capture_accumulate_env_var`.
    u32 accumulate_frames = 60;

    /// The wall clock a capture may spend before it gives up and writes what it has.
    double timeout_seconds = 60.0;

    /// Reads every variable above, applying the defaults for the ones that are unset.
    [[nodiscard]] static capture_request from_environment();
};

namespace sr
{
/// Reads `texture` back, converts it to RGB and writes it to `path`, picking the format from the extension.
///
/// Blocking: it waits on the download, which is what a capture wants — the run is over either way.
/// The texture must carry `copy_src` usage and be `bgra8_unorm`, which is what a capture target is.
[[nodiscard]] cc::result<cc::unit> write_capture_image(sg::context& ctx,
                                                       sg::texture_2d const& texture,
                                                       tg::vec2i size,
                                                       cc::string_view path);
} // namespace sr
