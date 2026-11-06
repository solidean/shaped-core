#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/raytracing/acceleration_structure.hh>

// The software walk's structures: what webgpu's polyfill and an opted-in dx12 or vulkan build share.
// The layout is libs/graphics/shaped-graphics-language/docs/raytracing-polyfill.md.

namespace sg::impl
{
/// How many acceleration members a pipeline's roots hold.
inline constexpr isize software_trace_roots = 16;

/// The shape of a BVH2 over a primitive count, with node 0 its root.
struct acceleration_tree
{
    u32 node_count = 0;

    /// The leaves are the last nodes, [first_leaf, node_count).
    u32 first_leaf = 0;
    u32 leaf_count = 0;

    /// Each level's inner nodes, bottom-up, which is the order their boxes are computed in.
    struct level
    {
        u32 first = 0;
        u32 count = 0;
    };
    cc::vector<level> levels;

    /// Two units per node, their boxes zero and their `w` words set.
    cc::vector<u32> node_words;
};

/// Leaves over consecutive runs of up to 4 primitives, then each level pairing consecutive nodes, an odd one moving up unpaired.
[[nodiscard]] acceleration_tree lay_out_acceleration_tree(u32 primitive_count);

/// The inverse of a row-major 3x4 affine transform, row-major 3x4 again.
/// A singular one, such as an instance scaled to nothing, inverts to zero, which moves every ray to the origin and away from every hit it could not have had.
void invert_affine(float const (&m)[12], float (&out)[12]);

/// Appends instance `index` of a TLAS as the pool holds it, 7 units, or 10 in a TLAS where one moves.
void append_instance_words(cc::vector<u32>& words, tlas_instance const& instance, u32 blas_unit, u32 index, bool tlas_moves);

/// Where an acceleration member of `bindings` stands among them, by binding index, which is the order roots take.
[[nodiscard]] isize acceleration_position(cc::span<binding const> bindings, u32 binding_index);
[[nodiscard]] isize acceleration_count(cc::span<binding const> bindings);
/// Whether a binding of `bindings` is traced in software, which puts the pool and the roots in the reserved group.
[[nodiscard]] bool traces_in_software(cc::span<binding const> bindings);

/// What a backend's memory for the pool is: written through `mapped`, read by a shader at `gpu_address`.
struct software_pool_storage
{
    byte* mapped = nullptr;
    u64 gpu_address = 0;
    isize size_in_bytes = 0;
};

/// The memory an opted-in dx12 or vulkan build writes its structures into, one per context.
/// **Host-visible and written at record time**, so a build needs no copy, no compute pass and no compiled kernel:
/// a region is new memory no recorded work reads, so nothing has to be ordered against it.
/// A region is immutable once written and its offset never changes, so a dispatch that bound an older storage still
/// finds every region it can name.
/// Safe from any recording thread.
class software_acceleration_pool
{
public:
    explicit software_acceleration_pool(context& ctx) : _ctx(ctx) {}
    virtual ~software_acceleration_pool() = default;

    /// `units` of the pool; throws sg::allocation_exception where the pool cannot grow.
    [[nodiscard]] u32 allocate(isize units);
    void write(u32 unit, cc::span<u32 const> words);
    /// Hands a region back once every epoch that can read it has retired.
    void release(u32 unit, u32 units);

    /// The storage as it is now, created on first use, which the next dispatch binds.
    [[nodiscard]] software_pool_storage storage();

protected:
    /// New zeroed storage of at least `size_in_bytes`.
    [[nodiscard]] virtual cc::result<software_pool_storage> create_storage(isize size_in_bytes) = 0;
    /// Frees `storage` once no recorded work can read it.
    virtual void retire_storage(software_pool_storage storage) = 0;

private:
    struct free_range
    {
        u32 unit = 0;
        u32 units = 0;
    };
    struct state
    {
        software_pool_storage storage;
        cc::vector<free_range> free;
    };

    void grow(state& s, isize min_units);

    context& _ctx;
    cc::mutex<state> _state;
};

/// The pool regions of a build, with what a TLAS over it reads.
/// All three read the geometry's `cpu_*` spans within the call, and assert where one is missing.
[[nodiscard]] software_region build_software_blas(software_acceleration_pool& pool,
                                                  cc::span<blas_triangles const> geometries);
[[nodiscard]] software_region build_software_blas(software_acceleration_pool& pool,
                                                  cc::span<blas_aabbs const> geometries);
/// Every instance's BLAS must have been built with `accel_build_flag::software_trace`.
[[nodiscard]] software_region build_software_tlas(software_acceleration_pool& pool,
                                                  cc::span<tlas_instance const> instances);
} // namespace sg::impl
