#pragma once

// The single include gate for <webgpu/webgpu.h> plus the handle and string helpers every WebGPU TU shares.

#include <clean-core/common/utility.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/backends/webgpu/fwd.hh>
#include <webgpu/webgpu.h>

/// An owning reference to one WebGPU object: releases on destruction, adds a reference on copy.
///
/// WebGPU objects are reference counted by the implementation, and a released object stays alive while queued work still uses it.
/// So dropping a handle is always safe, whatever the GPU is doing, which is why this backend needs no deferred deletion for correctness.
template <class T>
struct sg::backend::webgpu::wgpu_handle
{
    wgpu_handle() = default;
    explicit wgpu_handle(T raw) : _raw(raw) {}

    wgpu_handle(wgpu_handle const& rhs) : _raw(rhs._raw) { add_ref(); }
    wgpu_handle(wgpu_handle&& rhs) noexcept : _raw(rhs._raw) { rhs._raw = nullptr; }
    wgpu_handle& operator=(wgpu_handle const& rhs)
    {
        if (this != &rhs)
        {
            release();
            _raw = rhs._raw;
            add_ref();
        }
        return *this;
    }
    wgpu_handle& operator=(wgpu_handle&& rhs) noexcept
    {
        if (this != &rhs)
        {
            release();
            _raw = rhs._raw;
            rhs._raw = nullptr;
        }
        return *this;
    }
    ~wgpu_handle() { release(); }

    [[nodiscard]] T get() const { return _raw; }
    [[nodiscard]] explicit operator bool() const { return _raw != nullptr; }

    /// Hands the raw object out and forgets it, for an API that takes ownership.
    [[nodiscard]] T release_ownership()
    {
        auto const r = _raw;
        _raw = nullptr;
        return r;
    }

private:
    void add_ref() const;
    void release();

    T _raw = nullptr;
};

namespace sg::backend::webgpu
{
#define SG_WEBGPU_HANDLE(Type, Name)                     \
    template <>                                          \
    inline void wgpu_handle<WGPU##Type>::add_ref() const \
    {                                                    \
        if (_raw != nullptr)                             \
            wgpu##Type##AddRef(_raw);                    \
    }                                                    \
    template <>                                          \
    inline void wgpu_handle<WGPU##Type>::release()       \
    {                                                    \
        if (_raw != nullptr)                             \
            wgpu##Type##Release(_raw);                   \
        _raw = nullptr;                                  \
    }                                                    \
    using Name = wgpu_handle<WGPU##Type>

SG_WEBGPU_HANDLE(Instance, wgpu_instance);
SG_WEBGPU_HANDLE(Adapter, wgpu_adapter);
SG_WEBGPU_HANDLE(Device, wgpu_device);
SG_WEBGPU_HANDLE(Queue, wgpu_queue);
SG_WEBGPU_HANDLE(Buffer, wgpu_buffer);
SG_WEBGPU_HANDLE(Texture, wgpu_texture);
SG_WEBGPU_HANDLE(TextureView, wgpu_texture_view);
SG_WEBGPU_HANDLE(Sampler, wgpu_sampler);
SG_WEBGPU_HANDLE(BindGroupLayout, wgpu_bind_group_layout);
SG_WEBGPU_HANDLE(BindGroup, wgpu_bind_group);
SG_WEBGPU_HANDLE(PipelineLayout, wgpu_pipeline_layout);
SG_WEBGPU_HANDLE(ShaderModule, wgpu_shader_module);
SG_WEBGPU_HANDLE(ComputePipeline, wgpu_compute_pipeline);
SG_WEBGPU_HANDLE(RenderPipeline, wgpu_render_pipeline);
SG_WEBGPU_HANDLE(CommandEncoder, wgpu_command_encoder);
SG_WEBGPU_HANDLE(CommandBuffer, wgpu_command_buffer);
SG_WEBGPU_HANDLE(ComputePassEncoder, wgpu_compute_pass);
SG_WEBGPU_HANDLE(RenderPassEncoder, wgpu_render_pass);
SG_WEBGPU_HANDLE(QuerySet, wgpu_query_set);
SG_WEBGPU_HANDLE(Surface, wgpu_surface);

#undef SG_WEBGPU_HANDLE

/// A view of `s` WebGPU can read; valid while `s` is.
[[nodiscard]] inline WGPUStringView to_wgpu(cc::string_view s)
{
    return WGPUStringView{.data = s.data(), .length = size_t(s.size())};
}

/// The text a WebGPU callback handed over, copied out, since it is valid only for the callback's duration.
[[nodiscard]] inline cc::string from_wgpu(WGPUStringView s)
{
    if (s.data == nullptr)
        return {};
    if (s.length == WGPU_STRLEN)
        return cc::string(cc::string_view(s.data));
    return cc::string(cc::string_view(s.data, isize(s.length)));
}

/// `value` rounded up to a multiple of `alignment`, which must be positive.
[[nodiscard]] constexpr isize align_up(isize value, isize alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

/// WebGPU copies and writes address buffers in whole 4-byte words, so every buffer is allocated to a multiple of 4.
inline constexpr isize buffer_word_bytes = 4;

/// A copy between a buffer and a texture places each row at a multiple of this many bytes.
inline constexpr isize copy_row_alignment = 256;
} // namespace sg::backend::webgpu
