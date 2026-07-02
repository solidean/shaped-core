#pragma once

#include <shaped-graphics/command_list.hh>

namespace sg::backend::vulkan
{
/// Vulkan implementation of sg::command_list. Derives directly; the concrete recording methods
/// land here with the first buffer upload/download/copy milestone.
class vulkan_command_list final : public sg::command_list
{
};
} // namespace sg::backend::vulkan
