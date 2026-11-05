#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-xr/fwd.hh>
#include <typed-geometry/camera/projection.hh>
#include <typed-geometry/linalg/mat.hh>
#include <typed-geometry/transform/homogeneous_transform.hh>

#include <memory>

/// The binding a session renders through, which decides the sg backend its context must be.
enum class xr::graphics_api : xr::u8
{
    vulkan,
    dx12,
};

struct xr::system_description
{
    cc::string application_name = "shaped-core";
    graphics_api api = graphics_api::vulkan;
    /// Asks for the runtime's passthrough (`XR_FB_passthrough`) where it offers it, so a session may show the room.
    bool wants_passthrough = false;
};

/// What an action reports: a button, an axis, a stick, where something is, or a vibration it makes.
enum class xr::action_kind : xr::u8
{
    boolean,
    scalar,
    vector2,
    pose,
    vibration,
};

/// Which hand an action is read for; an action declared for both is one action with a state per hand.
enum class xr::hand : xr::u8
{
    left,
    right,
};

/// One suggested binding: an input or output path of an interaction profile, such as
/// `/interaction_profiles/oculus/touch_controller` and `/user/hand/left/input/trigger/value`.
/// The runtime binds what the controllers in hand offer, and remaps a profile it does not have to one it does.
struct xr::action_binding
{
    cc::string profile;
    cc::string path;
};

/// An action the application reads by name rather than by device path, OpenXR's model.
/// `name` is lowercase letters, digits, `-`, `_` and `.`; `label` is what the runtime shows a user who rebinds it.
struct xr::action_description
{
    cc::string name;
    cc::string label;
    action_kind kind = action_kind::boolean;
    /// One state per hand, read with `xr::hand`; every binding then starts with `/user/hand/left` or `/user/hand/right`.
    bool is_per_hand = true;
    cc::vector<action_binding> bindings;
};

struct xr::session_description
{
    /// The depth range every view's projection maps to 0 and 1, and the one the runtime is told for its depth layer.
    f32 near_distance = 0.05f;
    f32 far_distance = 100.0f;
    /// Shows the headset's cameras behind what is rendered, where the system asked for passthrough and it came up.
    /// The frame's color is then cleared to transparent where the room shows through.
    bool passthrough = false;
    /// The actions the session reads, bound before it begins; `xr::standard_controller_actions()` is the usual set.
    cc::vector<action_description> actions;
};

namespace xr
{
/// Both hands' grip and aim poses, trigger, squeeze, thumbstick, the two face buttons, the thumbstick click, the menu
/// button and haptics, named so, bound for Quest Touch and for OpenXR's simple controller.
/// The face buttons are `primary` (A or X) and `secondary` (B or Y); the menu button is the left controller's alone.
[[nodiscard]] cc::vector<action_description> standard_controller_actions();
} // namespace xr

/// One eye: where it is and how it projects, in tg's frame — left-handed, +y up, +z forward — of the session's space.
/// OpenXR's frame is right-handed with -z forward, and the session converts at its edge so nothing above sees that.
struct xr::view
{
    tg::rigid_transform3f camera_to_world;
    /// Left-handed, depth 0 at the session's near distance and 1 at its far one, as a depth cleared to 1 tests less.
    tg::projective_transform3f projection;

    /// World to clip space: the projection after the inverse of the eye's pose.
    [[nodiscard]] tg::mat4f world_to_clip() const;
};

/// One frame: its views, and the images each view renders into, view i into layer i.
/// Render both views in one pass with view instancing (`view_count = 2`, SGL's `@view_index`), or one pass a layer.
struct xr::frame
{
    /// False where the runtime wants nothing drawn this frame, which still ends through `session::submit_frame`.
    bool should_render = false;
    cc::vector<view> views;
    int width = 0;
    int height = 0;
    /// `views.size()` layers each, with `render_target` and `depth_stencil` usage; what `submit_frame` hands the runtime.
    sg::texture_2d_array color;
    sg::texture_2d_array depth;
};

/// The OpenXR runtime and the headset it found: what a session needs, and the sg context it must render on.
///
/// The runtime dictates the GPU, and on vulkan the instance and device extensions, so the context comes from here:
/// `create_context` creates it to the runtime's spec, on the backend `graphics_api` names.
/// A system outlives every session created from it.
class xr::system
{
public:
    /// Fails where the build has no OpenXR loader, no runtime is active, the runtime lacks the binding, or no headset is
    /// connected, each named in the error.
    [[nodiscard]] static cc::result<cc::unique_ptr<system>> try_create(system_description const& desc);
    ~system();

    system(system const&) = delete;
    system& operator=(system const&) = delete;

    [[nodiscard]] cc::string_view runtime_name() const;
    [[nodiscard]] cc::string_view system_name() const;
    [[nodiscard]] graphics_api api() const;
    /// Whether the runtime offers passthrough and the description asked for it.
    [[nodiscard]] bool offers_passthrough() const;

    /// An sg context on the adapter the runtime composes on, with what the runtime requires of it.
    [[nodiscard]] cc::result<sg::context_handle> create_context() const;

    /// A session rendering through `ctx`, which must come from `create_context` and outlive the session.
    [[nodiscard]] cc::result<cc::unique_ptr<session>> create_session(sg::context& ctx, session_description const& desc);

    /// What `try_create` makes a system of; `state` is shaped-xr's own, so nothing else can name one.
    struct state;
    explicit system(cc::unique_ptr<state> s);

private:
    cc::unique_ptr<state> _state;
};

/// The runtime's frame loop: its states, its pacing, and the images it composes.
///
/// Every iteration polls events, then, while running, waits for a frame and submits it:
///
///     while (session->poll_events())
///         if (auto f = session->wait_frame())
///             session->submit_frame(render(*f)); // render returns the command list that drew f, or null
///
/// The session begins when the runtime is ready and ends when it stops, as a headset taken off does; it resumes when
/// the runtime is ready again, so `wait_frame` gives nothing in between.
class xr::session
{
public:
    ~session();
    session(session const&) = delete;
    session& operator=(session const&) = delete;

    /// Handles the runtime's events; false once the runtime wants the application to quit.
    [[nodiscard]] bool poll_events();

    /// Whether the runtime is pacing frames, from ready until it stops.
    [[nodiscard]] bool is_running() const;
    /// Whether the headset shows this application and gives it input.
    [[nodiscard]] bool is_focused() const;
    /// Whether the room shows behind what is rendered.
    [[nodiscard]] bool shows_passthrough() const;

    /// Blocks until the runtime paces the next frame, and begins it; none while the session is not running.
    /// A frame that is returned must be ended by `submit_frame`, whether or not it should render.
    [[nodiscard]] cc::optional<frame> wait_frame();

    /// Submits `cmd`, which drew the frame `wait_frame` returned, and hands the frame to the runtime.
    /// `cmd` must be a list of the session's context; null for a frame that did not render.
    void submit_frame(std::unique_ptr<sg::command_list> cmd);

    // ---- actions, synced by every `wait_frame` while the session is focused
    //
    // A name is one of `session_description::actions`; one that is not asserts.
    // An action the controllers in hand do not offer, or one read while unfocused, reads as released, zero or untracked.

    [[nodiscard]] bool pressed(cc::string_view action, hand h = hand::left) const;
    [[nodiscard]] f32 scalar(cc::string_view action, hand h = hand::left) const;
    [[nodiscard]] tg::vec2f vector2(cc::string_view action, hand h = hand::left) const;
    /// Where the pose is at the frame's display time, in tg's frame of the session's space; none while untracked.
    [[nodiscard]] cc::optional<tg::rigid_transform3f> pose(cc::string_view action, hand h = hand::left) const;
    /// Vibrates for `seconds` at `amplitude`, 0 to 1.
    void vibrate(cc::string_view action, hand h, f32 amplitude, f32 seconds);

    /// What `system::create_session` makes a session of; `state` is shaped-xr's own, so nothing else can name one.
    struct state;
    explicit session(cc::unique_ptr<state> s);

private:
    cc::unique_ptr<state> _state;
};
