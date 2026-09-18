#pragma once

#include <shaped-graphics/backends/webgpu/fwd.hh>
#include <shaped-graphics/backends/webgpu/webgpu_common.hh>
#include <shaped-graphics/resource/raw_buffer.hh>

/// WebGPU implementation of sg::raw_buffer.
///
/// **The WebGPU buffer is allocated to a whole number of 4-byte words**, because every WebGPU copy and write addresses one that way.
/// So a transfer that ends at the buffer's logical end may round up into the padding, while one ending inside it must already be word-aligned.
///
/// Releasing a handle is always safe on WebGPU, whose objects are reference counted and outlive queued work that names them.
/// The epoch deferral here is therefore about sg's contract rather than memory safety.
/// An expired buffer is `destroy()`ed, and its finalizers run, only once the epoch that last recorded against it has retired.
class sg::backend::webgpu::webgpu_buffer final : public sg::raw_buffer
{
public:
    webgpu_buffer(webgpu_context& ctx, isize size_in_bytes, sg::buffer_usages usage, wgpu_buffer buffer);
    ~webgpu_buffer() override;

    /// The WebGPU buffer, null once expired.
    [[nodiscard]] WGPUBuffer raw() const { return _buffer.get(); }

    /// Bytes actually allocated: the logical size rounded up to a whole word.
    [[nodiscard]] isize allocated_bytes() const { return align_up(_size_in_bytes, buffer_word_bytes); }

protected:
    void on_expired() const override;

private:
    /// Hands the buffer and the finalizers to the open epoch; idempotent.
    void release_storage() const;

    webgpu_context& _ctx;
    mutable wgpu_buffer _buffer; // mutable: expiry is a const lifetime hook
};
