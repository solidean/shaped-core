#include <clean-core/common/assert.hh>
#include <clean-core/string/format.hh>
#include <shaped-shader-compiler-dxc/impl/reflection.hh>
#include <spirv_reflect.h>

namespace ssc::dxc::impl
{
namespace
{
/// The sg binding kind a SPIR-V descriptor type means, or nothing for one sg has no vocabulary for yet.
///
/// The read-only / read-write split is the one place this is not a plain rename.
/// SPIR-V has a single STORAGE_BUFFER type, because in Vulkan the distinction is the shader's own NonWritable
/// decoration rather than a different descriptor — so the two sg kinds are told apart by that decoration below, and
/// the raw / structured split by whether the block has members.
[[nodiscard]] cc::optional<sg::binding_type> map_descriptor_type(SpvReflectDescriptorBinding const& b, bool writable)
{
    switch (b.descriptor_type)
    {
    case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        return sg::binding_type::uniform_buffer;
    case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER:
        return sg::binding_type::sampler;
    case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        return sg::binding_type::readonly_texture;
    case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        return sg::binding_type::readwrite_texture;
    case SPV_REFLECT_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
        return sg::binding_type::acceleration_structure;
    case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER:
    {
        // A ByteAddressBuffer reflects as a block with no members; a StructuredBuffer<T> has T's members.
        bool const raw = b.block.member_count == 0;
        if (writable)
            return raw ? sg::binding_type::readwrite_raw_buffer : sg::binding_type::readwrite_structured_buffer;
        return raw ? sg::binding_type::readonly_raw_buffer : sg::binding_type::readonly_structured_buffer;
    }
    default:
        // Texel buffers, input attachments and the dynamic variants have no sg spelling yet.
        // Failing is deliberate: silently dropping a binding leaves a group layout that does not match the shader.
        return {};
    }
}

/// The sg view dimension a sampled or storage image declares.
/// A texture binding needs it so a backend can synthesize a dimension-correct null descriptor for a vacant element.
[[nodiscard]] cc::optional<sg::texture_view_dimension> map_image_dimension(SpvReflectImageTraits const& image)
{
    bool const arrayed = image.arrayed != 0;
    bool const ms = image.ms != 0;
    switch (image.dim)
    {
    case SpvDim1D:
        return arrayed ? sg::texture_view_dimension::tex_1d_array : sg::texture_view_dimension::tex_1d;
    case SpvDim2D:
        if (ms)
            return arrayed ? sg::texture_view_dimension::tex_2d_ms_array : sg::texture_view_dimension::tex_2d_ms;
        return arrayed ? sg::texture_view_dimension::tex_2d_array : sg::texture_view_dimension::tex_2d;
    case SpvDim3D:
        return sg::texture_view_dimension::tex_3d;
    case SpvDimCube:
        return arrayed ? sg::texture_view_dimension::cube_array : sg::texture_view_dimension::cube;
    default:
        return {};
    }
}

/// The entry point matching `name`, or the module's only one when the name is empty.
[[nodiscard]] SpvReflectEntryPoint const* find_entry_point(SpvReflectShaderModule const& module, cc::string_view name)
{
    if (name.empty())
        return module.entry_point_count > 0 ? &module.entry_points[0] : nullptr;
    for (uint32_t i = 0; i < module.entry_point_count; ++i)
        if (cc::string_view(module.entry_points[i].name) == name)
            return &module.entry_points[i];
    return nullptr;
}
} // namespace

cc::result<reflected_shader> reflect_spirv(cc::span<byte const> spirv, sg::shader_stage stage, cc::string_view entry_point)
{
    SpvReflectShaderModule module = {};
    if (auto const r = spvReflectCreateShaderModule(size_t(spirv.size()), spirv.data(), &module);
        r != SPV_REFLECT_RESULT_SUCCESS)
        return cc::error(cc::format("SPIR-V reflection failed to parse the module (code {})", int(r)));

    // The module owns heap allocations, so every exit below has to go through the destroy.
    struct module_guard
    {
        SpvReflectShaderModule* m;
        ~module_guard() { spvReflectDestroyShaderModule(m); }
    } const guard{&module};

    reflected_shader out;

    // Bindings are enumerated for the whole module rather than per entry point: a DXC-emitted module carries one
    // entry point, and a binding's set/binding pair does not vary by which one reads it.
    uint32_t count = 0;
    if (auto const r = spvReflectEnumerateDescriptorBindings(&module, &count, nullptr); r != SPV_REFLECT_RESULT_SUCCESS)
        return cc::error(cc::format("SPIR-V reflection failed to count descriptor bindings (code {})", int(r)));

    auto bindings = cc::vector<SpvReflectDescriptorBinding*>::create_defaulted(isize(count));
    if (count > 0)
        if (auto const r = spvReflectEnumerateDescriptorBindings(&module, &count, bindings.data());
            r != SPV_REFLECT_RESULT_SUCCESS)
            return cc::error(cc::format("SPIR-V reflection failed to read descriptor bindings (code {})", int(r)));

    for (auto const* b : bindings)
    {
        CC_ASSERT(b != nullptr, "SPIRV-Reflect returned a null binding");

        // NonWritable on the block is what a read-only storage buffer carries; SPIR-V has no separate descriptor for it.
        bool const writable = (b->block.decoration_flags & SPV_REFLECT_DECORATION_NON_WRITABLE) == 0;
        auto const type = map_descriptor_type(*b, writable);
        if (!type.has_value())
            return cc::error(cc::format("SPIR-V binding '{}' uses a descriptor kind sg has no vocabulary for yet",
                                        b->name != nullptr ? b->name : "<unnamed>"));

        sg::binding out_binding;
        out_binding.name = b->name != nullptr ? cc::string(b->name) : cc::string();
        out_binding.index = b->binding;

        // The set is a hardware-visible descriptor set, so it fills group_index.
        // `space` stays absent because SPIR-V has no register-numbering namespace at all.
        // libs/graphics/shaped-graphics/src/shaped-graphics/binding/binding.hh says why the two are not
        // interchangeable: filling the wrong one gives a group layout a backend cannot bind.
        out_binding.group_index = b->set;

        // An unbounded array reflects as a zero-length dimension, which is sg's `count == 0`.
        out_binding.count = b->count;
        out_binding.type = type.value();

        if (b->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
            out_binding.block_size = isize(b->block.size);
        if (out_binding.type == sg::binding_type::readonly_texture
            || out_binding.type == sg::binding_type::readwrite_texture)
            out_binding.texture_dimension = map_image_dimension(b->image);

        out.bindings.push_back(cc::move(out_binding));
    }

    // A push-constant block is sg's `inline_constants`, and it is NOT a descriptor — it lives in no set, so the
    // enumeration above never sees it.
    // Reported as a uniform_buffer binding with neither a group_index nor a space, which is what tells a caller
    // apart from a `cbuffer` in a descriptor set: that one always carries its set.
    //
    // Without this an HLSL shader written for both backends cannot use inline constants at all — the DXIL arm
    // reflects a root-constant register and the SPIR-V arm reflected nothing.
    uint32_t push_count = 0;
    if (auto const r = spvReflectEnumeratePushConstantBlocks(&module, &push_count, nullptr);
        r != SPV_REFLECT_RESULT_SUCCESS)
        return cc::error(cc::format("SPIR-V reflection failed to count push-constant blocks (code {})", int(r)));

    if (push_count > 0)
    {
        auto blocks = cc::vector<SpvReflectBlockVariable*>::create_defaulted(isize(push_count));
        if (auto const r = spvReflectEnumeratePushConstantBlocks(&module, &push_count, blocks.data());
            r != SPV_REFLECT_RESULT_SUCCESS)
            return cc::error(cc::format("SPIR-V reflection failed to read push-constant blocks (code {})", int(r)));

        // Vulkan allows several blocks only across stages, and DXC emits at most one per module — so more than one
        // here is a shader sg has no way to describe, and saying so beats picking the first.
        if (push_count > 1)
            return cc::error(cc::format("SPIR-V module declares {} push-constant blocks; sg's inline constants are one",
                                        push_count));

        auto const* const block = blocks[0];
        CC_ASSERT(block != nullptr, "SPIRV-Reflect returned a null push-constant block");

        sg::binding out_binding;
        out_binding.name = block->name != nullptr ? cc::string(block->name) : cc::string();
        out_binding.type = sg::binding_type::uniform_buffer;
        out_binding.block_size = isize(block->size);
        out.bindings.push_back(cc::move(out_binding));
    }

    if (sg::is_compute_stage(stage))
    {
        auto const* const entry = find_entry_point(module, entry_point);
        if (entry == nullptr)
            return cc::error(cc::format("SPIR-V module declares no entry point named '{}'", entry_point));
        out.workgroup_size = sg::compute_dimensions{
            .x = int(entry->local_size.x),
            .y = int(entry->local_size.y),
            .z = int(entry->local_size.z),
        };
    }

    return out;
}
} // namespace ssc::dxc::impl
