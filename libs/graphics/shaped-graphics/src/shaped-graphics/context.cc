#include <clean-core/common/assert.hh>
#include <shaped-graphics/context.hh>

namespace sg
{
context::context(backend_kind backend) : _backend(backend)
{
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
