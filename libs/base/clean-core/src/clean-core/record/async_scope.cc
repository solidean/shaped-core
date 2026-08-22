#include "async_scope.hh"

using namespace cc::primitive_defines;

// An async scope's two events and its two links.
//
// The BEGIN event is what puts a NAME on a trace: an id is minted at runtime and a descriptor is static, so nothing
// else in the stream could say that trace 0x0800…03 was called "handle-request".
// One event per scope creation buys that, and an offline reader then needs nothing else — no ambient chain, no live
// process, no pointer that meant something only while it ran.

cc::rec::impl::trace_link_scope::trace_link_scope(cc::rec::trace_id id)
{
    if (id == rec::trace_id::none)
        return;

    // The id's bit pattern IS the value, not a pointer to it.
    // The link routinely outlives this object, so anything it pointed at would have to outlive it too.
    new (cc::placement_new, _storage) cc::async_ambient_scope(rec::impl::trace_tag(), u64(id));
    _installed = true;
}

cc::rec::impl::trace_link_scope::~trace_link_scope()
{
    if (_installed)
        reinterpret_cast<cc::async_ambient_scope*>(_storage)->~async_ambient_scope();
}

cc::rec::impl::async_scope_guard::async_scope_guard(cc::rec::desc const& begin_desc,
                                                    cc::rec::desc const& end_desc,
                                                    cc::rec::trace_id id)
  : _end_desc(end_desc), _id(id), _trace(id), _scope(rec::impl::async_scope_tag(), const_cast<rec::desc*>(&begin_desc))
{
    // After both links are in, so the event sits beside a delta that already names the full context.
    rec::record_event(begin_desc, u64(id));
}

cc::rec::impl::async_scope_guard::~async_scope_guard()
{
    // Before the links come off, for the same reason.
    rec::record_event(_end_desc, u64(_id));
}
