#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/metal/fwd.hh>
#include <shaped-graphics/backends/metal/metal_common.hh>
#include <shaped-graphics/backends/metal/metal_resource_access.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/raytracing/acceleration_structure.hh>

/// Metal acceleration structures.
///
/// **A built structure is not a buffer here**, which is the one shape difference from both other backends.
/// DXR names a structure by the GPU virtual address of the buffer the driver built it into, and Vulkan wraps an object
/// around such a buffer; `MTL::AccelerationStructure` derives from `MTL::Resource` and there is no `MTLBuffer` in the
/// caller's hands at any point.
/// It is minted by `newAccelerationStructure(size)` and named by `gpuResourceID()` — the same 8-byte `MTL::ResourceID`
/// a texture or sampler occupies in an argument buffer.
/// That is why `sg::blas` and `sg::tlas` carry no storage handle: the one they used to carry was a D3D12 fact.
///
/// **sg keeps BLAS and TLAS distinct and Metal does not**, so both subclasses hold the same `metal_accel_storage`.

/// The Metal object behind a blas or a tlas, plus the tracking every resource in this backend needs.
///
/// Held by value by both subclasses rather than shared through a base, because sg's two types have none in common —
/// and a Metal acceleration structure is one kind of object whichever of them is asking.
class sg::backend::metal::metal_accel_storage
{
public:
    metal_accel_storage(metal_context& ctx, MTL::AccelerationStructure* accel, isize size_in_bytes)
      : _ctx(ctx), _accel(accel), _size_in_bytes(size_in_bytes)
    {
    }

    metal_accel_storage(metal_accel_storage const&) = delete;
    metal_accel_storage& operator=(metal_accel_storage const&) = delete;

    [[nodiscard]] MTL::AccelerationStructure* accel() const { return _accel; }

    /// What an instance descriptor and an argument buffer name this structure by.
    /// Zero once released, which is what makes a use-after-expire a wrong id rather than a dangling pointer.
    [[nodiscard]] MTL::ResourceID resource_id() const
    {
        return _accel != nullptr ? _accel->gpuResourceID() : MTL::ResourceID{};
    }

    [[nodiscard]] isize size_in_bytes() const { return _size_in_bytes; }

    /// Access tracking, shared by every command list recording against this structure.
    /// Mutable because a command list declares against a handle to const, exactly as it does for a buffer.
    [[nodiscard]] cc::mutex<metal_resource_access>& access() const { return _access; }

    /// The direct-queue submission that last named this structure; see `submission_stamp`.
    [[nodiscard]] submission_stamp& submission() const { return _submission; }

    /// Hands the MTLAccelerationStructure to the epoch and runs `finalizers` behind it.
    /// Idempotent, and safe to call from both the expiry hook and the destructor.
    void release(cc::vector<cc::unique_function<void()>>& finalizers) const;

private:
    metal_context& _ctx;
    mutable MTL::AccelerationStructure* _accel = nullptr; // mutable: release runs from the const lifetime hooks
    isize _size_in_bytes = 0;
    mutable cc::mutex<metal_resource_access> _access;
    mutable submission_stamp _submission;
};

/// Metal bottom-level acceleration structure.
class sg::backend::metal::metal_blas final : public sg::blas
{
public:
    metal_blas(metal_context& ctx,
               MTL::AccelerationStructure* accel,
               isize size_in_bytes,
               isize build_scratch_size_in_bytes,
               isize update_scratch_size_in_bytes,
               sg::accel_build_flags build_flags,
               int geometry_count)
      : sg::blas(size_in_bytes, build_scratch_size_in_bytes, update_scratch_size_in_bytes, build_flags, geometry_count),
        _storage(ctx, accel, size_in_bytes)
    {
    }

    ~metal_blas() override { _storage.release(_finalizers); }

    [[nodiscard]] metal_accel_storage const& storage() const { return _storage; }

private:
    void on_expired() const override { _storage.release(_finalizers); }

    metal_accel_storage _storage;
};

/// Metal top-level acceleration structure, the same shape as metal_blas.
/// The base retains the referenced BLAS handles, which is the ownership edge.
class sg::backend::metal::metal_tlas final : public sg::tlas
{
public:
    metal_tlas(metal_context& ctx,
               MTL::AccelerationStructure* accel,
               isize size_in_bytes,
               isize build_scratch_size_in_bytes,
               isize update_scratch_size_in_bytes,
               sg::accel_build_flags build_flags,
               int instance_count,
               cc::vector<sg::blas_handle> referenced_blases)
      : sg::tlas(size_in_bytes,
                 build_scratch_size_in_bytes,
                 update_scratch_size_in_bytes,
                 build_flags,
                 instance_count,
                 cc::move(referenced_blases)),
        _storage(ctx, accel, size_in_bytes)
    {
    }

    ~metal_tlas() override { _storage.release(_finalizers); }

    [[nodiscard]] metal_accel_storage const& storage() const { return _storage; }

private:
    void on_expired() const override { _storage.release(_finalizers); }

    metal_accel_storage _storage;
};
