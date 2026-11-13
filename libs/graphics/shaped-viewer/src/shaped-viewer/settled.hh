#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-viewer/fwd.hh>
#include <typed-geometry/linalg/vec.hh>

// A render that runs until its image settles and hands it back: the viewer's frame loop, kept inside one async.
// It is the capture a headless `sv::interactive` takes, with the image returned rather than written to a file,
// so a tool that renders as part of its own work, such as a bake, needs no loop and no file.

/// What a settled render waits for.
struct sv::settled_options
{
    /// The image's size in pixels.
    tg::vec2i size = tg::vec2i(1024, 1024);

    /// The samples per pixel every traced layer accumulates before the image counts as settled.
    u32 samples = 64;

    /// How long it may take before it hands back the image it has, unsettled.
    f64 timeout_seconds = 60.0;

    /// The home of the host whose frames this renders in, as a guest: it never advances an epoch, and paces on its own work.
    /// The host runs that home between its frames, before it advances; null renders as an app of its own, advancing epochs.
    cc::async_scheduler* host = nullptr;
};

/// One channel of a settled render: its mean at every pixel, rows from the top.
struct sv::settled_channel
{
    render_channel channel = {};
    cc::vector<tg::vec4f> values;
};

/// The finished image of a settled render.
struct sv::settled_image
{
    tg::vec2i size = tg::vec2i(0, 0);

    /// The composited image as a display shows it: 8 bits each of red, green, blue and alpha, sRGB, rows from the top.
    cc::vector<u8> rgba;

    /// The channels the first traced layer averaged beside its image (`render_settings::channels`), in the order of their bits.
    cc::vector<settled_channel> channels;

    /// False where the time ran out first and this is the image it had: a layer short of its samples, or a payload streaming.
    bool settled = false;
};

namespace sv
{
/// Renders what `author` draws into a headless viewer named `id` until the image settles, and resolves to it.
///
/// `author` is the body an interactive loop runs, called once a frame: it adds the views, the scenes and their cameras,
/// so the same body draws the same image here and in a window.
/// It settles as a capture does: no payload still streaming, every traced layer at `options.samples`, exposure read.
/// The viewer lives for this async alone, and nothing in it blocks: it awaits each frame's pacing and its readbacks.
/// A guest of `options.host` runs on that home; otherwise it runs on the context's device home where it has one.
/// A viewer that cannot come up, or a loop that ends before its image does, fails the async.
[[nodiscard]] cc::shared_async<settled_image> render_settled(cc::string id,
                                                             settled_options options,
                                                             cc::unique_function<void(frame&)> author);

/// The same, on a context the caller owns and keeps alive until the async resolves.
[[nodiscard]] cc::shared_async<settled_image> render_settled(sg::context& ctx,
                                                             cc::string id,
                                                             settled_options options,
                                                             cc::unique_function<void(frame&)> author);
} // namespace sv
