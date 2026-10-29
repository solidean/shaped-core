#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/function/unique_function.hh>
#include <shaped-graphics/fwd.hh>

#include <atomic>

/// Opacity micromaps: per-triangle tables that let traversal decide most hits on alpha-tested geometry alone.
/// Each map subdivides one triangle into 4^level micro-triangles and stores a state for each.
/// A micro-triangle that is opaque is a hit and a transparent one a miss, with no candidate decision; only an unknown one asks.
/// Behind `sg::feature::opacity_micromap`: built through `cmd.raytracing.build_micromap`, and named by a `blas_triangles`.
/// See libs/graphics/shaped-graphics/docs/concepts/acceleration-structures.md.

/// How many bits a micro-triangle's state takes.
enum class sg::micromap_format : sg::u16
{
    /// Opaque or transparent; a ray never asks the candidate decision.
    two_state = 1,

    /// Opaque, transparent, or unknown with a guess (`micromap_state`); a ray asks at an unknown one unless it forces 2-state.
    four_state = 2,
};

/// One micro-triangle's state, as a 4-state map packs it into 2 bits; a 2-state map keeps the low bit alone.
enum class sg::micromap_state : sg::u8
{
    transparent = 0,
    opaque = 1,
    unknown_transparent = 2,
    unknown_opaque = 3,
};

/// A whole triangle's state, given in place of a map index, so a triangle that is uniform needs no map at all.
enum class sg::micromap_special_index : sg::i32
{
    fully_transparent = -1,
    fully_opaque = -2,
    fully_unknown_transparent = -3,
    fully_unknown_opaque = -4,
};

/// How many maps of one subdivision level and format a micromap holds, or a BLAS geometry names.
/// Both APIs size their builds from these counts on the CPU, so they must be known before the build, even when a GPU pass writes the maps.
struct sg::micromap_usage
{
    u32 count = 0;
    u32 subdivision_level = 0;
    micromap_format format = micromap_format::four_state;
};

/// One map's GPU record in a micromap's descriptor buffer: 8 bytes, the layout both D3D12 and Vulkan read.
/// `states_offset_in_bytes` is where the map's packed states start in the states buffer.
struct sg::micromap_descriptor
{
    u32 states_offset_in_bytes = 0;
    u16 subdivision_level = 0;
    micromap_format format = micromap_format::four_state;
};
static_assert(sizeof(sg::micromap_descriptor) == 8, "a micromap descriptor is the APIs' 8-byte record");

/// The input to `build_micromap`: the packed states and one descriptor per map.
/// Both buffers must carry `buffer_usage::accel_structure_build_input`, and both must start 256-byte aligned at their offsets.
/// A map's states are packed micro-triangle by micro-triangle in the APIs' fixed order, lowest bits first.
struct sg::micromap_triangles
{
    raw_buffer_handle states = nullptr;
    isize states_offset_in_bytes = 0;

    /// `micromap_descriptor` records, one per map, in the order the maps are indexed.
    raw_buffer_handle descriptors = nullptr;
    isize descriptors_offset_in_bytes = 0;

    /// How many maps there are of each level and format; their counts sum to the number of descriptors.
    cc::vector<micromap_usage> usage;
};

/// An opacity micromap: an opaque, driver-built structure a BLAS reads while it is built and traced.
/// Built through `cmd.raytracing.build_micromap`, and the returned handle is persistent — valid across epochs.
/// A BLAS that names it holds it, so it lives at least as long as every structure built from it.
class sg::micromap : public std::enable_shared_from_this<micromap>
{
public:
    virtual ~micromap();

    /// Bytes the built micromap occupies.
    [[nodiscard]] isize size_in_bytes() const { return _size_in_bytes; }
    [[nodiscard]] isize build_scratch_size_in_bytes() const { return _build_scratch_size_in_bytes; }

    /// What the build was given: how many maps of each level and format.
    [[nodiscard]] cc::span<micromap_usage const> usage() const { return _usage; }

    /// How many maps it holds, which a geometry's map indices must stay below.
    [[nodiscard]] isize map_count() const;

    void add_finalizer(cc::unique_function<void()> finalizer) const { _finalizers.push_back(cc::move(finalizer)); }
    [[nodiscard]] bool is_expired() const { return _expired.load(std::memory_order_acquire); }
    [[nodiscard]] bool is_valid() const { return !is_expired(); }

    /// Expire the micromap now, releasing its GPU storage — deferred until it is no longer in flight, and idempotent.
    void expire() const
    {
        if (!_expired.exchange(true, std::memory_order_acq_rel))
            on_expired();
    }

protected:
    micromap(isize size_in_bytes, isize build_scratch_size_in_bytes, cc::vector<micromap_usage> usage);

    virtual void on_expired() const {}

    isize _size_in_bytes = 0;
    isize _build_scratch_size_in_bytes = 0;
    cc::vector<micromap_usage> _usage;
    mutable cc::vector<cc::unique_function<void()>> _finalizers; // mutable: add_finalizer is const (a lifetime hook)
    mutable std::atomic<bool> _expired = {false};                // mutable: expire() is a const lifetime hook
};
