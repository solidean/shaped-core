#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/barrier/resource_access.hh>
#include <shaped-graphics/fwd.hh>

/// One resource foreign code will touch inside a native scope, and how it will touch it.
///
/// The access is sg's own vocabulary rather than D3D12's, because it is what the tracker reasons in — the scope
/// translates it exactly as a dispatch or a copy does.
/// `stages` says when, and defaults to compute, which is what a vendor SDK's networks run in.
struct sg::backend::dx12::native_texture_access
{
    sg::raw_texture_handle texture;
    sg::access_flags access;
    sg::pipeline_stage_flags stages = sg::pipeline_stage_flag::compute;
};

/// The same for a buffer, which has no layout.
struct sg::backend::dx12::native_buffer_access
{
    sg::raw_buffer_handle buffer;
    sg::access_flags access;
    sg::pipeline_stage_flags stages = sg::pipeline_stage_flag::compute;
};

namespace sg::backend::dx12
{
/// The layout a texture must be in to be touched with `access`, for code sg cannot see into.
///
/// Write beats read and the shader family beats the copy one, since foreign code given both does the write.
/// An access naming none of them asks for `general`, which every access can use and none is optimal for.
[[nodiscard]] sg::texture_layout native_layout_for(sg::access_flags access);
} // namespace sg::backend::dx12

/// Lets foreign code — a vendor SDK, a capture tool — record onto an sg command list, without sg losing track of what
/// it touched.
///
/// This is the **one sanctioned escape hatch** out of sg's barrier model, and the second place an access is declared
/// rather than inferred (array bindings are the first).
/// Everything else infers access from the operation, which foreign code makes impossible: sg cannot see the call, so
/// the caller names what it will touch and how.
///
/// Opening transitions every named resource into the declared access, and records that as its state, so the next sg
/// operation on it barriers from there.
/// Closing forgets the list's bind state, because foreign code is free to set its own descriptor heaps, root signature
/// and pipeline — so the next sg draw or dispatch rebinds from scratch rather than trusting what it last set.
///
/// **A caller that under-declares corrupts the tracker**: sg will believe a state the GPU is not in, and the result is
/// a validation error on one machine and wrong pixels on another.
/// Declare every resource the foreign call touches, and nothing it does not — the debug layer catches the first half.
///
/// The scope holds no lifetime: the handles it is given must outlive it, as they must outlive the list's recording.
class sg::backend::dx12::dx12_native_scope
{
public:
    /// Transitions each listed resource into its declared access, then hands out the native list.
    /// `cmd` must be a dx12 command list, and no rendering scope may be open — a native call is not a draw.
    [[nodiscard]] static dx12_native_scope open(sg::command_list& cmd,
                                                cc::span<native_texture_access const> textures,
                                                cc::span<native_buffer_access const> buffers = {});

    dx12_native_scope(dx12_native_scope&& other) noexcept;
    dx12_native_scope& operator=(dx12_native_scope&& other) noexcept;
    dx12_native_scope(dx12_native_scope const&) = delete;
    dx12_native_scope& operator=(dx12_native_scope const&) = delete;

    /// Forgets the list's bind state, so sg rebinds everything the foreign code may have replaced.
    ~dx12_native_scope();

    /// The list to record onto — the same one sg records into, so the foreign work lands in submission order.
    [[nodiscard]] ID3D12GraphicsCommandList* list() const;

    /// The device the list belongs to, which is what a vendor SDK is usually initialized against.
    [[nodiscard]] ID3D12Device* device() const;

    /// The native resource behind a handle this scope declared.
    /// Asserts on one it did not: an undeclared resource has no barrier, which is the failure this type exists to prevent.
    [[nodiscard]] ID3D12Resource* resource(sg::raw_texture_handle const& texture) const;
    [[nodiscard]] ID3D12Resource* resource(sg::raw_buffer_handle const& buffer) const;

private:
    explicit dx12_native_scope(dx12_command_list& cmd) : _cmd(&cmd) {}

    dx12_command_list* _cmd = nullptr;

    // What `resource` is allowed to hand out, in declaration order — a scope names a handful of resources, so a scan
    // beats a map.
    cc::vector<sg::raw_texture_handle> _textures;
    cc::vector<sg::raw_buffer_handle> _buffers;
};
