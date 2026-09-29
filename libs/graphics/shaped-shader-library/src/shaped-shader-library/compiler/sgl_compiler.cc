#include <clean-core/common/assert.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh> // sg::async_compiled_shader is a cc::shared_async
#include <shaped-graphics-language/check/resources.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-shader-library/binding/binding_groups.hh> // slib::inline_constants_space, slib::bound_samplers_space
#include <shaped-shader-library/compiler/sgl_compiler.hh>
#include <shaped-shader-library/impl/pipeline_fields.hh> // the pixel formats by name

using namespace cc::primitive_defines;

namespace
{
// SGL links no sg, so it names each feature as sg does, and this is the one file that sees both lists.
static_assert(
    []
    {
        for (auto const name : sgl::check::k_feature_names)
        {
            auto is_known = false;
            for (auto const f : sg::k_all_features)
                is_known = is_known || sg::to_string(f) == name;
            if (!is_known)
                return false;
        }
        return true;
    }(),
    "every feature SGL knows is an sg::feature of the same name");

[[nodiscard]] sgl::emit::target target_of(sg::shader_format format)
{
    switch (format)
    {
    case sg::shader_format::dxil:
        return sgl::emit::target::hlsl_dx12;
    case sg::shader_format::spirv:
        return sgl::emit::target::hlsl_vulkan;
    case sg::shader_format::wgsl:
        return sgl::emit::target::wgsl;
    case sg::shader_format::metal_lib:
        return sgl::emit::target::msl;
    default:
        CC_UNREACHABLE("SGL is written for dxil, spirv, wgsl and metal_lib compilers only");
    }
}

// sgl names a texture's shape as sg names its view dimension, member for member.
static_assert(
    []
    {
        for (auto i = isize(0); i < isize(sizeof(sgl::check::k_shapes) / sizeof(sgl::check::k_shapes[0])); ++i)
            if (isize(sgl::check::k_shapes[i].shape) != i)
                return false;
        return isize(sgl::check::texture_shape::cube_array) == isize(sg::texture_view_dimension::cube_array);
    }(),
    "sgl::check::texture_shape is sg::texture_view_dimension member for member");

[[nodiscard]] sg::texture_view_dimension dimension_of(cc::string_view name)
{
    for (auto const& info : sgl::check::k_shapes)
        if (info.sg_name == name)
            return sg::texture_view_dimension(info.shape);
    CC_UNREACHABLE("SGL names every texture dimension as sg does");
}

[[nodiscard]] sg::pixel_format pixel_format_of(cc::string_view name)
{
    for (auto const& c : slib::impl::fields::cases_pixel_format)
        if (c.name == name)
            return sg::pixel_format(c.value);
    CC_UNREACHABLE("SGL names every image format as sg does");
}

[[nodiscard]] sg::access_mode access_of(cc::string_view name)
{
    return name == "read_write" ? sg::access_mode::read_write
         : name == "write"      ? sg::access_mode::write
                                : sg::access_mode::read;
}

[[nodiscard]] sg::texture_sample_type sample_type_of(cc::string_view name)
{
    return name == "unfilterable_float" ? sg::texture_sample_type::unfilterable_float
         : name == "depth"              ? sg::texture_sample_type::depth
         : name == "sint"               ? sg::texture_sample_type::sint
         : name == "uint"               ? sg::texture_sample_type::uint
                                        : sg::texture_sample_type::filterable_float;
}

[[nodiscard]] sg::sampler_binding_type sampler_type_of(cc::string_view name)
{
    return name == "comparison"    ? sg::sampler_binding_type::comparison
         : name == "non_filtering" ? sg::sampler_binding_type::non_filtering
                                   : sg::sampler_binding_type::filtering;
}

/// `b` as sg sees it on `format`: dx12 numbers a group as a register space, vulkan and WebGPU as a set.
/// A file-scope sampler is stated as each target's reflection reports the address sg binds a `bound_sampler` at:
/// dx12's `s<i>` of `bound_samplers_space`, SPIR-V's binding i + 1 of the reserved set, slib's WGSL reader's index i
/// of the reserved group, and Metal's sampler slot i.
[[nodiscard]] sg::binding binding_of(sgl::interface_binding const& b, sg::shader_format format, sg::shader_stage stage)
{
    auto result = sg::binding{.name = b.name,
                              .reflected_name = b.emitted == b.name ? cc::string() : b.emitted,
                              .index = u32(b.slot),
                              .count = u32(b.count)};
    auto const is_dx12 = format == sg::shader_format::dxil;
    if (b.is_file_sampler)
    {
        if (is_dx12)
            result.space = slib::bound_samplers_space;
        else if (format == sg::shader_format::spirv || format == sg::shader_format::wgsl)
            result.group_index = u32(sg::reserved_binding_group);
        if (format == sg::shader_format::spirv)
            result.index = u32(b.slot + 1);
    }
    else if (b.is_inline)
    {
        // dx12 reads the inline block at b0 of slib's reserved space; every other backend places it by its own rule.
        if (is_dx12)
            result.space = slib::inline_constants_space;
    }
    else if (is_dx12)
        result.space = u32(b.group);
    else
        result.group_index = u32(b.group);

    switch (b.kind)
    {
    case sgl::described_member_kind::constant:
        result.type = sg::binding_type::constants_buffer;
        // A block is read in rows of 16 bytes, which is the size every target's compiler states it as.
        result.block_size = isize((b.block_size + 15) / 16 * 16);
        break;
    case sgl::described_member_kind::buffer:
        result.type = sg::binding_type::buffer;
        result.access = access_of(b.access);
        break;
    case sgl::described_member_kind::texture:
        result.type = sg::binding_type::texture;
        result.texture_dimension = dimension_of(b.texture_dimension);
        result.sample_type = sample_type_of(b.sample_type);
        break;
    case sgl::described_member_kind::image:
        result.type = sg::binding_type::image;
        result.access = access_of(b.access);
        result.texture_dimension = dimension_of(b.texture_dimension);
        result.image_format = pixel_format_of(b.image_format);
        break;
    case sgl::described_member_kind::sampler:
        result.type = sg::binding_type::sampler;
        result.sampler_type = sampler_type_of(b.sampler_type);
        break;
    case sgl::described_member_kind::acceleration_structure:
        result.type = sg::binding_type::acceleration_structure;
        break;
    }
    result.visibility.set(stage);
    return result;
}

class sgl_shader_compiler final : public slib::shader_compiler
{
public:
    explicit sgl_shader_compiler(std::unique_ptr<slib::shader_compiler> inner)
      : _inner(cc::move(inner)), _target(target_of(_inner->target_format()))
    {
    }

    [[nodiscard]] slib::shader_language source_language() const override { return slib::shader_language::sgl; }
    [[nodiscard]] sg::shader_format target_format() const override { return _inner->target_format(); }

    [[nodiscard]] cc::result<slib::preprocessed_source> preprocess(slib::shader_source_description const& desc,
                                                                   slib::include_resolver resolve) const override
    {
        (void)resolve; // SGL has no include directive

        auto stage = sgl::check::stage::none;
        switch (desc.stage)
        {
        case sg::shader_stage::vertex:
            stage = sgl::check::stage::vertex;
            break;
        case sg::shader_stage::tessellation_control:
            stage = sgl::check::stage::tessellation_control;
            break;
        case sg::shader_stage::tessellation_evaluation:
            stage = sgl::check::stage::tessellation_evaluation;
            break;
        case sg::shader_stage::geometry:
            stage = sgl::check::stage::geometry;
            break;
        case sg::shader_stage::fragment:
            stage = sgl::check::stage::pixel;
            break;
        case sg::shader_stage::compute:
            stage = sgl::check::stage::compute;
            break;
        case sg::shader_stage::raygen:
            stage = sgl::check::stage::raygen;
            break;
        case sg::shader_stage::miss:
            stage = sgl::check::stage::miss;
            break;
        case sg::shader_stage::closest_hit:
            stage = sgl::check::stage::closest_hit;
            break;
        case sg::shader_stage::any_hit:
            stage = sgl::check::stage::any_hit;
            break;
        case sg::shader_stage::intersection:
            stage = sgl::check::stage::intersection;
            break;
        case sg::shader_stage::callable:
            stage = sgl::check::stage::callable;
            break;
        default:
            return cc::error(cc::format("SGL has no entry point of the stage '{}' is declared as", desc.entry_point));
        }

        auto text = sgl::compile_to_text(
            {.source = desc.source,
             .source_name = desc.label.empty() ? cc::string_view("<sgl>") : cc::string_view(desc.label),
             .entry_point = desc.entry_point,
             .stage = stage,
             .target = _target});
        if (text.has_error())
            return cc::error(cc::format("SGL reported errors:\n{}", text.error()));
        auto const& emitted = text.value();
        auto shader = sg::compiled_shader{.stage = desc.stage,
                                          .format = _inner->target_format(),
                                          .entry_point = emitted.entry_point};
        auto result = slib::preprocessed_source{.source = emitted.text, .entry_point = emitted.entry_point};
        for (auto const& b : emitted.bindings)
        {
            auto binding = binding_of(b, shader.format, shader.stage);
            if (b.is_used)
                shader.bindings.push_back(binding);
            result.declared_bindings.push_back(cc::move(binding));
        }
        if (stage == sgl::check::stage::compute)
            shader.workgroup_size
                = sg::compute_dimensions{.x = emitted.workgroup[0], .y = emitted.workgroup[1], .z = emitted.workgroup[2]};
        if (emitted.color_targets >= 0)
            shader.color_output_count = emitted.color_targets;
        shader.target_set = emitted.target_struct;

        auto features = sg::feature_set();
        for (auto i = isize(0); i < sgl::check::k_feature_count; ++i)
            if (emitted.features.has(sgl::check::feature(i)))
                features.set(sg::feature_from_string(sgl::check::k_feature_names[i]).value());
        shader.required_features = features;

        // The words SGL's slots are spelled in, turned into the access sg's barrier tracker declares.
        shader.footprint.source = sg::footprint_source::exact;
        for (auto const& slot : emitted.footprint)
        {
            auto access = sg::access_flags();
            if (slot.view == sgl::check::slot_view::constants)
                access |= sg::access_flag::constants_read;
            else if (slot.reads)
                access |= slot.view == sgl::check::slot_view::storage ? sg::access_flag::storage_read
                                                                      : sg::access_flag::shader_read;
            if (slot.writes)
                access |= sg::access_flag::shader_write;
            shader.footprint.slots.push_back({.name = slot.host_name, .access = access, .dynamic_index = false});
        }
        result.stated = cc::move(shader);
        for (auto const& l : emitted.layouts)
        {
            auto layout = slib::block_layout{.global = l.global, .stride = l.stride};
            for (auto const& f : l.fields)
                layout.fields.push_back({.name = f.name, .offset = f.offset});
            result.layouts.push_back(cc::move(layout));
        }
        return result;
    }

    [[nodiscard]] sg::async_compiled_shader compile(slib::shader_source_description const& desc) const override
    {
        return _inner->compile(desc);
    }

    [[nodiscard]] cc::optional<cc::vector<slib::block_layout>> reflect_layouts(sg::compiled_shader const& shader) const override
    {
        return _inner->reflect_layouts(shader);
    }

private:
    std::unique_ptr<slib::shader_compiler> _inner;
    sgl::emit::target _target;
};
} // namespace

std::unique_ptr<slib::shader_compiler> slib::create_sgl_compiler(std::unique_ptr<shader_compiler> inner)
{
    CC_ASSERT(inner != nullptr, "an SGL compiler needs the compiler that builds its text");
    return std::make_unique<sgl_shader_compiler>(cc::move(inner));
}
