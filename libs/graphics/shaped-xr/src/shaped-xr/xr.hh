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
enum class sxr::graphics_api : sxr::u8
{
    vulkan,
    dx12,
};

/// What an application can ask of a runtime beyond rendering.
/// Each needs extensions the instance must be created with, so it is named in `system_description::wants`.
enum class sxr::capability : sxr::u8
{
    passthrough,
    environment_depth,
    scene_planes,
    scene_meshes,
    anchors,
    persistent_anchors,
    hit_tests,
};

/// Whether a session may use a capability now.
/// `needs_permission` and `denied` are the user's answer on a headset where one is asked, which no build reaches yet.
enum class sxr::availability : sxr::u8
{
    unsupported, ///< the runtime or headset lacks it, or `system_description::wants` did not name it
    needs_permission,
    denied,
    available,
};

/// How a frame meets the room behind it.
enum class sxr::blend_mode : sxr::u8
{
    opaque,
    /// The room shows through by the frame's alpha, which is premultiplied, so a frame clears to transparent.
    alpha_blend,
    /// The frame adds light to the room, the only thing an optical see-through display can do; black is invisible.
    additive,
};

struct sxr::system_description
{
    cc::string application_name = "shaped-core";
    graphics_api api = graphics_api::vulkan;
    /// What sessions of this system may use; what the runtime lacks is left out rather than failing, see `system::offers`.
    cc::vector<capability> wants;
};

/// What an action reports: a button, an axis, a stick, where something is, or a vibration it makes.
enum class sxr::action_kind : sxr::u8
{
    boolean,
    scalar,
    vector2,
    pose,
    vibration,
};

/// Which hand an action is read for; an action declared for both is one action with a state per hand.
enum class sxr::hand : sxr::u8
{
    left,
    right,
};

/// One suggested binding: an input or output path of an interaction profile, such as
/// `/interaction_profiles/oculus/touch_controller` and `/user/hand/left/input/trigger/value`.
/// The runtime binds what the controllers in hand offer, and remaps a profile it does not have to one it does.
struct sxr::action_binding
{
    cc::string profile;
    cc::string path;
};

/// An action the application reads by name rather than by device path, OpenXR's model.
/// `name` is lowercase letters, digits, `-`, `_` and `.`; `label` is what the runtime shows a user who rebinds it.
struct sxr::action_description
{
    cc::string name;
    cc::string label;
    action_kind kind = action_kind::boolean;
    /// One state per hand, read with `sxr::hand`; every binding then starts with `/user/hand/left` or `/user/hand/right`.
    bool is_per_hand = true;
    cc::vector<action_binding> bindings;
};

struct sxr::session_description
{
    /// The depth range every view's projection maps to 0 and 1, and the one the runtime is told for its depth layer.
    f32 near_distance = 0.05f;
    f32 far_distance = 100.0f;
    /// What the session asks for; one the runtime lacks falls back to opaque with a warning, and `session::blend` says which.
    /// A camera layer needs `capability::passthrough` in `system_description::wants`.
    blend_mode blend = blend_mode::opaque;
    /// Lets `frame.color` be read back, for a test or a mirror, until the frame is submitted.
    /// Off by default, because it asks more of every image the runtime allocates.
    bool readable_color = false;
    /// The actions the session reads, bound before it begins; `sxr::standard_controller_actions()` is the usual set.
    cc::vector<action_description> actions;
};

namespace sxr
{
/// Both hands' grip and aim poses, trigger, squeeze, thumbstick, the two face buttons, the thumbstick click, the menu
/// button and haptics, named so, bound for Quest Touch and for OpenXR's simple controller.
/// The face buttons are `primary` (A or X) and `secondary` (B or Y); the menu button is the left controller's alone.
[[nodiscard]] cc::vector<action_description> standard_controller_actions();
} // namespace sxr

/// One eye: where it is and how it projects, in tg's frame — left-handed, +y up, +z forward — of the session's space.
/// OpenXR's frame is right-handed with -z forward, and the session converts at its edge so nothing above sees that.
struct sxr::view
{
    tg::rigid_transform3f camera_to_world;
    /// Left-handed, depth 0 at the session's near distance and 1 at its far one, as a depth cleared to 1 tests less.
    tg::projective_transform3f projection;

    /// World to clip space: the projection after the inverse of the eye's pose.
    [[nodiscard]] tg::mat4f world_to_clip() const;
};

/// One frame: its views, and the images each view renders into, view i into layer i.
/// Render both views in one pass with view instancing (`view_count = 2`, SGL's `@view_index`), or one pass a layer.
struct sxr::frame
{
    /// False where the runtime wants nothing drawn this frame, as the first ones of a session do: `views` is empty and
    /// the images are invalid, and the frame still ends, through `session::submit_frame(nullptr)`.
    bool should_render = false;
    cc::vector<view> views;
    int width = 0;
    int height = 0;
    /// `views.size()` layers each, with `render_target` and `depth_stencil` usage; what `submit_frame` hands the runtime.
    /// `color` also has `copy_src` where `session_description::readable_color` asked for it.
    sg::texture_2d_array color;
    sg::texture_2d_array depth;
};

/// The OpenXR runtime and the headset it found: what a session needs, and the sg context it must render on.
///
/// The runtime dictates the GPU, and on vulkan the instance and device extensions, so the context comes from here:
/// `create_context` creates it to the runtime's spec, on the backend `graphics_api` names.
/// A system outlives every session created from it, and a process has one at a time, since the loader holds one instance.
class sxr::system
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
    /// Whether the description wanted `c`, the runtime lists all it needs, and the headset says it has it.
    /// A runtime may still refuse a call, as a toggle in the Link app makes it do.
    [[nodiscard]] bool offers(capability c) const;
    /// Whether a session asking for `b` gets it, so far as can be told before one exists.
    [[nodiscard]] bool offers_blend(blend_mode b) const;

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
class sxr::session
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
    /// How this session's frames meet the room, which is opaque where what was asked for did not come up.
    [[nodiscard]] blend_mode blend() const;
    /// Whether this session may use `c` now, which unlike `system::offers` may depend on what the user allowed.
    [[nodiscard]] availability availability_of(capability c) const;

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
