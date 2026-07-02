// Translation unit for the vulkan backend static library. The stub types are defined inline in the
// headers (backends favor readable, low-encapsulation code); the create_vulkan_context factory lives
// here out-of-line, mirroring the dx12 backend. Defining it here also gives the archive a real symbol
// — a symbol-less static archive is rejected by the macOS linker ("archive member '/' not a mach-o
// file"). Real device/resource code lands here as the backend is implemented.

#include <shaped-graphics/backends/vulkan/vulkan_buffer.hh>
#include <shaped-graphics/backends/vulkan/vulkan_command_list.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>

namespace sg
{
cc::result<context_handle> create_vulkan_context(backend::vulkan::vulkan_config const&)
{
    return cc::error("vulkan backend not implemented yet");
}
} // namespace sg
