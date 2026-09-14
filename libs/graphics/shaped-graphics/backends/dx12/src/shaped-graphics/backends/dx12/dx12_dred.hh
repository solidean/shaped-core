#pragma once

#include <clean-core/string/string.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>

/// Device Removed Extended Data: what the GPU was doing when the device went away.
///
/// A removed device reports only an HRESULT, and `DXGI_ERROR_DEVICE_HUNG` names nothing that would let anyone
/// find the work that hung.
/// DRED is the runtime's answer: auto-breadcrumbs record how far each command list got, and the page-fault
/// record names the allocation an offending access landed in.
///
/// **Both must be armed before the device exists**, because the runtime decides then whether to carry the
/// bookkeeping at all — which is why this is a process-wide arm rather than a context method.
namespace sg::backend::dx12
{
/// Arms DRED for every device created afterwards in this process, once.
///
/// Returns whether it is on, which is false on a runtime too old to offer the interface.
/// Auto-breadcrumbs cost a write per command-list operation, so this is opt-in per `dx12_config::enable_dred`
/// rather than always on.
bool enable_dred_once();

/// What DRED holds about `device`'s removal, empty when it was not armed or the runtime reports nothing.
///
/// Worth reading only once the device is actually removed: before that the runtime has nothing to say, and
/// the queried interface reports `DXGI_ERROR_NOT_CURRENTLY_AVAILABLE`.
///
/// The breadcrumbs are truncated per list — a hung list's tail is what matters, not its whole history.
[[nodiscard]] cc::string dred_report(ID3D12Device* device);
} // namespace sg::backend::dx12
