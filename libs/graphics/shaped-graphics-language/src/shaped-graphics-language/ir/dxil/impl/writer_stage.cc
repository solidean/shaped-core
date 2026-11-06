#include "writer.hh"

#include <clean-core/string/format.hh>

#include <initializer_list>

using namespace sgl;
using namespace sgl::ir;
using namespace sgl::ir::dxil::impl;

namespace
{
/// What DXIL reads a stage input with.
struct built_in_rule
{
    ir::built_in input = ir::built_in::none;
    dx_op op = dx_op::thread_id;
    /// Whether the operation takes a component, which a scalar's does not.
    bool has_component = true;
};

constexpr built_in_rule k_built_ins[] = {
    {ir::built_in::thread_id, dx_op::thread_id},
    {ir::built_in::workgroup_id, dx_op::group_id},
    {ir::built_in::local_thread_id, dx_op::thread_id_in_group},
    {ir::built_in::local_thread_index, dx_op::flattened_thread_id_in_group, false},
    {ir::built_in::subgroup_size, dx_op::wave_get_lane_count, false},
    {ir::built_in::subgroup_invocation_id, dx_op::wave_get_lane_index, false},
};

/// What a stage is to the metadata and the container; a stage is a row here.
struct stage_rule
{
    check::stage stage = check::stage::none;
    cc::string_view model;
    /// `PSVShaderKind`, and `DXIL::ShaderKind` as a program's version word states it, which number stages alike.
    u32 kind = 0;
};
constexpr stage_rule k_stages[] = {
    {check::stage::compute, "cs", 5},
    {check::stage::vertex, "vs", 1},
    {check::stage::pixel, "ps", 0},
};

/// What a signature's element is to `dx.sig` and `PSV0`, beside the `D3D_NAME` the packer gives.
struct system_rule
{
    u32 d3d_name = 0;
    /// `DXIL::SemanticKind`, which `PSV0` numbers the same.
    u32 kind = 0;
};
constexpr system_rule k_systems[] = {
    {0, 0},   // an ordinary link or attribute
    {1, 3},   // SV_Position
    {6, 1},   // SV_VertexID
    {8, 2},   // SV_InstanceID
    {64, 16}, // SV_Target
};

/// `DXIL::InterpolationMode`, parallel to `ir::interpolation`.
constexpr u32 k_modes[] = {0, 2, 3, 6, 4, 5, 7, 1};

/// The shader model a module states; a higher one is what a module's `writer_uses` asks for.
constexpr u32 k_model_major = 6;
constexpr u32 k_model_minor = 6;
constexpr u32 k_model_minor_start_location = 8;
constexpr u32 k_tag_shader_flags = 0;
constexpr u32 k_tag_usage_mask = 3;
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
    {&writer_uses::start_location, 1ull << 38, 1ull << 32},
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

void writer::built_in(u32 index, stage_element const& e, instruction const& i)
{
    built_in_rule const* found = nullptr;
    for (auto const& r : k_built_ins)
        if (r.input == e.system)
            found = &r;
    if (found == nullptr)
        return unsupported(cc::format("the stage input {} on DXIL", int(e.system)));
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

void writer::number_signature()
{
    _summary.signature = pack_signature(_p);
    _summary.usage.resize_to_filled(_summary.signature.size(), u8(0));
    _element_signature.resize_to_filled(_p.stage_elements.size(), -1);
    u32 next[3] = {};
    for (auto s = isize(0); s < _summary.signature.size(); ++s)
    {
        auto const& e = _summary.signature[s];
        _element_signature[e.element] = i32(s);
        _signature_ids.push_back(next[isize(e.table)]++);
        system_rule const* system = nullptr;
        for (auto const& r : k_systems)
            if (r.d3d_name == e.system_value)
                system = &r;
        if (system == nullptr)
            unsupported(cc::format("the signature element {} on DXIL", e.name));
        _summary.semantic_kinds.push_back(system == nullptr ? u8(0) : u8(system->kind));
        _summary.modes.push_back(u8(k_modes[isize(e.mode)]));
    }
}

void writer::stage_load(u32 index, instruction const& i)
{
    auto const element = _p.operands_of(i)[0];
    auto const& e = _p.stage_elements[element];
    if (_p.stage == check::stage::compute)
        return built_in(index, e, i);

    auto const start_of = [&](dx_op op)
    {
        _uses.start_location = true;
        ll_value const args[] = {_m.constant_i32(k_dx_ops[isize(op)].number)};
        return call(op, _m.int_type(), args);
    };
    if (e.system == ir::built_in::first_instance)
    {
        _scratch.clear();
        _scratch.push_back(start_of(dx_op::start_instance_location));
        return define(index);
    }

    auto const at = _element_signature[element];
    if (at < 0)
        return unsupported(cc::format("the stage input {} on DXIL", _p.name_at(e.name_first, e.name_size)));
    auto const overload = scalar_type(scalar_at(i.type, 0));
    auto const number = _m.constant_i32(k_dx_ops[isize(dx_op::load_input)].number);
    auto const undefined = _m.constant_undef(_m.int_type());
    auto const id = _m.constant_i32(_signature_ids[at]);
    _loaded.clear();
    for (auto l = u32(0); l < leaves_of(i.type); ++l)
    {
        ll_value const args[] = {number, id, _m.constant_i32(0), _m.constant_i8(l), undefined};
        _loaded.push_back(call(dx_op::load_input, overload, args, true));
    }
    _scratch.clear();
    // EMIT-128: HLSL counts a vertex and an instance from the draw's base, and DXIL's own counts from zero
    if (e.system == ir::built_in::vertex_index)
        _scratch.push_back(_m.binary(binary_op::add, _loaded[0], start_of(dx_op::start_vertex_location)));
    else if (e.system == ir::built_in::instance_index)
        _scratch.push_back(_m.binary(binary_op::add, _loaded[0], start_of(dx_op::start_instance_location)));
    else
        _scratch.push_back_range(_loaded);
    define(index);
}

void writer::stage_store(instruction const& i)
{
    auto const operands = _p.operands_of(i);
    auto const at = _element_signature[operands[0]];
    if (at < 0)
    {
        auto const& e = _p.stage_elements[operands[0]];
        return unsupported(cc::format("the stage output {} on DXIL", _p.name_at(e.name_first, e.name_size)));
    }
    auto const value = value_ref(operands[1]);
    auto const number = _m.constant_i32(k_dx_ops[isize(dx_op::store_output)].number);
    auto const id = _m.constant_i32(_signature_ids[at]);
    for (auto l = u32(0); l < count_of(value); ++l)
    {
        auto const lane = leaf_at(value, l);
        ll_value const args[] = {number, id, _m.constant_i32(0), _m.constant_i8(l), lane};
        (void)call(dx_op::store_output, _m.type_of(lane), args);
    }
}

void writer::read_usage()
{
    for (auto const& f : _functions)
    {
        auto const shape = dx_class_id(f.key >> 32);
        if (shape != dx_class_id::load_input && shape != dx_class_id::store_output)
            continue;
        auto const table = shape == dx_class_id::load_input ? signature_table::input : signature_table::output;
        _m.calls_of(f.function,
                    [&](cc::span<u32 const> args)
                    {
                        u64 id = 0;
                        u64 lane = 0;
                        if (!_m.constant_bits(ll_value(args[1]), id) || !_m.constant_bits(ll_value(args[3]), lane))
                            return;
                        for (auto s = isize(0); s < _summary.signature.size(); ++s)
                            if (_summary.signature[s].table == table && _signature_ids[s] == u32(id))
                                _summary.usage[s] |= u8(1u << u32(lane));
                    });
    }
}

void writer::fill_dependencies()
{
    auto const extent = [&](signature_table table, u32& rows, u32& scalars)
    {
        for (auto const& e : _summary.signature)
            if (e.table == table && e.row >= 0)
            {
                rows = cc::max(rows, u32(e.row) + 1);
                scalars = cc::max(scalars, u32(e.row) * 4 + e.first_column + e.lanes);
            }
    };
    extent(signature_table::input, _summary.input_rows, _summary.input_scalars);
    extent(signature_table::output, _summary.output_rows, _summary.output_scalars);

    // Every declared input lane reaches every declared output lane: a table can safely say too much.
    auto const words = (_summary.output_scalars + 31) / 32;
    _summary.dependencies.resize_to_filled(isize(_summary.input_rows) * 4 * words, 0u);
    for (auto const& input : _summary.signature)
    {
        if (input.table != signature_table::input || input.row < 0)
            continue;
        for (auto const& output : _summary.signature)
        {
            if (output.table != signature_table::output || output.row < 0)
                continue;
            for (auto i = u32(0); i < input.lanes; ++i)
                for (auto o = u32(0); o < output.lanes; ++o)
                {
                    auto const to = u32(output.row) * 4 + output.first_column + o;
                    auto const from = u32(input.row) * 4 + input.first_column + i;
                    _summary.dependencies[isize(from) * words + to / 32] |= 1u << (to % 32);
                }
        }
    }
}

u32 writer::signature_list(signature_table table)
{
    auto const byte_of = [&](u32 value) { return _m.md_value(_m.byte_type(), _m.constant_i8(value)); };
    auto list = cc::vector<u32>();
    for (auto s = isize(0); s < _summary.signature.size(); ++s)
    {
        auto const& e = _summary.signature[s];
        if (e.table != table)
            continue;
        // `DXIL::ComponentType` of `DxilProgramSigCompType`: unsigned, signed, float
        auto const component = e.component_type == 3 ? 9u : e.component_type == 2 ? 4u : 5u;
        u32 const indices[] = {_m.md_i32(e.semantic_index)};
        auto extra = u32(0);
        if (_summary.usage[s] != 0)
        {
            u32 const tags[] = {_m.md_i32(k_tag_usage_mask), _m.md_i32(_summary.usage[s])};
            extra = _m.md_node(tags);
        }
        u32 const fields[] = {_m.md_i32(_signature_ids[s]),
                              _m.md_string(e.name),
                              byte_of(component),
                              byte_of(_summary.semantic_kinds[s]),
                              _m.md_node(indices),
                              byte_of(_summary.modes[s]),
                              _m.md_i32(1),
                              byte_of(e.lanes),
                              _m.md_i32(u32(e.row)),
                              byte_of(e.first_column),
                              extra};
        list.push_back(_m.md_node(fields));
    }
    return list.empty() ? 0 : _m.md_node(list);
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

    stage_rule const* rule = nullptr;
    for (auto const& r : k_stages)
        if (r.stage == _p.stage)
            rule = &r;
    if (rule == nullptr)
        return unsupported(cc::format("a {} stage on DXIL", check::stage_name(_p.stage)));
    auto const is_compute = _p.stage == check::stage::compute;
    auto const model_minor = _uses.start_location ? k_model_minor_start_location : k_model_minor;

    u32 const version[] = {_m.md_i32(1), _m.md_i32(model_minor)};
    u32 const validator[] = {_m.md_i32(1), _m.md_i32(9)};
    u32 const model[] = {_m.md_string(rule->model), _m.md_i32(k_model_major), _m.md_i32(model_minor)};
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
    auto signatures = u32(0);
    if (!is_compute)
    {
        fill_dependencies();
        u32 const lists_of_signature[]
            = {signature_list(signature_table::input), signature_list(signature_table::output), 0};
        signatures = _m.md_node(lists_of_signature);
        if (!_failure.empty())
            return;

        // `dx.viewIdState`: the counts of scalars, then one row per input scalar up to the last
        auto const words = (_summary.output_scalars + 31) / 32;
        auto state = cc::vector<ll_value>();
        state.push_back(_m.constant_i32(_summary.input_scalars));
        state.push_back(_m.constant_i32(_summary.output_scalars));
        for (auto k = u32(0); k < _summary.input_scalars * words; ++k)
            state.push_back(_m.constant_i32(_summary.dependencies[k]));
        auto const array = _m.array(_m.int_type(), u64(state.size()));
        u32 const state_node[] = {_m.md_node(
            cc::span<u32 const>(std::initializer_list<u32>{_m.md_value(array, _m.constant_struct(array, state))}))};
        _m.md_named("dx.viewIdState", state_node);
    }
    auto const properties_count = is_compute ? isize(4) : isize(2);
    u32 const entry[] = {_m.md_value(_m.type_of(_entry), _entry), _m.md_string(_p.entry_name), signatures,
                         resources_node, _m.md_node(cc::span<u32 const>(properties, properties_count))};

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
    _summary.psv_stage = rule->kind;
    _summary.program_kind = rule->kind;
    _summary.program_version = (k_model_major << 4) | model_minor;
    _summary.dxil_version = 0x100 | model_minor;
    _summary.entry_name = _p.entry_name;
    if (_p.stage == check::stage::vertex)
        for (auto const& e : _summary.signature)
            _summary.writes_position
                = _summary.writes_position || (e.table == signature_table::output && e.system_value == 1);
    // the PSV lists a module's bindings in this order, whichever order they were found in
    for (auto const cls : {2u, 3u, 0u, 1u})
        for (auto const& b : _bindings)
            if (b.cls == cls)
                _summary.resources.push_back(
                    {.psv_type = b.psv_type, .space = b.space, .lower = b.lower, .upper = b.lower, .kind = b.kind});
    if (is_compute)
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
