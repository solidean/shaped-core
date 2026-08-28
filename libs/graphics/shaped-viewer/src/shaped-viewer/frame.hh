#pragma once

#include <clean-core/common/utility.hh> // cc::move, for the frame a frame_scope adopts
#include <clean-core/container/set.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/impl/view_state.hh>
#include <shaped-viewer/layout/layout_tree.hh>
#include <shaped-viewer/refs.hh>
#include <shaped-viewer/view/layer.hh>
#include <shaped-viewer/view/view_data.hh>
#include <typed-geometry/linalg/vec.hh>

/// An id-stack scope opened on a frame: view names created while it is alive are derived under it.
///
/// This is what lets the same name be used more than once — `scoped_id(i)` in a loop gives each iteration its own view.
/// It is deliberately independent of the layout: moving a view between containers must not change its id, or relayout
/// would throw away the camera and the accumulated image the id is there to keep.
///
/// `frame::scoped_id` is the only way to get one; `push_id` / `pop_id` are the explicit pair for a caller whose
/// scopes do not nest the way C++ scopes do.
class sv::id_scope
{
public:
    id_scope(id_scope&& o) noexcept;
    id_scope& operator=(id_scope&& o) noexcept;
    id_scope(id_scope const&) = delete;
    id_scope& operator=(id_scope const&) = delete;
    ~id_scope();

    /// Pops early; idempotent, and the destructor calls it if you have not.
    void end();

private:
    friend class frame;
    explicit id_scope(frame* f) : _frame(f), _live(true) {}

    frame* _frame = nullptr;
    bool _live = false;
};

/// One frame of the viewer: obtained by iterating `sv::interactive(...)` (or `viewer::frames()`), or from
/// `viewer::begin_frame()` directly, and authored through the handles below.
///
/// A frame inherits the whole window surface, which in turn inherits the whole view surface, so the shorthand and the
/// long form are the same call:
///
///     for (auto f : sv::interactive("my viewer"))
///     {
///         auto r = f.window().view().layout_rows();
///         auto s = r.add_view("left").add_scene();
///         s.add_light(...);
///         s.add_mesh(mesh);
///
///         f.add_scene().add_mesh(mesh); // the default window's default view's 3D scene
///     }
///
/// Move-only, and the authoring surface alone: a frame never presents itself.
/// What ends it is the type that holds it — `frame_scope` when it falls out of scope, `viewer::end_frame` when the
/// caller says so — so each loop has exactly one rule and the frame in the middle is the same object either way.
/// A "closed" frame — `is_open()` false, returned while the window is minimized — does nothing.
class sv::frame : public window_api<frame>
{
public:
    frame(frame&& o) noexcept;
    frame& operator=(frame&& o) noexcept;
    frame(frame const&) = delete;
    frame& operator=(frame const&) = delete;
    ~frame() = default;

    /// Whether this frame is drawable.
    /// A closed frame (minimized window) should be skipped.
    [[nodiscard]] bool is_open() const { return _open; }

    /// Backbuffer size in pixels — the area the default window fills.
    [[nodiscard]] tg::vec2i viewport_size() const { return _size; }

    /// Monotonic frame counter, increasing once per drawn frame.
    [[nodiscard]] u64 id() const { return _id; }

    /// How many resources still owe post-load work — mip generation and its kin, drained under a per-epoch budget.
    ///
    /// This is the half of "is the image finished" that accumulation cannot see.
    /// Such work changes a texture's contents rather than its id, so it never restarts a view's accumulation:
    /// a caller waiting for a settled image has to watch this AND `view_ref::accumulated_frames`.
    [[nodiscard]] isize pending_resource_work() const;

    /// Seconds since the frame loop started, sampled once when this frame was acquired.
    /// Every view in the frame animates off that one instant, so they cannot drift apart within a frame.
    [[nodiscard]] double seconds() const { return _seconds; }

    /// Seconds since the previous drawn frame; 0 for the first.
    /// A frame the window could not draw is not a previous frame, so resuming from a minimized window reports the whole idle gap here — clamp it before integrating anything.
    [[nodiscard]] double delta_seconds() const { return _delta_seconds; }

    /// The default window, created on first use.
    [[nodiscard]] window_ref window();

    /// A window by name.
    /// Only the default window is presented today; see libs/graphics/shaped-viewer/docs/TODO.md for what a second one still needs.
    [[nodiscard]] window_ref window(cc::string_view id);

    template <class Arg0, class... Args>
    [[nodiscard]] window_ref window(cc::format_string<std::type_identity_t<Arg0>, std::type_identity_t<Args>...> fmt,
                                    Arg0&& arg0,
                                    Args&&... args)
    {
        return window(cc::format(fmt, cc::forward<Arg0>(arg0), cc::forward<Args>(args)...));
    }

    /// The default window's view — what `window_api` forwards every view call to.
    [[nodiscard]] view_ref default_view();

    /// Pushes an id-stack scope: view names created until the matching `pop_id` derive from it.
    /// Every push needs its pop; `scoped_id` is the form that cannot be forgotten.
    void push_id(cc::string_view id);
    void push_id(int id);

    template <class Arg0, class... Args>
    void push_id(cc::format_string<std::type_identity_t<Arg0>, std::type_identity_t<Args>...> fmt,
                 Arg0&& arg0,
                 Args&&... args)
    {
        push_id(cc::format(fmt, cc::forward<Arg0>(arg0), cc::forward<Args>(args)...));
    }

    /// Pops the innermost id-stack scope, which must exist.
    void pop_id();

    /// The same push, undone by the returned scope — through `end()`, or when it falls out of scope.
    [[nodiscard]] id_scope scoped_id(cc::string_view id);
    [[nodiscard]] id_scope scoped_id(int id);

    template <class Arg0, class... Args>
    [[nodiscard]] id_scope scoped_id(cc::format_string<std::type_identity_t<Arg0>, std::type_identity_t<Args>...> fmt,
                                     Arg0&& arg0,
                                     Args&&... args)
    {
        return scoped_id(cc::format(fmt, cc::forward<Arg0>(arg0), cc::forward<Args>(args)...));
    }

    /// The seed view names currently derive from — for a caller minting a view_id by hand.
    [[nodiscard]] u64 id_seed() const { return _id_seed; }

    /// Declares a named capture: a setup this frame can be asked for by name, instead of the view as the body leaves it.
    ///
    ///     f.register_capture("front", [&](sv::capture_context const&) { view.camera(front_camera); });
    ///
    /// `body` runs INLINE, right here in the frame body, on every frame of the run — and only when this capture is the
    /// one being taken (`SC_CAPTURE_NAME`). So it may simply force what it wants, and whatever the body writes after it
    /// still wins, which is visible in the source rather than in a rule.
    /// On an interactive run nothing is taken, so no callback ever runs.
    ///
    /// **`body` must be idempotent after its first frame.**
    /// Any change to what the image depends on restarts the accumulation, so a callback writing a slightly different
    /// value every frame never converges: the run spends its whole timeout and then fails.
    /// `capture_context::first_frame` is where one-shot setup goes.
    ///
    /// Re-registering the same name is how this is meant to be called — every frame, from the same place.
    void register_capture(cc::string_view name, cc::function_ref<void(capture_context const&)> body);

    /// Flattens the frame into a render plan, records it and presents.
    /// Idempotent, and a no-op on a closed frame.
    void present();

private:
    friend class viewer;
    friend class frame_scope;
    friend class frame_iterator;
    friend class id_scope;
    friend class view_ref;
    friend class window_ref;
    friend class layout_ref;
    friend class leaf_ref;
    friend class scene_ref;
    friend class mesh_ref;
    friend class light_ref;

    frame() = default; // a closed frame

    /// The GPU resources this viewer's views draw from — where a mesh and its materials are uploaded.
    ///
    /// Internal, because acquiring is not the caller's job: `scene_ref::add_mesh` does it from the content hashes an
    /// `sv::mesh` already carries, which is what keeps a per-frame add O(1).
    [[nodiscard]] gpu_resource_manager& resources();

    /// The window named `id`, appending it (and the root view it presents) on first use.
    [[nodiscard]] u32 ensure_window(cc::string_view id);

    /// Appends a view named `id` under the current id-stack seed, and returns its slot.
    /// A duplicate id within one frame is a caller error: two views would fight over one camera and one accumulator.
    [[nodiscard]] view_index add_view(cc::string_view id);

    /// The persistent state of the view at `view`, created on first use — what the seeding setters write.
    [[nodiscard]] impl::view_state& state_of(view_index view);

    viewer* _viewer = nullptr;
    tg::vec2i _size = tg::vec2i(0, 0);
    u64 _id = 0;
    double _seconds = 0.0;
    double _delta_seconds = 0.0;
    bool _open = false;
    bool _presented = false;

    cc::vector<view_data> _views;    ///< every view authored this frame; a leaf names them by index
    layout_tree _nodes;              ///< the shared node pool every layout layer indexes into
    cc::vector<view_index> _windows; ///< each window's root view
    cc::vector<view_id> _window_ids;
    cc::set<view_id> _seen_ids; ///< ids authored this frame, so a duplicate is caught rather than aliased
    u64 _id_seed = 0;           ///< the id-stack seed view names currently derive from
    cc::vector<u64> _id_stack;  ///< the seeds pushed above it, innermost last — what pop_id restores
};

/// A frame that presents when it falls out of scope — what `viewer::frames()` yields.
///
/// It *is* a frame, so it authors identically; the scope adds nothing but the ending, which is why the loop body needs
/// no present call of its own.
/// Nothing holds a `frame*` to one, so the non-virtual destructor it inherits is not a hazard.
class sv::frame_scope final : public frame
{
public:
    explicit frame_scope(frame&& f) : frame(cc::move(f)) {}

    frame_scope(frame_scope&&) noexcept = default;
    frame_scope& operator=(frame_scope&& o) noexcept;
    ~frame_scope() { present(); }

private:
    friend class frame_iterator;

    frame_scope() = default; ///< a closed scope, holding a frame that presents nothing
};

/// Sentinel end of a frame range.
struct sv::frame_sentinel
{
};

/// Iterator driving a viewer's frame loop: each step polls, acquires the next drawable frame and yields it by
/// move, so `for (auto frame : viewer.frames())` owns the frame and presents it at the end of the body.
class sv::frame_iterator
{
public:
    explicit frame_iterator(viewer* v);

    [[nodiscard]] frame_scope&& operator*() { return cc::move(_current); }
    frame_iterator& operator++();
    [[nodiscard]] bool operator!=(frame_sentinel) const { return _active; }

private:
    void advance();

    viewer* _viewer = nullptr;
    frame_scope _current;
    bool _active = false;
};

/// The range returned by `viewer::frames()`: a begin iterator that runs the loop and a sentinel end.
///
/// It may also *own* the viewer — that is what `sv::interactive` returns, so a caller needs no variable for it and
/// the viewer is torn down when the loop ends.
///
/// Either way the range owns the *loop*, so leaving it closes the window: a `break` out of the body would otherwise
/// leave a window up that nothing polls any more, which the desktop shows as a hung application.
/// A caller who wants the viewer to outlive the loop drives it with `begin_frame` / `end_frame` instead.
class sv::frame_range
{
public:
    /// Borrows a viewer someone else owns — what `viewer::frames()` hands back.
    explicit frame_range(viewer* v) : _viewer(v) {}

    /// Takes ownership of `v`, destroying it when the loop ends — what `sv::interactive` hands back.
    /// A raw pointer plus a flag rather than a unique_ptr member, so `frame.hh` does not need `viewer` complete.
    [[nodiscard]] static frame_range owning(viewer* v);

    frame_range(frame_range&&) noexcept;
    frame_range& operator=(frame_range&&) noexcept;
    frame_range(frame_range const&) = delete;
    frame_range& operator=(frame_range const&) = delete;
    ~frame_range();

    [[nodiscard]] frame_iterator begin() { return frame_iterator(_viewer); }
    [[nodiscard]] frame_sentinel end() const { return {}; }

private:
    /// Closes the window the loop ran on, and destroys the viewer when this range owns it.
    void end_loop();

    viewer* _viewer = nullptr;
    bool _owns = false;
};
