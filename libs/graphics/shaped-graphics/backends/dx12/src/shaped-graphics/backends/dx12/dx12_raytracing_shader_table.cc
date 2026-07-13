#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/backends/dx12/dx12_buffer.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_raytracing_pipeline.hh>
#include <shaped-graphics/backends/dx12/dx12_raytracing_shader_table.hh>
#include <shaped-graphics/context.persistent.hh>
#include <shaped-graphics/context.upload.hh>
#include <shaped-graphics/types.hh>

#include <cstring>

namespace sg::backend::dx12
{
namespace
{
[[nodiscard]] constexpr cc::isize align_up(cc::isize value, cc::isize alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}
} // namespace

cc::result<dx12_raytracing_shader_table_handle> dx12_raytracing_shader_table::create(
    dx12_context& ctx,
    sg::raytracing_shader_table_description const& desc)
{
    auto const pipeline = std::dynamic_pointer_cast<dx12_raytracing_pipeline const>(desc.pipeline);
    CC_ASSERT(pipeline != nullptr, "shader table pipeline is not a dx12 raytracing pipeline");
    CC_ASSERT(!desc.raygen.empty(), "shader table requires at least one raygen record");

    // Validate every handle indexes a shader the pipeline registered.
    for (auto h : desc.raygen)
        CC_ASSERT(cc::u32(h) < cc::u32(pipeline->shader_ids_raygen.size()), "raygen handle out of range");
    for (auto h : desc.miss)
        CC_ASSERT(cc::u32(h) < cc::u32(pipeline->shader_ids_miss.size()), "miss handle out of range");
    for (auto h : desc.hit)
        CC_ASSERT(cc::u32(h) < cc::u32(pipeline->shader_ids_hit.size()), "hit handle out of range");
    for (auto h : desc.callable)
        CC_ASSERT(cc::u32(h) < cc::u32(pipeline->shader_ids_callable.size()), "callable handle out of range");

    // A record holds only the 32-byte identifier; records align to 32, sections to 64.
    constexpr cc::isize record_size = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
    cc::isize const stride = align_up(record_size, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
    auto const section_size
        = [stride](cc::isize count) { return align_up(count * stride, D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT); };

    cc::isize const raygen_section = section_size(desc.raygen.size());
    cc::isize const miss_section = section_size(desc.miss.size());
    cc::isize const hit_section = section_size(desc.hit.size());
    cc::isize const callable_section = section_size(desc.callable.size());

    cc::isize const raygen_start = 0;
    cc::isize const miss_start = raygen_start + raygen_section;
    cc::isize const hit_start = miss_start + miss_section;
    cc::isize const callable_start = hit_start + hit_section;
    cc::isize const total = callable_start + callable_section;

    // Assemble the CPU byte image: each record is the pipeline's stored identifier at the record's stride.
    auto image = cc::vector<cc::byte>::create_filled(total, cc::byte{0});
    auto const write_section
        = [&](cc::isize start, auto const& handles, cc::vector<dx12_raytracing_pipeline::shader_identifier> const& ids)
    {
        cc::isize offset = start;
        for (auto h : handles)
        {
            std::memcpy(image.data() + offset, ids[cc::isize(cc::u32(h))].bytes, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
            offset += stride;
        }
    };
    write_section(raygen_start, desc.raygen, pipeline->shader_ids_raygen);
    write_section(miss_start, desc.miss, pipeline->shader_ids_miss);
    write_section(hit_start, desc.hit, pipeline->shader_ids_hit);
    write_section(callable_start, desc.callable, pipeline->shader_ids_callable);

    // TODO: back this with a dedicated shader_binding_table usage once types.hh grows one; a plain
    // shader-readable + copy-dst buffer is a temporary stand-in.
    auto buffer_result
        = ctx.persistent.try_create_raw_buffer(total, sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
    CC_RETURN_IF_ERROR(buffer_result);
    sg::raw_buffer_handle const raw = cc::move(buffer_result.value());

    // Stream the records in on the async copy queue; a later dispatch that reads the buffer waits on the copy.
    ctx.upload.bytes_to_buffer(raw, cc::make_pinned_data(cc::span<cc::byte const>(image)));

    auto const dx_buffer = std::dynamic_pointer_cast<dx12_buffer const>(raw);
    CC_ASSERT(dx_buffer != nullptr, "shader table buffer is not a dx12 buffer");
    D3D12_GPU_VIRTUAL_ADDRESS const base = dx_buffer->gpu_virtual_address();

    // Build through a non-const pointer (the returned handle is shared_ptr<...const>).
    std::shared_ptr<dx12_raytracing_shader_table> table(new dx12_raytracing_shader_table(desc.pipeline));
    table->buffer = dx_buffer;

    auto const set_range
        = [base, stride](D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE& range, cc::isize start, cc::isize count)
    {
        if (count <= 0)
            return;
        range.StartAddress = base + D3D12_GPU_VIRTUAL_ADDRESS(start);
        range.SizeInBytes = UINT64(count * stride);
        range.StrideInBytes = UINT64(stride);
    };
    set_range(table->raygen_table, raygen_start, desc.raygen.size());
    set_range(table->miss_table, miss_start, desc.miss.size());
    set_range(table->hit_table, hit_start, desc.hit.size());
    set_range(table->callable_table, callable_start, desc.callable.size());

    return dx12_raytracing_shader_table_handle(cc::move(table));
}

D3D12_GPU_VIRTUAL_ADDRESS_RANGE dx12_raytracing_shader_table::raygen_record(sg::raygen_index index) const
{
    CC_ASSERT(raygen_table.StartAddress != 0, "shader table has no raygen section");
    cc::u64 const i = cc::u64(cc::u32(index));
    CC_ASSERT(i * raygen_table.StrideInBytes < raygen_table.SizeInBytes, "raygen index out of range");
    D3D12_GPU_VIRTUAL_ADDRESS_RANGE range = {};
    range.StartAddress = raygen_table.StartAddress + i * raygen_table.StrideInBytes;
    range.SizeInBytes = raygen_table.StrideInBytes;
    return range;
}
} // namespace sg::backend::dx12
