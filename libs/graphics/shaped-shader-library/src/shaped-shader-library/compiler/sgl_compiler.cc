#include <clean-core/common/assert.hh>
#include <clean-core/container/byte_stream_builder.hh>
#include <clean-core/container/pinned_data.hh>
#include <clean-core/math/float.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh> // sg::async_compiled_shader is a cc::shared_async
#include <shaped-graphics-language/check/resources.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-shader-library/binding/binding_groups.hh> // slib::inline_constants_space, slib::bound_samplers_space
#include <shaped-shader-library/compiler/sgl_compiler.hh>
#include <shaped-shader-library/impl/pipeline_fields.hh> // the pixel formats by name
#include <shaped-shader-library/impl/sgl_emit_cache.hh>
#include <shaped-shader-library/impl/sgl_library.hh>
#include <shaped-shader-library/impl/sgl_module_artifacts.hh>

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
    case sgl::described_member_kind::bytes:
        result.type = sg::binding_type::bytes;
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
    /// `inner` may be null for spirv alone, which SGL then writes itself and nothing compiles.
    sgl_shader_compiler(std::unique_ptr<slib::shader_compiler> inner,
                        cc::optional<bcache::blob_cache*> persistent_cache,
                        bool diagnostics,
                        sgl::check::undefined_guard guards_undefined,
                        bool keeps_module_artifacts)
      : _inner(cc::move(inner)),
        _format(_inner != nullptr ? _inner->target_format() : sg::shader_format::spirv),
        _target(target_of(_format)),
        _persistent_cache(persistent_cache),
        _diagnostics(diagnostics),
        _guards_undefined(guards_undefined),
        _keeps_module_artifacts(keeps_module_artifacts)
    {
    }

    [[nodiscard]] slib::shader_language source_language() const override { return slib::shader_language::sgl; }
    [[nodiscard]] sg::shader_format target_format() const override { return _format; }

    [[nodiscard]] cc::result<slib::preprocessed_source> preprocess(slib::shader_source_description const& desc,
                                                                   slib::include_resolver resolve) const override
    {
        (void)resolve; // SGL has no include directive
        return slib::impl::cached_sgl_preprocess(
            key_of(desc), [&] { return preprocess_now(desc); }, slib::impl::sgl_emit_store(_persistent_cache));
    }

    [[nodiscard]] sg::async_compiled_shader compile(slib::shader_source_description const& desc) const override
    {
        // SGL wrote the binary itself, so nothing is left to compile and nothing can crash doing it
        if (!desc.bytecode.empty())
            return cc::make_async_from_value(sg::compiled_shader{
                .stage = desc.stage,
                .format = _format,
                .entry_point = desc.entry_point,
                .bytecode = cc::make_pinned_data(cc::span<byte const>(desc.bytecode)),
                .compiler = {.name = cc::string("sgl"),
                             .version = cc::string(sgl::compiler_fingerprint()),
                             .signature = cc::string(_format == sg::shader_format::dxil ? "dxil" : "spirv")}});
        if (_inner == nullptr)
            return cc::make_async_from_error<sg::compiled_shader>(cc::async_error::make_error(cc::any_error(cc::format(
                "'{}' has no binary of SGL's own, and this build has no DXC to compile its text", desc.entry_point))));
        // EMIT-4: what the emitted text needs of the compiler that builds it, whoever wired that compiler
        auto inner = desc;
        if (_target == sgl::emit::target::hlsl_dx12 || _target == sgl::emit::target::hlsl_vulkan)
            // a half is `float16_t`, which DXC compiles only with 16-bit types on, and SGL writes no spelling the flag changes
            inner.dxc_args.push_back(cc::string("-enable-16bit-types"));
        if (_target == sgl::emit::target::msl)
            // `coherent(device)` and texture atomics, which a metallib compiles from MSL 3.2 on
            inner.metal_language_version = "metal3.2";
        return _inner->compile(inner);
    }

    [[nodiscard]] cc::shared_async<cc::unit> prefetch_modules(cc::function_ref<slib::module_library()> read) const override
    {
        auto* const store = _keeps_module_artifacts ? slib::impl::sgl_emit_store(_persistent_cache) : nullptr;
        if (store == nullptr)
            return cc::make_async_from_value(cc::unit{});
        auto const modules = read();
        return slib::impl::prefetch_sgl_modules(modules.files(), *store);
    }

    [[nodiscard]] cc::optional<cc::vector<slib::block_layout>> reflect_layouts(sg::compiled_shader const& shader) const override
    {
        if (_inner == nullptr)
            return {};
        return _inner->reflect_layouts(shader);
    }

private:
    /// Everything `preprocess_now` reads of `desc`, and the target it writes for.
    /// The label is in it, since the front end may quote the name it is given.
    [[nodiscard]] cc::hash128 key_of(slib::shader_source_description const& desc) const
    {
        auto& b = cc::byte_stream_builder::thread_local_scratch();
        b.add_string(desc.source);
        b.add_string(desc.entry_point);
        b.add_pod(desc.stage);
        b.add_string(desc.label);
        b.add_pod(_target);
        b.add_pod(_diagnostics);
        b.add_pod(_guards_undefined);
        b.add_pod(desc.software_trace);
        b.add_pod(desc.native_spirv);
        b.add_pod(desc.native_dxil);
        b.add_pod(i64(desc.options.size()));
        for (auto const& o : desc.options)
        {
            b.add_string(o.name);
            b.add_string(o.value);
        }
        b.add_pod(i64(desc.modules.size()));
        for (auto const& m : desc.modules)
        {
            b.add_string(m.path);
            b.add_string(m.text);
        }
        return cc::hash128::create(b.written_bytes(), 0);
    }

    /// SGL's front end and emitter, translated into what slib takes: what the cache stores.
    [[nodiscard]] cc::result<slib::preprocessed_source> preprocess_now(slib::shader_source_description const& desc) const
    {
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
        case sg::shader_stage::mesh:
            stage = sgl::check::stage::mesh;
            break;
        case sg::shader_stage::task:
            stage = sgl::check::stage::task;
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

        // shader_asset::acquire says through this name what the device has, which is the compile's to know and no
        // option of the source; an sg feature no shader asks about is no name SGL knows, and is left out
        auto options = cc::vector<sgl::check::option_value>();
        auto device_features = sgl::check::feature_set();
        auto weakens_float = false;
        auto native_line_table = false;
        auto mesh = sgl::emit::mesh_form::native;
        auto polyfill_vertex = cc::string();
        auto polyfill_task = cc::string();
        auto polyfill_control = cc::string();
        auto polyfill_pass = cc::string();
        auto const is_spirv = _target == sgl::emit::target::hlsl_vulkan;
        auto const is_dxil = _target == sgl::emit::target::hlsl_dx12;
        auto const requires_native
            = (is_spirv && (_inner == nullptr || desc.native_spirv)) || (is_dxil && desc.native_dxil);
        auto native = requires_native ? sgl::emit::native_route::require : sgl::emit::native_route::text;
        for (auto const& o : desc.options)
        {
            if (o.name == slib::k_weakens_float_option)
            {
                weakens_float = o.value == "true";
                continue;
            }
            if (o.name == slib::k_native_line_table_option)
            {
                native_line_table = o.value == "true";
                continue;
            }
            if (o.name == slib::k_mesh_form_option)
            {
                mesh = o.value == "compute"  ? sgl::emit::mesh_form::compute
                     : o.value == "vertex"   ? sgl::emit::mesh_form::vertex_fetch
                     : o.value == "scan"     ? sgl::emit::mesh_form::scan
                     : o.value == "launched" ? sgl::emit::mesh_form::launched
                                             : sgl::emit::mesh_form::native;
                continue;
            }
            if (o.name == slib::k_polyfill_task_option)
            {
                polyfill_task = o.value;
                continue;
            }
            if (o.name == slib::k_polyfill_vertex_option)
            {
                polyfill_vertex = o.value;
                continue;
            }
            if (o.name == slib::k_polyfill_control_option)
            {
                polyfill_control = o.value;
                continue;
            }
            if (o.name == slib::k_polyfill_pass_option)
            {
                polyfill_pass = o.value;
                continue;
            }
            if (o.name == slib::k_native_spirv_option || o.name == slib::k_native_dxil_option)
            {
                auto const is_for_this = o.name == slib::k_native_spirv_option ? is_spirv : is_dxil;
                if (native == sgl::emit::native_route::text && is_for_this)
                    native = sgl::emit::native_route::prefer;
                continue;
            }
            if (o.name != slib::k_device_features_option)
            {
                options.push_back({.name = o.name, .value = o.value});
                continue;
            }
            auto rest = cc::string_view(o.value);
            while (!rest.empty())
            {
                auto end = isize(0);
                while (end < rest.size() && rest[end] != ',')
                    ++end;
                if (auto const index = sgl::check::find_feature(rest.subview({.offset = 0, .size = end})); index >= 0)
                    device_features.set(sgl::check::feature(index));
                rest = end < rest.size() ? rest.subview(end + 1) : cc::string_view();
            }
        }
        // A hit's cluster needs Shader Model 6.10 on dx12, which the pinned DXC lacks and SGL's DXIL refuses, so a dx12
        // compile reads -1 for it until the preview DXC is pinned (the plan's D8), rather than failing every trace.
        if (is_dxil)
            device_features.remove(sgl::check::feature::clustered_geometry);
        auto const library = slib::impl::sgl_library_of(desc.modules);
        auto text = sgl::compile_to_text(
            {.source = desc.source,
             .source_name = desc.label.empty() ? cc::string_view("<sgl>") : cc::string_view(desc.label),
             .library = library,
             // CHK-527: a polyfill's pass of the prelude in place of the asset's own entry point
             .entry_point = polyfill_pass.empty() ? cc::string_view(desc.entry_point) : cc::string_view(polyfill_pass),
             .stage = polyfill_pass.empty() ? stage : sgl::check::stage::compute,
             .target = _target,
             .options = options,
             // EMIT-182: DXC's and Metal's messages, and PIX's and Xcode's source view, name the SGL line
             .line_directives = true,
             .diagnostics = _diagnostics,
             .guards_undefined = _guards_undefined,
             .software_trace = desc.software_trace,
             .weakens_float = weakens_float,
             .device_features = device_features,
             .native = native,
             .native_line_table = native_line_table,
             .mesh = mesh,
             .polyfill_vertex = polyfill_vertex,
             .polyfill_task = polyfill_task,
             .polyfill_control = polyfill_control});
        // what the front end checked rather than loaded, kept for the next process
        if (auto* const store = _keeps_module_artifacts ? slib::impl::sgl_emit_store(_persistent_cache) : nullptr;
            store != nullptr)
            slib::impl::keep_made_sgl_modules(*store);
        if (text.has_error())
            return cc::error(cc::format("SGL reported errors:\n{}", text.error()));
        auto const& emitted = text.value();
        // CHK-524: a polyfilled mesh stage's half is the stage it became, which the compile after this one builds; the
        // pixel stage after it stays one
        auto const is_polyfilled = desc.stage == sg::shader_stage::mesh || desc.stage == sg::shader_stage::geometry
                                || desc.stage == sg::shader_stage::task
                                || desc.stage == sg::shader_stage::tessellation_control
                                || desc.stage == sg::shader_stage::tessellation_evaluation;
        auto const form_stage = !polyfill_pass.empty()                     ? sg::shader_stage::compute
                              : !is_polyfilled                             ? desc.stage
                              : mesh == sgl::emit::mesh_form::compute      ? sg::shader_stage::compute
                              : mesh == sgl::emit::mesh_form::scan         ? sg::shader_stage::compute
                              : mesh == sgl::emit::mesh_form::launched     ? sg::shader_stage::compute
                              : mesh == sgl::emit::mesh_form::vertex_fetch ? sg::shader_stage::vertex
                                                                           : desc.stage;
        auto shader = sg::compiled_shader{.stage = form_stage, .format = _format, .entry_point = emitted.entry_point};
        if (emitted.mesh_layout.has_value())
        {
            auto const& l = emitted.mesh_layout.value();
            shader.mesh_polyfill_corners = l.corners;
            shader.mesh_polyfill_primitives = l.max_primitives;
            shader.mesh_polyfill_words = l.group_words;
        }
        if (emitted.tessellation_layout.has_value())
        {
            auto const& l = emitted.tessellation_layout.value();
            shader.tessellation_domain = l.domain;
            shader.tessellation_control_points = l.control_points;
            shader.tessellation_patch_words = l.patch_words();
            shader.tessellation_vertex_words = l.vertex_words;
            shader.tessellation_partitioning = l.partitioning;
            shader.tessellation_output = l.output;
        }
        if (emitted.task_layout.has_value())
        {
            shader.task_polyfill_record_words = emitted.task_layout.value().record_words();
            shader.task_polyfill_meshes = emitted.task_layout.value().max_meshes;
        }
        auto result = slib::preprocessed_source{.source = emitted.text,
                                                .entry_point = emitted.entry_point,
                                                .used_modules = emitted.library_files,
                                                .warnings = emitted.warnings,
                                                .native_gap = emitted.native_gap};
        if (!emitted.spirv.empty())
            result.bytecode = cc::vector<byte>::create_copy_of(cc::span<u32 const>(emitted.spirv).as_bytes());
        if (!emitted.dxil.empty())
            result.bytecode = cc::vector<byte>::create_copy_of(cc::span<byte const>(emitted.dxil));
        // EMIT-185: DXC keeps the special values and the order only with every float operation strict, which in
        // SPIR-V is SignedZeroInfNanPreserve and NoContraction, and denormals only where it is asked to preserve them
        auto const is_hlsl = _target == sgl::emit::target::hlsl_dx12 || _target == sgl::emit::target::hlsl_vulkan;
        if (is_hlsl && emitted.float_guarantee != sgl::check::float_level::fast)
            result.dxc_args.push_back(cc::string("-Gis"));
        if (is_hlsl && emitted.float_guarantee == sgl::check::float_level::ieee)
        {
            result.dxc_args.push_back(cc::string("-denorm"));
            result.dxc_args.push_back(cc::string("preserve"));
        }
        for (auto const& b : emitted.bindings)
        {
            auto binding = binding_of(b, shader.format, shader.stage);
            if (b.is_used)
                shader.bindings.push_back(binding);
            result.declared_bindings.push_back(cc::move(binding));
        }
        // a mesh and a task stage's workgroup too, which Metal's mesh draw states and no MSL text does; and a polyfilled
        // stage's compute form's, whatever stage it was declared as
        if (form_stage == sg::shader_stage::compute
            || ((stage == sgl::check::stage::mesh || stage == sgl::check::stage::task)
                && form_stage != sg::shader_stage::vertex))
            shader.workgroup_size
                = sg::compute_dimensions{.x = emitted.workgroup[0], .y = emitted.workgroup[1], .z = emitted.workgroup[2]};
        if (emitted.preferred_subgroup_size > 0)
            shader.preferred_subgroup_size = emitted.preferred_subgroup_size;
        shader.is_dispatched_by_threads = emitted.is_dispatched_by_threads;
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
        // EMIT-183: each print and assert as sg reads its records back
        auto const file_of = [&](sgl::emit::source_line const& l)
        { return l.file >= 0 && l.file < emitted.line_files.size() ? emitted.line_files[l.file] : cc::string(); };
        for (auto const& site : emitted.debug_sites)
        {
            auto described = sg::shader_debug_site{.id = site.id,
                                                   .is_assert = site.is_assert,
                                                   .pieces = site.pieces,
                                                   .at = {.file = file_of(site.origin.at), .line = site.origin.at.line}};
            for (auto const& value : site.values)
            {
                auto const scalar = value.family == "float" ? sg::debug_scalar::float32
                                  : value.family == "half"  ? sg::debug_scalar::float16
                                  : value.family == "uint"  ? sg::debug_scalar::uint32
                                  : value.family == "bool"  ? sg::debug_scalar::boolean
                                                            : sg::debug_scalar::int32;
                described.values.push_back({.scalar = scalar, .lanes = value.lanes, .type = value.type});
            }
            for (auto const& line : site.origin.inlined_from)
                described.inlined_from.push_back({.file = file_of(line), .line = line.line});
            shader.debug_sites.push_back(cc::move(described));
        }
        // EMIT-181: what a backend compiling the text itself, webgpu's, reports a message at
        if (shader.format == sg::shader_format::wgsl)
            for (auto const& line : emitted.lines)
            {
                auto mapped = sg::shader_source_line{.emitted = line.emitted,
                                                     .at = {.file = file_of(line.at), .line = line.at.line}};
                for (auto const& call : line.inlined_from)
                    mapped.inlined_from.push_back({.file = file_of(call), .line = call.line});
                shader.source_map.push_back(cc::move(mapped));
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

    std::unique_ptr<slib::shader_compiler> _inner;
    sg::shader_format _format;
    sgl::emit::target _target;
    /// As `sgl_compiler_options` holds it, resolved at each compile so the default follows whatever is installed then.
    cc::optional<bcache::blob_cache*> _persistent_cache = {};
    bool _diagnostics = false;
    sgl::check::undefined_guard _guards_undefined = sgl::check::undefined_guard::none;
    bool _keeps_module_artifacts = false;
};
} // namespace

slib::shader_option slib::option_of(cc::string_view name, bool value)
{
    return {.name = cc::string(name), .value = cc::string(value ? "true" : "false")};
}

slib::shader_option slib::option_of(cc::string_view name, int value)
{
    return {.name = cc::string(name), .value = cc::format("{}", value)};
}

slib::shader_option slib::option_of(cc::string_view name, u32 value)
{
    return {.name = cc::string(name), .value = cc::format("{}", value)};
}

slib::shader_option slib::option_of(cc::string_view name, float value)
{
    CC_ASSERT(cc::is_finite(value), "an option's float is finite");
    auto text = cc::to_string(value);
    if (!text.contains('.') && !text.contains('e'))
        text += ".0";
    return {.name = cc::string(name), .value = cc::move(text)};
}

slib::shader_option slib::option_of(cc::string_view name, sg::pixel_format value)
{
    for (auto const& c : slib::impl::fields::cases_pixel_format)
        if (c.value == int(value))
            return {.name = cc::string(name), .value = cc::format(".{}", c.name)};
    CC_UNREACHABLE("every sg::pixel_format has a name");
}

static_assert(int(slib::sgl_undefined_guard::abort) == int(sgl::check::undefined_guard::abort)
              && int(slib::sgl_undefined_guard::report) == int(sgl::check::undefined_guard::report)
              && int(slib::sgl_undefined_guard::define) == int(sgl::check::undefined_guard::define));

std::unique_ptr<slib::shader_compiler> slib::create_sgl_compiler(std::unique_ptr<shader_compiler> inner,
                                                                 sgl_compiler_options const& options)
{
    CC_ASSERT(inner != nullptr, "an SGL compiler needs the compiler that builds its text");
    return std::make_unique<sgl_shader_compiler>(cc::move(inner), options.persistent_cache, options.diagnostics,
                                                 sgl::check::undefined_guard(options.guards_undefined),
                                                 options.keeps_module_artifacts);
}

std::unique_ptr<slib::shader_compiler> slib::create_sgl_spirv_compiler(sgl_compiler_options const& options)
{
    return std::make_unique<sgl_shader_compiler>(nullptr, options.persistent_cache, options.diagnostics,
                                                 sgl::check::undefined_guard(options.guards_undefined),
                                                 options.keeps_module_artifacts);
}
