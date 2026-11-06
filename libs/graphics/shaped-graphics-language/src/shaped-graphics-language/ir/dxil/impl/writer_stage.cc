#include "writer.hh"

#include <clean-core/string/format.hh>

using namespace sgl;
using namespace sgl::ir;
using namespace sgl::ir::dxil::impl;

namespace
{
/// A stage input by the number the form states it with, SPIR-V's `BuiltIn`.
struct built_in_rule
{
    i32 number = 0;
    dx_op op = dx_op::thread_id;
    /// Whether the operation takes a component, which a scalar's does not.
    bool has_component = true;
};

constexpr built_in_rule k_built_ins[] = {
    {28, dx_op::thread_id},
    {26, dx_op::group_id},
    {27, dx_op::thread_id_in_group},
    {29, dx_op::flattened_thread_id_in_group, false},
    {36, dx_op::wave_get_lane_count, false},
    {41, dx_op::wave_get_lane_index, false},
};

/// What a stage is to the metadata and the container; only compute is lowered yet, and a stage is a row here.
struct stage_rule
{
    cc::string_view model;
    /// `PSVShaderKind`.
    u32 psv = 0;
    /// `DXIL::ShaderKind`, as a program's version word states it.
    u32 program = 0;
};
constexpr stage_rule k_compute = {.model = "cs", .psv = 5, .program = 5};

/// The shader model every module states; a higher one is a new row of what needs it.
constexpr u32 k_model_major = 6;
constexpr u32 k_model_minor = 6;
constexpr u32 k_tag_shader_flags = 0;
constexpr u32 k_tag_num_threads = 4;
constexpr u32 k_tag_element_type = 0;
constexpr u32 k_tag_structured_stride = 1;

/// A feature a module uses, as the entry's flags and the container's SFI0 state it.
struct feature_rule
{
    bool writer_uses::* used;
    u64 metadata_flag;
    u64 feature_flag;
};
constexpr feature_rule k_features[] = {
    {&writer_uses::structured, 1ull << 4, 0},
    {&writer_uses::double_, 1ull << 2, 1ull << 0},
    {&writer_uses::double_extensions, 1ull << 6, 1ull << 5},
    {&writer_uses::waves, 1ull << 19, 1ull << 14},
    {&writer_uses::int64, 1ull << 20, 1ull << 15},
    {&writer_uses::low_precision, 1ull << 5, 1ull << 18},
    {&writer_uses::typed_uav_load, 1ull << 13, 1ull << 11},
};
/// What every module says: 16-bit types are native, as the text route's `-enable-16bit-types` has them.
constexpr u64 k_native_low_precision = 1ull << 23;
} // namespace

u32 writer::entry_attributes()
{
    // the one float behaviour no operation can state on its own
    if (_p.guarantee != check::float_level::ieee)
        return 0;
    ll_attribute const list[] = {{.key = "fp32-denorm-mode", .value = "preserve"}};
    return _m.attributes(list);
}

void writer::built_in(u32 index, global const& g, instruction const& i)
{
    built_in_rule const* found = nullptr;
    for (auto const& r : k_built_ins)
        if (r.number == g.built_in)
            found = &r;
    if (found == nullptr)
        return unsupported(cc::format("the stage input {} on DXIL", g.built_in));
    auto const number = _m.constant_i32(k_dx_ops[isize(found->op)].number);
    auto const is_wave = found->op == dx_op::wave_get_lane_count || found->op == dx_op::wave_get_lane_index;
    if (is_wave)
        _uses.waves = true;
    auto const overload = is_wave ? _m.void_type() : _m.int_type();
    _scratch.clear();
    if (!found->has_component)
    {
        ll_value const args[] = {number};
        _scratch.push_back(call(found->op, overload, args));
        return define(index);
    }
    for (auto l = u32(0); l < leaves_of(i.type); ++l)
    {
        ll_value const args[] = {number, _m.constant_i32(l)};
        _scratch.push_back(call(found->op, overload, args));
    }
    define(index);
}

void writer::write_entry_metadata()
{
    auto const undefined_pointer = [&](cc::string_view name)
    {
        // a resource's global, of which DXIL keeps the type alone; its layout is what the record states
        ll_type const members[] = {_m.int_type()};
        auto const type = _m.pointer(_m.structure(name, members));
        return _m.md_value(type, _m.constant_undef(type));
    };

    auto lists = cc::vector<u32>();
    u32 heads[4] = {};
    for (auto cls = u32(0); cls < 4; ++cls)
    {
        lists.clear();
        for (auto const& b : _bindings)
        {
            if (b.cls != cls)
                continue;
            auto const type = undefined_pointer(cc::format("sgl.resource.{}.{}", cls, b.id));
            auto fields = cc::vector<u32>();
            fields.push_back(_m.md_i32(b.id));
            fields.push_back(type);
            fields.push_back(_m.md_string(""));
            fields.push_back(_m.md_i32(b.space));
            fields.push_back(_m.md_i32(b.lower));
            fields.push_back(_m.md_i32(1));
            auto extra = u32(0);
            if (b.stride > 0)
            {
                u32 const tags[] = {_m.md_i32(k_tag_structured_stride), _m.md_i32(b.stride)};
                extra = _m.md_node(tags);
            }
            else if (b.component > 0)
            {
                u32 const tags[] = {_m.md_i32(k_tag_element_type), _m.md_i32(b.component)};
                extra = _m.md_node(tags);
            }
            switch (cls)
            {
            case 0: // an SRV: kind, sample count, extra
                fields.push_back(_m.md_i32(b.kind));
                fields.push_back(_m.md_i32(0));
                fields.push_back(extra);
                break;
            case 1: // a UAV: kind, coherent, counter, rasterizer order, extra
                fields.push_back(_m.md_i32(b.kind));
                fields.push_back(_m.md_i1(_p.globals[b.global].is_coherent));
                fields.push_back(_m.md_i1(false));
                fields.push_back(_m.md_i1(false));
                fields.push_back(extra);
                break;
            case 2: // a CBV: size, extra
                fields.push_back(_m.md_i32(b.size));
                fields.push_back(0);
                break;
            default: // a sampler: kind, extra
                fields.push_back(_m.md_i32(0));
                fields.push_back(0);
                break;
            }
            lists.push_back(_m.md_node(fields));
        }
        heads[cls] = lists.empty() ? 0 : _m.md_node(lists);
    }

    u32 const version[] = {_m.md_i32(1), _m.md_i32(k_model_minor)};
    u32 const validator[] = {_m.md_i32(1), _m.md_i32(9)};
    u32 const model[] = {_m.md_string(k_compute.model), _m.md_i32(k_model_major), _m.md_i32(k_model_minor)};
    u32 const resources[] = {heads[0], heads[1], heads[2], heads[3]};
    auto const has_resources = !_bindings.empty();
    auto const resources_node = has_resources ? _m.md_node(resources) : 0;

    auto metadata_flags = k_native_low_precision;
    auto feature_flags = u64(0);
    for (auto const& f : k_features)
        if (_uses.*f.used)
        {
            metadata_flags |= f.metadata_flag;
            feature_flags |= f.feature_flag;
        }

    u32 const threads[]
        = {_m.md_i32(u32(_p.workgroup[0])), _m.md_i32(u32(_p.workgroup[1])), _m.md_i32(u32(_p.workgroup[2]))};
    u32 const properties[]
        = {_m.md_i32(k_tag_shader_flags), _m.md_value(_m.long_type(), _m.constant_int(_m.long_type(), metadata_flags)),
           _m.md_i32(k_tag_num_threads), _m.md_node(threads)};
    u32 const entry[] = {_m.md_value(_m.type_of(_entry), _entry), _m.md_string(_p.entry_name), 0, resources_node,
                         _m.md_node(properties)};

    u32 const version_node[] = {_m.md_node(version)};
    u32 const validator_node[] = {_m.md_node(validator)};
    u32 const model_node[] = {_m.md_node(model)};
    u32 const resources_list[] = {resources_node};
    u32 const entries[] = {_m.md_node(entry)};
    _m.md_named("dx.version", version_node);
    _m.md_named("dx.valver", validator_node);
    _m.md_named("dx.shaderModel", model_node);
    if (has_resources)
        _m.md_named("dx.resources", resources_list);
    _m.md_named("dx.entryPoints", entries);

    _summary.metadata_flags = metadata_flags;
    _summary.feature_flags = feature_flags;
    _summary.psv_stage = k_compute.psv;
    _summary.program_kind = k_compute.program;
    _summary.program_version = (k_model_major << 4) | k_model_minor;
    _summary.entry_name = _p.entry_name;
    // the PSV lists a module's bindings in this order, whichever order they were found in
    for (auto const cls : {2u, 3u, 0u, 1u})
        for (auto const& b : _bindings)
            if (b.cls == cls)
                _summary.resources.push_back(
                    {.psv_type = b.psv_type, .space = b.space, .lower = b.lower, .upper = b.lower, .kind = b.kind});
    for (auto axis = 0; axis < 3; ++axis)
        _summary.workgroup[axis] = _p.workgroup[axis];
}

// ---- DXIL's vocabulary

ll_type writer::handle_type()
{
    if (_handle == ll_type::none)
    {
        ll_type const members[] = {_m.pointer(_m.byte_type())};
        _handle = _m.structure("dx.types.Handle", members);
    }
    return _handle;
}

ll_type writer::res_bind_type()
{
    if (_res_bind == ll_type::none)
    {
        ll_type const members[] = {_m.int_type(), _m.int_type(), _m.int_type(), _m.byte_type()};
        _res_bind = _m.structure("dx.types.ResBind", members);
    }
    return _res_bind;
}

ll_type writer::res_props_type()
{
    if (_res_props == ll_type::none)
    {
        ll_type const members[] = {_m.int_type(), _m.int_type()};
        _res_props = _m.structure("dx.types.ResourceProperties", members);
    }
    return _res_props;
}

ll_type writer::dimensions_type()
{
    if (_dimensions == ll_type::none)
    {
        ll_type const members[] = {_m.int_type(), _m.int_type(), _m.int_type(), _m.int_type()};
        _dimensions = _m.structure("dx.types.Dimensions", members);
    }
    return _dimensions;
}

cc::string_view writer::overload_name(module_builder const& m, ll_type overload)
{
    auto const bits = m.bits_of(overload);
    if (m.is_float(overload))
        return bits == 16 ? "f16" : bits == 64 ? "f64" : "f32";
    return bits == 1 ? "i1" : bits == 16 ? "i16" : bits == 64 ? "i64" : "i32";
}

u8 writer::overload_mask(module_builder const& m, ll_type overload)
{
    auto const bits = m.bits_of(overload);
    if (m.is_float(overload))
        return bits == 16 ? k_f16 : bits == 64 ? k_f64 : k_f32;
    return bits == 1 ? k_i1 : bits == 16 ? k_i16 : bits == 64 ? k_i64 : k_i32;
}

ll_type writer::res_ret_type(ll_type overload)
{
    ll_type const members[] = {overload, overload, overload, overload, _m.int_type()};
    return _m.structure(cc::format("dx.types.ResRet.{}", overload_name(_m, overload)), members);
}

ll_type writer::cbuf_ret_type(ll_type overload)
{
    auto const bits = _m.bits_of(overload);
    // a row of 16 bytes, in as many lanes as fit
    ll_type const members[] = {overload, overload, overload, overload, overload, overload, overload, overload};
    auto const lanes = bits == 64 ? 2 : bits == 16 ? 8 : 4;
    // a 16-bit row's name says its lanes, which no other's does
    auto const name = bits == 16 ? cc::format("dx.types.CBufRet.{}.8", overload_name(_m, overload))
                                 : cc::format("dx.types.CBufRet.{}", overload_name(_m, overload));
    return _m.structure(name, cc::span<ll_type const>(members, lanes));
}

ll_type writer::signature_type(char code, ll_type overload)
{
    switch (code)
    {
    case 'o':
        return overload;
    case 'v':
        return _m.void_type();
    case 'c':
        return _m.bool_type();
    case 'b':
        return _m.byte_type();
    case 'i':
        return _m.int_type();
    case 'f':
        return _m.float_type();
    case 'h':
        return handle_type();
    case 'B':
        return res_bind_type();
    case 'P':
        return res_props_type();
    case 'R':
        return res_ret_type(overload);
    case 'C':
        return cbuf_ret_type(overload);
    case 'D':
        return dimensions_type();
    default:
        return ll_type::none;
    }
}

ll_value writer::op_function(dx_op op, ll_type overload)
{
    auto const& info = k_dx_ops[isize(op)];
    auto const& shape = k_dx_classes[isize(info.shape)];
    auto const has_overload = info.overloads != k_no_overload;
    auto const key = (u64(info.shape) << 32) | (has_overload ? u64(u32(overload)) + 1 : 0u);
    for (auto const& known : _functions)
        if (known.key == key)
            return known.function;

    auto params = cc::vector<ll_type>();
    params.push_back(_m.int_type());
    auto const signature = shape.signature;
    for (auto k = isize(2); k < signature.size(); ++k)
        params.push_back(signature_type(signature[k], overload));
    auto const type = _m.function(signature_type(signature[0], overload), params);
    auto const name = has_overload ? cc::format("dx.op.{}.{}", shape.name, overload_name(_m, overload))
                                   : cc::format("dx.op.{}", shape.name);

    auto attributes = u32(0);
    switch (shape.attr)
    {
    case dx_class::no_attr:
    {
        ll_attribute const list[] = {{.kind = k_attr_nounwind}};
        attributes = _m.attributes(list);
        break;
    }
    case dx_class::read_none:
    {
        ll_attribute const list[] = {{.kind = k_attr_nounwind}, {.kind = k_attr_readnone}};
        attributes = _m.attributes(list);
        break;
    }
    case dx_class::read_only:
    {
        ll_attribute const list[] = {{.kind = k_attr_nounwind}, {.kind = k_attr_readonly}};
        attributes = _m.attributes(list);
        break;
    }
    case dx_class::no_duplicate:
    {
        ll_attribute const list[] = {{.kind = k_attr_noduplicate}, {.kind = k_attr_nounwind}};
        attributes = _m.attributes(list);
        break;
    }
    }
    auto const function = _m.add_function(name, type, attributes, true);
    _functions.push_back({.key = key, .function = function, .type = type});
    return function;
}

ll_value writer::call(dx_op op, ll_type overload, cc::span<ll_value const> args, bool is_stable)
{
    auto const& info = k_dx_ops[isize(op)];
    if (info.overloads != k_no_overload && (info.overloads & overload_mask(_m, overload)) == 0)
    {
        unsupported(cc::format("{} of {}", k_dx_classes[isize(info.shape)].name, overload_name(_m, overload)));
        return _m.constant_undef(_m.type_of(args.back()));
    }
    auto const function = op_function(op, overload);
    auto const key = (u64(info.shape) << 32) | (info.overloads != k_no_overload ? u64(u32(overload)) + 1 : 0u);
    for (auto const& known : _functions)
        if (known.key == key)
            return _m.call(function, known.type, args, is_stable);
    return ll_value::none;
}
