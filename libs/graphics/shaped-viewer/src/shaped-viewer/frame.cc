#include <clean-core/common/asserts.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <shaped-viewer/frame.hh>
#include <shaped-viewer/viewer.hh> // frame drives the viewer to present, and to run the frame loop

namespace sv
{
// ---- frame -----------------------------------------------------------------------------------------------

frame::frame(frame&& o) noexcept
  : _viewer(o._viewer),
    _size(o._size),
    _id(o._id),
    _seconds(o._seconds),
    _delta_seconds(o._delta_seconds),
    _open(o._open),
    _presented(o._presented),
    _views(cc::move(o._views)),
    _nodes(cc::move(o._nodes)),
    _windows(cc::move(o._windows)),
    _window_ids(cc::move(o._window_ids)),
    _seen_ids(cc::move(o._seen_ids)),
    _id_seed(o._id_seed),
    _id_stack(cc::move(o._id_stack))
{
    // The moved-from frame becomes an inert husk that presents nothing.
    o._viewer = nullptr;
    o._open = false;
    o._presented = true;
}

frame& frame::operator=(frame&& o) noexcept
{
    if (this != &o)
    {
        _viewer = o._viewer;
        _size = o._size;
        _id = o._id;
        _seconds = o._seconds;
        _delta_seconds = o._delta_seconds;
        _open = o._open;
        _presented = o._presented;
        _views = cc::move(o._views);
        _nodes = cc::move(o._nodes);
        _windows = cc::move(o._windows);
        _window_ids = cc::move(o._window_ids);
        _seen_ids = cc::move(o._seen_ids);
        _id_seed = o._id_seed;
        _id_stack = cc::move(o._id_stack);

        o._viewer = nullptr;
        o._open = false;
        o._presented = true;
    }
    return *this;
}


u32 frame::ensure_window(cc::string_view id)
{
    auto const wid = view_id::from_string(id);
    for (auto i = u32(0); i < _window_ids.size(); ++i)
        if (_window_ids[i] == wid)
            return i;

    // A window's root view is the texture it presents, so creating the window creates that view.
    // Its id is the window's, and it is deliberately outside the id stack: a window is not nested in anything.
    CC_ASSERT(!_seen_ids.contains(wid), "a window id collides with a view id authored this frame");
    (void)_seen_ids.insert(wid);

    auto root = view_data{};
    root.id = wid;
    _views.push_back(cc::move(root));

    auto const view = view_index(_views.size() - 1);
    auto& state = state_of(view);
    if (state.display_name.empty())
        state.display_name = display_name_of(id);

    _windows.push_back(view);
    _window_ids.push_back(wid);
    return u32(_windows.size() - 1);
}

window_ref frame::window()
{
    return window(default_window_name);
}

window_ref frame::window(cc::string_view id)
{
    CC_ASSERT(_open, "cannot author a closed frame");
    return window_ref(this, ensure_window(id));
}

view_ref frame::default_view()
{
    return window().default_view();
}

view_index frame::add_view(cc::string_view id)
{
    CC_ASSERT(_open, "cannot author a closed frame");

    auto v = view_data{};
    v.id = view_id::from_string(id, _id_seed);

    CC_ASSERT(!_seen_ids.contains(v.id),
              "duplicate view id in one frame — two views with the same id would fight over one camera and one "
              "accumulator; wrap the body in frame::scoped_id(i), suffix the id with ##i, or give them distinct names");
    (void)_seen_ids.insert(v.id);

    _views.push_back(cc::move(v));
    auto const index = view_index(_views.size() - 1);

    // The id up to its `##` is the name a human reads, unless the caller names it themselves.
    // Re-derived whenever the stored one is empty, so clearing a display name restores the default rather than
    // leaving the view nameless.
    auto& state = state_of(index);
    if (state.display_name.empty())
        state.display_name = display_name_of(id);

    return index;
}

gpu_resource_manager& frame::resources()
{
    CC_ASSERT(_viewer != nullptr, "a closed frame has no viewer to draw resources from");
    return _viewer->resources();
}

impl::view_state& frame::state_of(view_index view)
{
    return _viewer->state_of(_views[u32(view)].id);
}

void frame::push_id(cc::string_view id)
{
    CC_ASSERT(_open, "cannot author a closed frame");
    _id_stack.push_back(_id_seed);
    _id_seed = push_id_seed(_id_seed, id);
}

void frame::push_id(int id)
{
    CC_ASSERT(_open, "cannot author a closed frame");
    _id_stack.push_back(_id_seed);
    _id_seed = push_id_seed(_id_seed, i64(id));
}

void frame::pop_id()
{
    CC_ASSERT(!_id_stack.empty(), "pop_id without a matching push_id");
    _id_seed = _id_stack.back();
    _id_stack.remove_back();
}

id_scope frame::scoped_id(cc::string_view id)
{
    push_id(id);
    return id_scope(this);
}

id_scope frame::scoped_id(int id)
{
    push_id(id);
    return id_scope(this);
}

void frame::present()
{
    if (_viewer == nullptr || !_open || _presented)
        return;
    _presented = true;
    _viewer->finish_frame(*this);
}

// ---- frame_scope -----------------------------------------------------------------------------------------

frame_scope& frame_scope::operator=(frame_scope&& o) noexcept
{
    if (this != &o)
    {
        present(); // finalize whatever we are about to drop — this is the scope, so nothing else will
        frame::operator=(cc::move(o));
    }
    return *this;
}

// ---- id_scope --------------------------------------------------------------------------------------------

id_scope::id_scope(id_scope&& o) noexcept : _frame(o._frame), _live(o._live)
{
    o._frame = nullptr;
    o._live = false;
}

id_scope& id_scope::operator=(id_scope&& o) noexcept
{
    if (this != &o)
    {
        if (_live && _frame != nullptr)
            _frame->pop_id();
        _frame = o._frame;
        _live = o._live;
        o._frame = nullptr;
        o._live = false;
    }
    return *this;
}

id_scope::~id_scope()
{
    if (_live && _frame != nullptr)
        _frame->pop_id();
}

void id_scope::end()
{
    if (_live && _frame != nullptr)
        _frame->pop_id();
    _live = false;
}

// ---- frame_iterator --------------------------------------------------------------------------------------

frame_iterator::frame_iterator(viewer* v) : _viewer(v)
{
    advance();
}

void frame_iterator::advance()
{
    // The previous frame (if any) was moved out by operator* and presented by the loop variable's destructor;
    // here we only fetch the next drawable frame, skipping closed (minimized) ones while the window lives.
    while (_viewer->is_running())
    {
        _current = frame_scope(_viewer->acquire_frame());
        if (_current.is_open())
        {
            _active = true;
            return;
        }
    }
    _active = false;
}

frame_iterator& frame_iterator::operator++()
{
    advance();
    return *this;
}

// ---- frame_range -----------------------------------------------------------------------------------------

frame_range frame_range::owning(viewer* v)
{
    auto r = frame_range(v);
    r._owns = true;
    return r;
}

frame_range::frame_range(frame_range&& o) noexcept : _viewer(o._viewer), _owns(o._owns)
{
    o._viewer = nullptr;
    o._owns = false;
}

frame_range& frame_range::operator=(frame_range&& o) noexcept
{
    if (this != &o)
    {
        end_loop();
        _viewer = o._viewer;
        _owns = o._owns;
        o._viewer = nullptr;
        o._owns = false;
    }
    return *this;
}

frame_range::~frame_range()
{
    end_loop();
}

void frame_range::end_loop()
{
    if (_viewer == nullptr)
        return;

    // The same signal the close button raises: the window is gone once the loop that pumped it is.
    // A viewer this range owns is about to be destroyed anyway, but it takes the same path so the rule reads the same
    // from either end.
    _viewer->request_close();

    if (_owns)
        delete _viewer;
    _viewer = nullptr;
    _owns = false;
}
} // namespace sv
