#pragma once

#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/fwd.hh>

/// Which GPU a process creates its contexts on, from `SC_GPU`, for every backend that picks one itself.
///
/// `intel`, `amd` and `nvidia` match an adapter by its PCI vendor id; anything else is a case-insensitive substring of its name, as `B580`.
/// An adapter the selector does not match is never picked, and a context that finds none fails, naming what it saw.
/// It narrows the hardware search only: software adapters, an adapter chosen in code (`dx12_config::adapter_luid`,
/// `vulkan_config::choose_physical_device`) and the dx12 runtime's own `SC_DX12_ADAPTER` hiding are untouched by it.
///
/// Read on every call, so a test may set the variable for a context it is about to create.

namespace sg
{
/// What `SC_GPU` says, or empty where it is unset.
[[nodiscard]] cc::string gpu_selector_from_environment();

/// Whether an adapter of `vendor_id` named `name` is one `selector` asks for; an empty selector asks for any.
[[nodiscard]] bool gpu_selector_matches(cc::string_view selector, u32 vendor_id, cc::string_view name);
} // namespace sg
