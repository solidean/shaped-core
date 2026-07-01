#pragma once

#include <shaped-graphics/backend/backend_command_list.hh>

namespace sg::backend::vulkan
{
/// Vulkan command list. See sg::backend_command_list for the recording contract; the concrete
/// recording methods land with the first buffer upload/download/copy milestone.
class vulkan_command_list final : public sg::backend_command_list
{
};
} // namespace sg::backend::vulkan
