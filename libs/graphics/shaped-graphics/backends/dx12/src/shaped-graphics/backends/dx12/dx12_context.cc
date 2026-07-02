// dx12_context: device-level lifetime bodies (shutdown / teardown). The heavier bring-up path
// lives in dx12_context.create.cc.

#include <shaped-graphics/backends/dx12/dx12_context.hh>

namespace sg::backend::dx12
{
void dx12_context::shutdown()
{
    if (_is_shut_down)
        return;
    // Release the device-level COM objects (live-object tracking will unwind here later too).
    _queue.Reset();
    _device.Reset();
    _factory.Reset();
    _is_shut_down = true;
}
} // namespace sg::backend::dx12
