#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/buffer.hh>
#include <shaped-graphics/resource/views.hh>

/// The GPU-written tier of a partitioned TLAS: a compute shader writes the records of an update, and sg builds from them.
/// The records are SGL's `accel` module's, which a shader writes by field name and each backend lays out natively.
/// Their host types below size and bind buffers of them, and nothing on the host reads or writes one.
/// See libs/graphics/shaped-graphics/docs/plans/clusters-and-partitioned-tlas.md.

/// SGL's `accel.blas_ref`: a BLAS's GPU address, which only a `blas_table` hands out.
struct sg::gpu_blas_ref
{
    u64 address = 0;
};

/// SGL's `accel.ptlas_write`, whose fields stand in each backend's own order: opaque here, since the host never fills one.
/// Every backend sg supports lays it out in 112 bytes, which is what a buffer of them strides by.
struct alignas(16) sg::gpu_ptlas_write
{
    byte bytes[112] = {};
};

/// SGL's `accel.ptlas_update`, laid out alike on every backend.
struct sg::gpu_ptlas_update
{
    u32 index = 0;
    u32 hit_group_offset = 0;
    gpu_blas_ref blas;
};

/// SGL's `accel.ptlas_translation`, laid out alike on every backend.
struct sg::gpu_ptlas_translation
{
    u32 partition = 0;
    float translation[3] = {0, 0, 0};
};

/// SGL's `accel.buffer_ref`: a buffer's GPU address, which only a `buffer_table` hands out.
struct sg::gpu_buffer_ref
{
    u64 address = 0;
};

/// SGL's `accel.clas_triangles`, a cluster's build record: opaque here, since its fields differ per backend.
/// Sized for the largest backend's record, D3D12's 80 bytes, so a buffer of n of them has room for n on every backend;
/// a build strides by its own backend's record, which is Vulkan's 64 bytes there.
struct alignas(8) sg::gpu_clas_triangles
{
    byte bytes[80] = {};
};

static_assert(sizeof(sg::gpu_blas_ref) == 8);
static_assert(sizeof(sg::gpu_buffer_ref) == 8);
static_assert(sizeof(sg::gpu_clas_triangles) == 80);
static_assert(sizeof(sg::gpu_ptlas_write) == 112);
static_assert(sizeof(sg::gpu_ptlas_update) == 16);
static_assert(sizeof(sg::gpu_ptlas_translation) == 16);

/// A fixed list of BLASes, held, and a buffer of their addresses a shader binds as `buffer[accel.blas_ref]`.
/// A shader copies `table[k]` into a record, so an address on the GPU is always one sg handed out.
/// The table holds every BLAS it lists, and an update naming the table holds the table (`ptlas_indirect_operations`).
/// Created through `ctx.persistent.create_blas_table`; immutable, so another set of BLASes is another table.
class sg::blas_table
{
public:
    blas_table(cc::vector<blas_handle> blases, buffer<gpu_blas_ref> addresses)
      : _blases(cc::move(blases)), _addresses(cc::move(addresses))
    {
    }

    /// The BLASes in the order their addresses stand in the buffer.
    [[nodiscard]] cc::span<blas_handle const> blases() const { return _blases; }
    [[nodiscard]] isize size() const { return _blases.size(); }

    /// The addresses, for a binding of `buffer[accel.blas_ref]`.
    [[nodiscard]] readonly_buffer_view<gpu_blas_ref> view() const { return _addresses.as_readonly_buffer(); }
    [[nodiscard]] buffer<gpu_blas_ref> const& addresses() const { return _addresses; }

private:
    cc::vector<blas_handle> _blases;
    buffer<gpu_blas_ref> _addresses;
};

/// A fixed list of buffers, held, and a buffer of their addresses a shader binds as `buffer[accel.buffer_ref]`.
/// A shader copies `table[k]` into a cluster's record, and offsets it by bytes, so every address it names is a table's.
/// Each buffer must carry `buffer_usage::accel_structure_build_input`, which a build reads them as.
/// Created through `ctx.persistent.create_buffer_table`; a build naming the table holds it for its list.
class sg::buffer_table
{
public:
    buffer_table(cc::vector<raw_buffer_handle> buffers, buffer<gpu_buffer_ref> addresses)
      : _buffers(cc::move(buffers)), _addresses(cc::move(addresses))
    {
    }

    [[nodiscard]] cc::span<raw_buffer_handle const> buffers() const { return _buffers; }
    [[nodiscard]] isize size() const { return _buffers.size(); }

    /// The addresses, for a binding of `buffer[accel.buffer_ref]`.
    [[nodiscard]] readonly_buffer_view<gpu_buffer_ref> view() const { return _addresses.as_readonly_buffer(); }

private:
    cc::vector<raw_buffer_handle> _buffers;
    buffer<gpu_buffer_ref> _addresses;
};

/// One kind of record a compute pass wrote for an update: `max_count` of room from `offset_in_bytes`, and the count it
/// wrote, a `u32` at `count_offset_in_bytes` of `count`.
/// The count must not exceed `max_count`, which is what sg declares the build reads; a `max_count` of 0 is no records.
struct sg::indirect_records
{
    raw_buffer_handle records = nullptr;
    isize offset_in_bytes = 0;
    int max_count = 0;
    raw_buffer_handle count = nullptr;
    isize count_offset_in_bytes = 0;
};

/// One GPU-written update's records, which apply together and in no order among themselves, as `ptlas_operations` do.
/// A write whose `blas` is zero empties its slot, as a clear does.
struct sg::ptlas_indirect_operations
{
    /// `accel.ptlas_write` records, strided by `sizeof(gpu_ptlas_write)`.
    indirect_records writes;
    /// `accel.ptlas_update` records, strided by `sizeof(gpu_ptlas_update)`.
    indirect_records updates;
    /// `accel.ptlas_translation` records, strided by `sizeof(gpu_ptlas_translation)`.
    indirect_records translations;
    /// Every table a record of the partitioned TLAS may name after this update, including the instances earlier
    /// updates wrote: the structure holds these, and lets go of the tables an earlier update named.
    cc::span<blas_table_handle const> tables = {};
};
