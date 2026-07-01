#pragma once

#include <shaped-graphics/backend/backend_command_list.hh>

namespace sg::backend::dx12
{
/// DirectX 12 command list. See sg::backend_command_list for the recording contract; the
/// concrete recording methods land with the first buffer upload/download/copy milestone.
class dx12_command_list final : public sg::backend_command_list
{
};
} // namespace sg::backend::dx12
