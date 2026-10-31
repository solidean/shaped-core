#pragma once

#include <babel-serializer/image/image.hh> // babel::image::write_options, taken by value
#include <clean-core/common/time.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics/bytes_future.hh>
#include <shaped-graphics/resource/texture.hh> // sg::texture_2d, which sg's fwd.hh does not name
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

/// How much the program should accumulate before it captures, in the program's own unit.
///
/// What the unit is belongs to the program: a path tracer may count samples per pixel, a raster app frames drawn.
/// Either way it is the knob a scene needing longer reaches for.
inline constexpr cc::string_view capture_accumulate_env_var = "SC_CAPTURE_ACCUMULATE";

/// How many frames the program renders into each image, in place of `SC_CAPTURE_ACCUMULATE`'s quality bar.
///
/// A count the program keeps as it records, so an image settles without waiting on anything the GPU reports back.
/// The image is then a function of this count alone, on any machine; unset or 0 keeps the `accumulate` rule.
inline constexpr cc::string_view capture_iterations_env_var = "SC_CAPTURE_ITERATIONS";

/// How long a capture may take before it gives up, in seconds; for a sequence, how long each of its frames may take.
inline constexpr cc::string_view capture_timeout_env_var = "SC_CAPTURE_TIMEOUT";

/// How many frames to capture as a sequence; unset or 0 takes a single still.
///
/// A sequence writes each frame to `SC_CAPTURE_OUT` with the frame's index substituted, see `capture_frame_path`.
/// Each frame is finished exactly as a still is, by the program's own `accumulate` rule, before the clock moves on.
inline constexpr cc::string_view capture_frames_env_var = "SC_CAPTURE_FRAMES";

/// The frame rate of a sequence: frame `i` shows the program's clock at `SC_CAPTURE_TIME + i / fps`.
inline constexpr cc::string_view capture_fps_env_var = "SC_CAPTURE_FPS";

/// The program's clock at the first frame, in seconds; a still shows this instant.
///
/// A capture never reads the wall clock for what it shows, so the same request renders the same instant on any machine.
inline constexpr cc::string_view capture_time_env_var = "SC_CAPTURE_TIME";

/// Writes only every n-th frame of a sequence, for looking at a long one quickly.
///
/// Every frame is still rendered, so a temporal denoiser sees the same history and a written frame matches the full sequence's.
inline constexpr cc::string_view capture_stride_env_var = "SC_CAPTURE_STRIDE";
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

    /// What to accumulate before capturing, in the program's own unit; see `capture_accumulate_env_var`.
    u32 accumulate = 60;

    /// Frames rendered into each image, replacing `accumulate` when above 0; see `capture_iterations_env_var`.
    u32 iterations = 0;

    /// The time a capture may spend before it gives up and writes what it has, measured on `clock_seconds`.
    /// A sequence spends it per frame, starting over at each.
    double timeout_seconds = 60.0;

    /// How many frames to capture; 0 is a single still, written to `output_path` as it stands.
    i32 frames = 0;

    /// The sequence's frame rate, which sets the program clock's step from one frame to the next.
    double fps = 30.0;

    /// The program clock at the first frame, or of the still.
    double start_seconds = 0.0;

    /// Writes frame `i` of a sequence only when `i` is a multiple of this; must be >= 1.
    i32 stride = 1;

    /// The clock `timeout_seconds` is spent against, in seconds; only a difference of two readings means anything.
    ///
    /// The environment cannot name one, so `from_environment` leaves the steady clock.
    /// A test substitutes a clock it advances itself, which is how a timeout is reached without being waited out.
    double (*clock_seconds)() = cc::current_time_steady_secs;

    /// Reads every variable above, applying the defaults for the ones that are unset.
    [[nodiscard]] static capture_request from_environment();

    [[nodiscard]] bool is_sequence() const { return frames > 0; }

    /// The program clock at frame `index`.
    [[nodiscard]] double seconds_at(i32 index) const { return start_seconds + double(index) / fps; }
};

namespace sr
{
/// Where frame `index` of a sequence goes: `pattern` with its `%d` or `%0Nd` replaced by the index, as ffmpeg reads a sequence.
///
/// A pattern without one gets `_%05d` before its extension, so a still's path still names a sequence that sorts.
[[nodiscard]] cc::string capture_frame_path(cc::string_view pattern, i32 index);

/// Reads `texture` back, converts it to RGB and writes it to `path`, picking the format from the extension.
///
/// The image's extent is the texture's own, so a caller cannot disagree with it.
/// An earlier signature took the size separately, and the only guard caught a size larger than the texture — a
/// smaller one walked the readback at the wrong stride and produced a sheared image that read as a rendering bug.
///
/// Awaits the download rather than blocking for it, so it serves a context that never blocks as well.
/// The texture must carry `copy_src` usage and must be `bgra8_unorm`: the channel swap is written for it.
/// `texture` must outlive the returned async: it is read once the list is recorded, which may be after a hop to the device's home.
/// A loop that renders into `texture` again before awaiting this wants `submit_capture_readback` instead, which reads it at once.
[[nodiscard]] cc::shared_async<cc::result<cc::unit>> write_capture_image_async(sg::context& ctx,
                                                                               sg::texture_2d const& texture,
                                                                               cc::string path);

/// Records and submits the readback of `texture` now, so a frame rendered into it after this call cannot reach the image.
///
/// The caller must be on the thread that records `ctx`'s command lists, as a frame loop is between its frames.
/// The texture must carry `copy_src` usage and must be `bgra8_unorm`: `encode_capture_image_async` swaps channels for it.
[[nodiscard]] sg::bytes_future submit_capture_readback(sg::context& ctx, sg::texture_2d const& texture);

/// Awaits `readback` of an image of `size`, then converts and writes it to `path` on the compute pool.
///
/// Nothing in it touches the device, so several may be in flight while the loop renders on.
/// `opts` is handed to the encoder; the format still comes from the extension.
[[nodiscard]] cc::shared_async<cc::result<cc::unit>> encode_capture_image_async(sg::bytes_future readback,
                                                                                tg::vec2i size,
                                                                                cc::string path,
                                                                                babel::image::write_options opts);
} // namespace sr
