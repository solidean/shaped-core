#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/backends/metal/fwd.hh>
#include <shaped-graphics/backends/metal/metal_common.hh>
#include <shaped-graphics/barrier/resource_access.hh>
#include <shaped-graphics/fwd.hh>

/// One resource foreign code will touch inside a native scope, and how it will touch it.
///
/// The access is sg's own vocabulary, translated exactly as a dispatch or a copy is.
/// `stages` says when, and defaults to compute, which is what a vendor SDK's networks run in.
struct sg::backend::metal::native_texture_access
{
    sg::raw_texture_handle texture;
    sg::access_flags access;
    sg::pipeline_stage_flags stages = sg::pipeline_stage_flag::compute;
};

/// The same for a buffer.
struct sg::backend::metal::native_buffer_access
{
    sg::raw_buffer_handle buffer;
    sg::access_flags access;
    sg::pipeline_stage_flags stages = sg::pipeline_stage_flag::compute;
};

/// Lets foreign code — MetalFX, a capture tool — encode onto an sg command list without sg losing track of what it
/// touched.
///
/// The Metal twin of `dx12_native_scope`, and the same sanctioned escape hatch out of sg's barrier model: sg cannot see
/// the foreign call, so the caller names what it will touch and how.
///
/// **Foreign code encodes its own encoders, which carry none of sg's barriers, so the two meet at a fence.**
/// Opening closes sg's encoder after it waits on everything recorded before and updates `fence()`.
/// The foreign code must wait for that fence before its work and update it after — MetalFX does both when its
/// scaler's `fence` property is set to it.
/// The first encoder sg opens after the scope waits for the fence, so it sees what the foreign code wrote.
/// Foreign code that ignores the fence races sg in both directions.
///
/// **A caller that under-declares corrupts the tracker**: sg will believe a state the GPU is not in.
/// Declare every resource the foreign call touches, and nothing it does not.
///
/// The resources must be resident, which every sg resource is; foreign code's own resources are its own concern.
class sg::backend::metal::metal_native_scope
{
public:
    /// Records the declared accesses and closes sg's encoder behind a fence update, then hands out the native buffer.
    /// `cmd` must be a metal command list, and no rendering scope may be open — a native call is not a draw.
    [[nodiscard]] static metal_native_scope open(sg::command_list& cmd,
                                                 cc::span<native_texture_access const> textures,
                                                 cc::span<native_buffer_access const> buffers = {});

    metal_native_scope(metal_native_scope&& other) noexcept;
    metal_native_scope& operator=(metal_native_scope&& other) noexcept;
    metal_native_scope(metal_native_scope const&) = delete;
    metal_native_scope& operator=(metal_native_scope const&) = delete;

    /// Makes sg's next encoder wait for the fence.
    ~metal_native_scope();

    /// The command buffer to encode into — the one sg records into, so the foreign work lands in submission order.
    [[nodiscard]] MTL4::CommandBuffer* command_buffer() const;

    /// The device the buffer belongs to.
    [[nodiscard]] MTL::Device* device() const;

    /// The context's MTL4 compiler, which MetalFX creates its effects with.
    [[nodiscard]] MTL4::Compiler* compiler() const;

    /// The fence the foreign work must wait for before it runs and update after; see the type's comment.
    [[nodiscard]] MTL::Fence* fence() const;

    /// The native resource behind a handle this scope declared.
    /// Asserts on one it did not: an undeclared resource has no barrier, which is the failure this type exists to prevent.
    [[nodiscard]] MTL::Texture* texture(sg::raw_texture_handle const& texture) const;
    [[nodiscard]] MTL::Buffer* buffer(sg::raw_buffer_handle const& buffer) const;

private:
    explicit metal_native_scope(metal_command_list& cmd) : _cmd(&cmd) {}

    metal_command_list* _cmd = nullptr;

    // What `texture` and `buffer` may hand out, in declaration order — a scope names a handful, so a scan beats a map.
    cc::vector<sg::raw_texture_handle> _textures;
    cc::vector<sg::raw_buffer_handle> _buffers;
};
