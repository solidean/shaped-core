#include <clean-core/common/asserts.hh>
#include <clean-core/common/utility.hh>
#include <shaped-graphics/backends/metal/metal_buffer.hh>
#include <shaped-graphics/backends/metal/metal_command_list.hh>
#include <shaped-graphics/backends/metal/metal_context.hh>
#include <shaped-graphics/backends/metal/metal_native_scope.hh>
#include <shaped-graphics/backends/metal/metal_texture.hh>

namespace sg::backend::metal
{
namespace
{
[[nodiscard]] metal_command_list& as_metal_list(sg::command_list& cmd)
{
    auto* const list = dynamic_cast<metal_command_list*>(&cmd);
    CC_ASSERT(list != nullptr, "a metal native scope needs a metal command list");
    return *list;
}
} // namespace

metal_native_scope metal_native_scope::open(sg::command_list& cmd,
                                            cc::span<native_texture_access const> textures,
                                            cc::span<native_buffer_access const> buffers)
{
    auto& list = as_metal_list(cmd);
    auto scope = metal_native_scope(list);

    for (auto const& t : textures)
    {
        CC_ASSERT(t.texture != nullptr, "a native scope was given a null texture");
        (void)list.declare_texture(t.texture, t.stages, t.access);
        scope._textures.push_back(t.texture);
    }

    for (auto const& b : buffers)
    {
        CC_ASSERT(b.buffer != nullptr, "a native scope was given a null buffer");
        (void)list.declare_buffer(b.buffer, b.stages, b.access);
        scope._buffers.push_back(b.buffer);
    }

    list.enter_native_scope();
    return scope;
}

metal_native_scope::metal_native_scope(metal_native_scope&& other) noexcept
  : _cmd(other._cmd), _textures(cc::move(other._textures)), _buffers(cc::move(other._buffers))
{
    other._cmd = nullptr;
}

metal_native_scope& metal_native_scope::operator=(metal_native_scope&& other) noexcept
{
    if (this == &other)
        return *this;

    // The scope being replaced still owes its list the fence wait.
    if (_cmd != nullptr)
        _cmd->leave_native_scope();

    _cmd = other._cmd;
    _textures = cc::move(other._textures);
    _buffers = cc::move(other._buffers);
    other._cmd = nullptr;
    return *this;
}

metal_native_scope::~metal_native_scope()
{
    if (_cmd != nullptr)
        _cmd->leave_native_scope();
}

MTL4::CommandBuffer* metal_native_scope::command_buffer() const
{
    CC_ASSERT(_cmd != nullptr, "a moved-from native scope records nothing");
    return _cmd->buffer();
}

MTL::Device* metal_native_scope::device() const
{
    CC_ASSERT(_cmd != nullptr, "a moved-from native scope has no device");
    return _cmd->_metal_context.device();
}

MTL4::Compiler* metal_native_scope::compiler() const
{
    CC_ASSERT(_cmd != nullptr, "a moved-from native scope has no compiler");
    return _cmd->_metal_context.compiler();
}

MTL::Fence* metal_native_scope::fence() const
{
    CC_ASSERT(_cmd != nullptr, "a moved-from native scope has no fence");
    return _cmd->_metal_context.native_scope_fence();
}

MTL::Texture* metal_native_scope::texture(sg::raw_texture_handle const& texture) const
{
    for (auto const& declared : _textures)
        if (declared == texture)
            return static_cast<metal_texture const&>(*texture).texture();

    CC_UNREACHABLE("a native scope was asked for a texture it did not declare, so nothing barriered it");
}

MTL::Buffer* metal_native_scope::buffer(sg::raw_buffer_handle const& buffer) const
{
    for (auto const& declared : _buffers)
        if (declared == buffer)
            return static_cast<metal_buffer const&>(*buffer).buffer();

    CC_UNREACHABLE("a native scope was asked for a buffer it did not declare, so nothing barriered it");
}
} // namespace sg::backend::metal
