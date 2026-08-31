#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // cc::memcpy
#include <clean-core/container/vector.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/backends/vulkan/vulkan_raytracing_pipeline.hh>
#include <shaped-graphics/backends/vulkan/vulkan_raytracing_shader_table.hh>

namespace sg::backend::vulkan
{
namespace
{
[[nodiscard]] constexpr isize align_up(isize value, isize alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}
} // namespace

cc::result<vulkan_raytracing_shader_table_handle> vulkan_raytracing_shader_table::create(
    vulkan_context& ctx,
    sg::raytracing_shader_table_description const& desc)
{
    CC_ASSERT(desc.pipeline != nullptr, "a shader table needs a raytracing_pipeline");
    CC_ASSERT(!desc.raygen.empty(), "a shader table needs at least one raygen record");

    auto const vk_pipeline = std::dynamic_pointer_cast<vulkan_raytracing_pipeline const>(desc.pipeline);
    CC_ASSERT(vk_pipeline != nullptr, "pipeline is not a vulkan raytracing_pipeline");

    auto const& rt_props = ctx.raytracing_pipeline_properties();
    auto const handle_size = isize(rt_props.shaderGroupHandleSize);
    auto const record_stride = align_up(handle_size, isize(rt_props.shaderGroupHandleAlignment));
    auto const section_alignment = isize(rt_props.shaderGroupBaseAlignment);

    // Four sections, each starting on the base alignment.
    auto const raygen_start = isize(0);
    auto const miss_start = align_up(raygen_start + record_stride * desc.raygen.size(), section_alignment);
    auto const hit_start = align_up(miss_start + record_stride * desc.miss.size(), section_alignment);
    auto const callable_start = align_up(hit_start + record_stride * desc.hit.size(), section_alignment);
    auto const total_size = align_up(callable_start + record_stride * desc.callable.size(), section_alignment);

    // The CPU byte image: each record is the pipeline's stored group handle, at the record stride.
    // A record carries only the handle — sg states that shader tables hold no local root arguments — so the padding
    // between records stays zero.
    auto image = cc::vector<byte>::create_filled(total_size, byte(0));
    auto const write_section = [&](isize start, auto const& handles, auto const& handle_of)
    {
        for (isize i = 0; i < handles.size(); ++i)
        {
            auto const src = handle_of(handles[i]);
            cc::memcpy(image.data() + start + i * record_stride, src.data(), size_t(handle_size));
        }
    };
    write_section(raygen_start, desc.raygen, [&](sg::raygen_shader_handle h) { return vk_pipeline->raygen_handle(h); });
    write_section(miss_start, desc.miss, [&](sg::miss_shader_handle h) { return vk_pipeline->miss_handle(h); });
    write_section(hit_start, desc.hit, [&](sg::hit_shader_handle h) { return vk_pipeline->hit_handle(h); });
    write_section(callable_start, desc.callable,
                  [&](sg::callable_shader_handle h) { return vk_pipeline->callable_handle(h); });

    // The buffer must carry SHADER_BINDING_TABLE_BIT_KHR, which sg::buffer_usage does not model — see the extra-usage
    // parameter's note on create_vulkan_buffer.
    auto buffer_result
        = ctx.create_vulkan_buffer(total_size, sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst,
                                   sg::allocation_info{}, VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR);
    CC_RETURN_IF_ERROR(buffer_result);

    auto table = std::make_shared<vulkan_raytracing_shader_table>(desc.pipeline);
    table->buffer = cc::move(buffer_result.value());

    // Upload through an ordinary command list, so the records are in place before any trace reads them.
    auto cmd_result = ctx.create_vulkan_command_list();
    CC_RETURN_IF_ERROR(cmd_result);
    auto cmd = cc::move(cmd_result.value());
    cmd->upload.bytes_to_buffer(table->buffer, image);
    ctx.submit_vulkan_command_list(cc::move(cmd));

    auto const base = table->buffer->_device_address;
    auto const set_region = [&](VkStridedDeviceAddressRegionKHR& region, isize start, isize count)
    {
        if (count == 0)
            return; // an unused section stays address 0, which a trace reads as absent
        region.deviceAddress = base + VkDeviceAddress(start);
        region.stride = VkDeviceSize(record_stride);
        region.size = VkDeviceSize(record_stride * count);
    };
    set_region(table->raygen_table, raygen_start, desc.raygen.size());
    set_region(table->miss_table, miss_start, desc.miss.size());
    set_region(table->hit_table, hit_start, desc.hit.size());
    set_region(table->callable_table, callable_start, desc.callable.size());

    return vulkan_raytracing_shader_table_handle(cc::move(table));
}

VkStridedDeviceAddressRegionKHR vulkan_raytracing_shader_table::raygen_record(sg::raygen_index index) const
{
    CC_ASSERT(raygen_table.size > 0, "the shader table has no raygen records");
    auto const i = VkDeviceSize(u32(index));
    CC_ASSERT(i * raygen_table.stride < raygen_table.size, "raygen index is out of the table's range");

    // A raygen region names exactly one record, which Vulkan spells as size == stride.
    return VkStridedDeviceAddressRegionKHR{
        .deviceAddress = raygen_table.deviceAddress + i * raygen_table.stride,
        .stride = raygen_table.stride,
        .size = raygen_table.stride,
    };
}
} // namespace sg::backend::vulkan

namespace sg::backend::vulkan
{
cc::result<vulkan_raytracing_shader_table_handle> vulkan_context::create_vulkan_raytracing_shader_table(
    sg::raytracing_shader_table_description const& desc)
{
    return vulkan_raytracing_shader_table::create(*this, desc);
}
} // namespace sg::backend::vulkan
