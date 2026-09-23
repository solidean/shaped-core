#include <clean-core/common/asserts.hh>
#include <clean-core/common/utility.hh>
#include <shaped-graphics/backends/dx12/dx12_buffer.hh>
#include <shaped-graphics/backends/dx12/dx12_command_list.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_native_scope.hh>
#include <shaped-graphics/backends/dx12/dx12_texture.hh>
#include <shaped-graphics/backends/dx12/dx12_texture_access.hh> // subresource_extent_of
#include <shaped-graphics/context/context.hh>

namespace sg::backend::dx12
{
namespace
{
[[nodiscard]] dx12_command_list& as_dx12_list(sg::command_list& cmd)
{
    auto* const list = dynamic_cast<dx12_command_list*>(&cmd);
    CC_ASSERT(list != nullptr, "a dx12 native scope needs a dx12 command list");
    return *list;
}
} // namespace

sg::texture_layout native_layout_for(sg::access_flags access)
{
    if (access.has(sg::access_flag::shader_write))
        return sg::texture_layout::shader_readwrite;
    if (access.has(sg::access_flag::color_write))
        return sg::texture_layout::render_target;
    if (access.has(sg::access_flag::depth_write))
        return sg::texture_layout::depth_readwrite;
    if (access.has(sg::access_flag::copy_write))
        return sg::texture_layout::copy_dst;
    if (access.has(sg::access_flag::shader_read))
        return sg::texture_layout::shader_readonly;
    if (access.has(sg::access_flag::depth_read))
        return sg::texture_layout::depth_readonly;
    if (access.has(sg::access_flag::copy_read))
        return sg::texture_layout::copy_src;
    return sg::texture_layout::general;
}

dx12_native_scope dx12_native_scope::open(sg::command_list& cmd,
                                          cc::span<native_texture_access const> textures,
                                          cc::span<native_buffer_access const> buffers)
{
    auto& list = as_dx12_list(cmd);
    CC_ASSERT(!list._in_render_pass, "a native scope may not be opened inside a rendering scope");

    auto scope = dx12_native_scope(list);

    for (auto const& t : textures)
    {
        CC_ASSERT(t.texture != nullptr, "a native scope was given a null texture");
        auto const texture = std::static_pointer_cast<dx12_texture const>(t.texture);

        // The whole texture, because foreign code names a resource rather than a subresource range.
        auto const whole = sg::subresource_range::whole(subresource_extent_of(texture->description()));
        list.track_texture_access(texture, whole, t.stages, t.access, native_layout_for(t.access));
        scope._textures.push_back(t.texture);
    }

    for (auto const& b : buffers)
    {
        CC_ASSERT(b.buffer != nullptr, "a native scope was given a null buffer");
        list.track_buffer_access(std::static_pointer_cast<dx12_buffer const>(b.buffer), b.stages, b.access);
        scope._buffers.push_back(b.buffer);
    }

    // The barriers land before the foreign code runs, which is the whole point of declaring at all.
    list.flush_barriers();
    return scope;
}

dx12_native_scope::dx12_native_scope(dx12_native_scope&& other) noexcept
  : _cmd(other._cmd), _textures(cc::move(other._textures)), _buffers(cc::move(other._buffers))
{
    other._cmd = nullptr;
}

dx12_native_scope& dx12_native_scope::operator=(dx12_native_scope&& other) noexcept
{
    if (this == &other)
        return *this;

    // The scope being replaced still owes its list the bind-state reset.
    if (_cmd != nullptr)
        _cmd->forget_bind_state();

    _cmd = other._cmd;
    _textures = cc::move(other._textures);
    _buffers = cc::move(other._buffers);
    other._cmd = nullptr;
    return *this;
}

dx12_native_scope::~dx12_native_scope()
{
    if (_cmd != nullptr)
        _cmd->forget_bind_state();
}

ID3D12GraphicsCommandList* dx12_native_scope::list() const
{
    CC_ASSERT(_cmd != nullptr, "a moved-from native scope records nothing");
    return _cmd->_list.Get();
}

ID3D12Device* dx12_native_scope::device() const
{
    CC_ASSERT(_cmd != nullptr, "a moved-from native scope has no device");
    return _cmd->_ctx._device.Get();
}

ID3D12Resource* dx12_native_scope::resource(sg::raw_texture_handle const& texture) const
{
    for (auto const& declared : _textures)
        if (declared == texture)
            return std::static_pointer_cast<dx12_texture const>(texture)->_resource.Get();

    CC_UNREACHABLE("a native scope was asked for a texture it did not declare, so nothing barriered it");
}

ID3D12Resource* dx12_native_scope::resource(sg::raw_buffer_handle const& buffer) const
{
    for (auto const& declared : _buffers)
        if (declared == buffer)
            return std::static_pointer_cast<dx12_buffer const>(buffer)->_resource.Get();

    CC_UNREACHABLE("a native scope was asked for a buffer it did not declare, so nothing barriered it");
}
} // namespace sg::backend::dx12
