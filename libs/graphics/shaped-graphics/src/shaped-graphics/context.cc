#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <shaped-graphics/command_list.hh>
#include <shaped-graphics/context.hh>
#include <shaped-graphics/exceptions.hh>

namespace sg
{
std::unique_ptr<command_list> context::create_command_list()
{
    auto r = try_create_command_list();
    if (r.has_value())
        return cc::move(r.value());

    // Command-list creation only fails on device loss (backend already marked it) or an internal bug.
    if (_device_lost)
        throw device_lost_exception(_device_loss_reason);

    CC_UNREACHABLE("create_command_list failed on a healthy device — internal error");
}

void context::mark_device_lost(cc::string reason)
{
    // Sticky: the first observed reason wins; later observations don't overwrite it.
    if (_device_lost)
        return;
    _device_lost = true;
    _device_loss_reason = cc::move(reason);
}

context::context(backend_kind backend, thread_model threading)
  : persistent(*this), transient(*this), upload(*this), download(*this), _backend(backend), _thread_model(threading)
{
    // The scope members only store a back-reference; they don't touch any not-yet-constructed member.
}

context::~context()
{
    // The backend destructor calls shutdown() before this base destructor runs. Reaching here not
    // shut down means a lifetime/order bug (e.g. a context torn down through a path that skipped it).
    CC_ASSERT(_is_shut_down, "context must be shut down before destruction");
}

void context::shutdown()
{
    // A base context has no backend resources of its own; a backend overrides this to release its
    // device/queue/tracking (and duplicate this idempotent flag flip).
    _is_shut_down = true;
}
} // namespace sg
