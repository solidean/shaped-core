#include "writer.hh"

#include <clean-core/math/bit.hh>
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

[[nodiscard]] constexpr bool is_factor(ir::built_in b)
{
    return b == ir::built_in::tess_edge || b == ir::built_in::tess_inside;
}

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
    {check::stage::geometry, "gs", 2},
    {check::stage::tessellation_control, "hs", 3},
    {check::stage::tessellation_evaluation, "ds", 4},
    {check::stage::mesh, "ms", 13},
    {check::stage::task, "as", 14},
    {check::stage::raygen, "lib", 7},
    {check::stage::miss, "lib", 11},
    {check::stage::closest_hit, "lib", 10},
    {check::stage::any_hit, "lib", 9},
    {check::stage::intersection, "lib", 8},
    {check::stage::callable, "lib", 12},
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
    {4, 4},   // SV_RenderTargetArrayIndex
    {7, 10},  // SV_PrimitiveID
    {9, 13},  // SV_IsFrontFace
    {10, 12}, // SV_SampleIndex
    {65, 17}, // SV_Depth
    {66, 14}, // SV_Coverage
    {67, 19}, // SV_DepthGreaterEqual
    {68, 18}, // SV_DepthLessEqual
    {11, 25}, // SV_TessFactor of a quad
    {12, 26}, // SV_InsideTessFactor of a quad
    {13, 25}, // SV_TessFactor of a triangle
    {14, 26}, // SV_InsideTessFactor of a triangle
    {15, 25}, // SV_TessFactor of a line: its detail
    {16, 25}, // and its density
};

/// `DXIL::InterpolationMode`, parallel to `ir::interpolation`.
constexpr u32 k_modes[] = {0, 2, 3, 6, 4, 5, 7, 1};

/// The shader model a module states; a higher one is what a module's `writer_uses` asks for.
constexpr u32 k_model_major = 6;
constexpr u32 k_model_minor = 6;
constexpr u32 k_model_minor_start_location = 8;
/// `allocateRayQuery2`, which a query that sees micromaps takes, and every hit object operation.
constexpr u32 k_model_minor_micromaps = 9;
/// Shader model 6.7 on, a module with UAVs states that resources may not alias.
constexpr u64 k_flag_res_may_not_alias = 1ull << 33;
constexpr u32 k_tag_shader_flags = 0;
constexpr u32 k_tag_usage_mask = 3;
constexpr u32 k_tag_geometry_state = 1;
constexpr u32 k_tag_domain_state = 2;
constexpr u32 k_tag_hull_state = 3;
constexpr u32 k_tag_num_threads = 4;
constexpr u32 k_tag_mesh_state = 9;
constexpr u32 k_tag_task_state = 10;
/// `DXIL::MeshOutputTopology`.
constexpr u32 k_mesh_lines = 1;
constexpr u32 k_mesh_triangles = 2;
constexpr u32 k_tag_element_type = 0;
constexpr u32 k_tag_structured_stride = 1;
constexpr u32 k_tag_atomic64_use = 3;
/// `PSVResourceFlag::UsedByAtomic64`.
constexpr u32 k_psv_used_by_atomic64 = 1;

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
    {&writer_uses::view_id, 1ull << 21, 1ull << 16},
    {&writer_uses::layer_output, 1ull << 9, 1ull << 13},
    {&writer_uses::uavs_at_every_stage, 1ull << 16, 1ull << 2},
    {&writer_uses::ray_query, 1ull << 25, 1ull << 20},
    {&writer_uses::many_uavs, 1ull << 15, 1ull << 3},
    {&writer_uses::int64_shared_atomics, 1ull << 28, 1ull << 23},
};
/// What every module says: 16-bit types are native, as the text route's `-enable-16bit-types` has them.
constexpr u64 k_native_low_precision = 1ull << 23;

/// The producer every module names in `!llvm.ident`, which LLVM leaves optional and AMD's driver does not.
/// Without it AMD's DXIL compiler crashes in CreateComputePipelineState; the string itself may even be empty.
/// docs/bugs-external/amd-d3d12-dxil-without-llvm-ident-crashes has the reproduction.
void name_producer(module_builder& m)
{
    u32 const ident[] = {m.md_node(cc::span<u32 const>(std::initializer_list<u32>{m.md_string("sgl")}))};
    m.md_named("llvm.ident", ident);
}
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
        _signature_ids.push_back(e.continues ? next[isize(e.table)] - 1 : next[isize(e.table)]++);
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
    if (is_library())
        return ray_element(index, e, i);
    if (_p.stage == check::stage::compute || _p.stage == check::stage::task || _p.stage == check::stage::mesh)
        return built_in(index, e, i);

    auto const value_of = [&](dx_op op)
    {
        ll_value const args[] = {_m.constant_i32(k_dx_ops[isize(op)].number)};
        return call(op, _m.int_type(), args);
    };
    auto const start_of = [&](dx_op op)
    {
        _uses.start_location = true;
        return value_of(op);
    };
    if (e.system == ir::built_in::first_instance)
    {
        _scratch.clear();
        _scratch.push_back(start_of(dx_op::start_instance_location));
        return define(index);
    }
    // these are operations of their own, whether or not a signature names them
    auto const is_primitive_id = e.system == ir::built_in::primitive_id && _p.stage != check::stage::pixel;
    if (e.system == ir::built_in::view_index || e.system == ir::built_in::sample_mask
        || e.system == ir::built_in::sample_index || is_primitive_id)
    {
        _uses.view_id = _uses.view_id || e.system == ir::built_in::view_index;
        _scratch.clear();
        _scratch.push_back(value_of(e.system == ir::built_in::view_index    ? dx_op::view_id
                                    : e.system == ir::built_in::sample_mask ? dx_op::coverage
                                    : is_primitive_id                       ? dx_op::primitive_id
                                                                            : dx_op::sample_index));
        return define(index);
    }
    if (e.system == ir::built_in::domain_location)
    {
        auto const number = _m.constant_i32(k_dx_ops[isize(dx_op::domain_location)].number);
        _scratch.clear();
        for (auto l = u32(0); l < leaves_of(i.type); ++l)
        {
            ll_value const args[] = {number, _m.constant_i8(l)};
            _scratch.push_back(call(dx_op::domain_location, _m.float_type(), args));
        }
        return define(index);
    }

    auto const at = _element_signature[element];
    if (at < 0)
        return unsupported(cc::format("the stage input {} on DXIL", _p.name_at(e.name_first, e.name_size)));
    if (e.part == stage_part::patch)
    {
        auto const overload = scalar_type(scalar_at(i.type, 0));
        auto const number = _m.constant_i32(k_dx_ops[isize(dx_op::load_patch_constant)].number);
        auto const id = _m.constant_i32(_signature_ids[at]);
        auto const row = _m.constant_i32(is_factor(e.system) ? e.semantic_index : 0);
        _scratch.clear();
        for (auto l = u32(0); l < leaves_of(i.type); ++l)
        {
            ll_value const args[] = {number, id, row, _m.constant_i8(l)};
            _scratch.push_back(call(dx_op::load_patch_constant, overload, args, true));
        }
        return define(index);
    }
    // `SV_IsFrontFace` crosses as a uint
    auto const is_bool = e.system == ir::built_in::is_front_facing;
    auto const overload = is_bool ? _m.int_type() : scalar_type(scalar_at(i.type, 0));
    auto const number = _m.constant_i32(k_dx_ops[isize(dx_op::load_input)].number);
    auto const operands = _p.operands_of(i);
    // the input vertex or control point, which DXIL calls the vertex axis
    auto const axis = operands.size() > 1 ? leaf_at(value_ref(operands[1])) : _m.constant_undef(_m.int_type());
    auto const id = _m.constant_i32(_signature_ids[at]);
    _loaded.clear();
    for (auto l = u32(0); l < leaves_of(i.type); ++l)
    {
        ll_value const args[] = {number, id, _m.constant_i32(0), _m.constant_i8(l), axis};
        _loaded.push_back(call(dx_op::load_input, overload, args, true));
    }
    _scratch.clear();
    // EMIT-128: HLSL counts a vertex and an instance from the draw's base, and DXIL's own counts from zero
    if (e.system == ir::built_in::vertex_index)
        _scratch.push_back(_m.binary(binary_op::add, _loaded[0], start_of(dx_op::start_vertex_location)));
    else if (e.system == ir::built_in::instance_index)
        _scratch.push_back(_m.binary(binary_op::add, _loaded[0], start_of(dx_op::start_instance_location)));
    else if (is_bool)
        _scratch.push_back(_m.compare(predicate::i_ne, _loaded[0], _m.constant_i32(0)));
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
    auto const& e = _p.stage_elements[operands[0]];
    auto const is_mesh = _p.stage == check::stage::mesh;
    auto const op = is_mesh
                      ? (e.part == stage_part::primitive ? dx_op::store_primitive_output : dx_op::store_vertex_output)
                  : e.part == stage_part::patch ? dx_op::store_patch_constant
                                                : dx_op::store_output;
    auto const value = value_ref(operands[1]);
    auto const number = _m.constant_i32(k_dx_ops[isize(op)].number);
    auto const id = _m.constant_i32(_signature_ids[at]);
    auto const row = _m.constant_i32(is_factor(e.system) ? e.semantic_index : 0);
    for (auto l = u32(0); l < count_of(value); ++l)
    {
        auto const lane = leaf_at(value, l);
        ll_value const args[]
            = {number, id, row, _m.constant_i8(l), lane, is_mesh ? leaf_at(value_ref(operands[2])) : ll_value::none};
        (void)call(op, _m.type_of(lane), cc::span<ll_value const>(args, is_mesh ? 6 : 5));
    }
}

void writer::set_mesh_counts(instruction const& i)
{
    auto const operands = _p.operands_of(i);
    ll_value const args[] = {_m.constant_i32(k_dx_ops[isize(dx_op::set_mesh_output_counts)].number),
                             leaf_at(value_ref(operands[0])), leaf_at(value_ref(operands[1]))};
    (void)call(dx_op::set_mesh_output_counts, _m.void_type(), args);
}

void writer::write_primitive(instruction const& i)
{
    auto const operands = _p.operands_of(i);
    auto const corners = value_ref(operands[1]);
    // a line's third index is unused, and DXC writes it as zero
    ll_value const args[] = {_m.constant_i32(k_dx_ops[isize(dx_op::emit_indices)].number),
                             leaf_at(value_ref(operands[0])), leaf_at(corners, 0), leaf_at(corners, 1),
                             _p.primitive.writes_lines ? _m.constant_i32(0) : leaf_at(corners, 2)};
    (void)call(dx_op::emit_indices, _m.void_type(), args);
}

void writer::write_hull_function()
{
    _m.begin_function();
    _m.open_block(_m.add_block());
    ll_value const point_args[] = {_m.constant_i32(k_dx_ops[isize(dx_op::output_control_point_id)].number)};
    auto const point = call(dx_op::output_control_point_id, _m.int_type(), point_args);
    auto const load_number = _m.constant_i32(k_dx_ops[isize(dx_op::load_input)].number);
    auto const store_number = _m.constant_i32(k_dx_ops[isize(dx_op::store_output)].number);
    // the k-th arrayed input is the k-th output: both are the one control point struct, member by member
    auto output = isize(0);
    for (auto i = isize(0); i < _p.stage_elements.size(); ++i)
    {
        auto const& e = _p.stage_elements[i];
        if (e.part != stage_part::input || e.array_size == 0)
            continue;
        while (_p.stage_elements[output].part != stage_part::output)
            ++output;
        auto const from = _m.constant_i32(_signature_ids[_element_signature[i]]);
        auto const to = _m.constant_i32(_signature_ids[_element_signature[output]]);
        auto const overload = scalar_type(scalar_at(e.type, 0));
        for (auto l = u32(0); l < leaves_of(e.type); ++l)
        {
            ll_value const load_args[] = {load_number, from, _m.constant_i32(0), _m.constant_i8(l), point};
            auto const value = call(dx_op::load_input, overload, load_args, true);
            ll_value const store_args[] = {store_number, to, _m.constant_i32(0), _m.constant_i8(l), value};
            (void)call(dx_op::store_output, overload, store_args);
        }
        ++output;
    }
    _m.return_void();
}

void writer::discard()
{
    ll_value const args[] = {_m.constant_i32(k_dx_ops[isize(dx_op::discard)].number), _m.constant_bool(true)};
    (void)call(dx_op::discard, _m.void_type(), args);
}

void writer::derivative_lanes(u32 index, instruction const& i)
{
    auto const source = value_ref(_p.operands_of(i)[0]);
    auto const overload = _m.type_of(leaf_at(source));
    auto const across = [&](dx_op op, ll_value lane)
    {
        ll_value const args[] = {_m.constant_i32(k_dx_ops[isize(op)].number), lane};
        return call(op, overload, args);
    };
    auto const kind = derivative(i.sub);
    _loaded.clear();
    for (auto l = u32(0); l < count_of(source); ++l)
    {
        auto const lane = leaf_at(source, l);
        if (kind != derivative::width)
        {
            auto const op = kind == derivative::x      ? dx_op::deriv_coarse_x
                          : kind == derivative::y      ? dx_op::deriv_coarse_y
                          : kind == derivative::x_fine ? dx_op::deriv_fine_x
                                                       : dx_op::deriv_fine_y;
            _loaded.push_back(across(op, lane));
            continue;
        }
        auto const x = across(dx_op::fabs, across(dx_op::deriv_coarse_x, lane));
        auto const y = across(dx_op::fabs, across(dx_op::deriv_coarse_y, lane));
        _loaded.push_back(_m.binary(binary_op::add, x, y, flags_of(i)));
    }
    _scratch.clear();
    _scratch.push_back_range(_loaded);
    define(index);
}

void writer::read_usage()
{
    for (auto const& f : _functions)
    {
        auto const shape = dx_class_id(f.key >> 32);
        if (shape != dx_class_id::load_input && shape != dx_class_id::store_output
            && shape != dx_class_id::load_patch_constant && shape != dx_class_id::store_patch_constant
            && shape != dx_class_id::store_vertex_output && shape != dx_class_id::store_primitive_output)
            continue;
        // all of them name an id and then a row and a lane, so one walk reads them
        auto const table = shape == dx_class_id::load_input ? signature_table::input
                         : shape == dx_class_id::store_output || shape == dx_class_id::store_vertex_output
                             ? signature_table::output
                             : signature_table::patch;
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
    extent(signature_table::patch, _summary.patch_rows, _summary.patch_scalars);

    auto const each_scalar = [&](signature_table table, auto&& visit)
    {
        for (auto const& e : _summary.signature)
            if (e.table == table && e.row >= 0)
                for (auto l = u32(0); l < e.lanes; ++l)
                    visit(u32(e.row) * 4 + e.first_column + l);
    };
    // Every declared lane reaches every declared lane after it: a table can safely say too much.
    auto const table_of = [&](signature_table from, u32 from_rows, signature_table to, u32 to_scalars)
    {
        auto const words = (to_scalars + 31) / 32;
        auto table = cc::vector<u32>::create_filled(isize(from_rows) * 4 * words, 0u);
        each_scalar(
            from, [&](u32 a) { each_scalar(to, [&](u32 b) { table[isize(a) * words + b / 32] |= 1u << (b % 32); }); });
        return table;
    };
    auto const view_mask = [&](signature_table table, u32 scalars)
    {
        auto mask = cc::vector<u32>::create_filled(_uses.view_id ? isize((scalars + 31) / 32) : isize(0), 0u);
        if (_uses.view_id)
            each_scalar(table, [&](u32 to) { mask[to / 32] |= 1u << (to % 32); });
        return mask;
    };
    _summary.uses_view = _uses.view_id;
    _summary.view_outputs = view_mask(signature_table::output, _summary.output_scalars);
    _summary.dependencies
        = table_of(signature_table::input, _summary.input_rows, signature_table::output, _summary.output_scalars);
    if (_p.stage == check::stage::tessellation_control || _p.stage == check::stage::mesh)
    {
        _summary.view_patch = view_mask(signature_table::patch, _summary.patch_scalars);
        _summary.patch_dependencies
            = table_of(signature_table::input, _summary.input_rows, signature_table::patch, _summary.patch_scalars);
    }
    else if (_p.stage == check::stage::tessellation_evaluation)
        _summary.patch_dependencies
            = table_of(signature_table::patch, _summary.patch_rows, signature_table::output, _summary.output_scalars);
}

cc::vector<ll_value> writer::view_id_state()
{
    auto const& s = _summary;
    auto const words_of = [](u32 scalars) { return (scalars + 31) / 32; };
    auto state = cc::vector<ll_value>();
    auto const push_all = [&](cc::span<u32 const> words)
    {
        for (auto const word : words)
            state.push_back(_m.constant_i32(word));
    };
    auto const push_rows = [&](cc::vector<u32> const& table, u32 rows, u32 to_scalars)
    {
        for (auto k = isize(0); k < isize(rows) * words_of(to_scalars); ++k)
            state.push_back(_m.constant_i32(table[k]));
    };
    state.push_back(_m.constant_i32(s.input_scalars));
    state.push_back(_m.constant_i32(s.output_scalars));
    push_all(s.view_outputs);
    push_rows(s.dependencies, s.input_scalars, s.output_scalars);
    if (_p.stage == check::stage::geometry)
        // the three streams SGL does not write
        for (auto stream = 0; stream < 3; ++stream)
            state.push_back(_m.constant_i32(0));
    if (_p.stage == check::stage::tessellation_control || _p.stage == check::stage::mesh)
    {
        state.push_back(_m.constant_i32(s.patch_scalars));
        push_all(s.view_patch);
        push_rows(s.patch_dependencies, s.input_scalars, s.patch_scalars);
    }
    if (_p.stage == check::stage::tessellation_evaluation)
    {
        state.push_back(_m.constant_i32(s.patch_scalars));
        push_rows(s.patch_dependencies, s.patch_scalars, s.output_scalars);
    }
    return state;
}

void writer::stage_properties(cc::vector<u32>& properties)
{
    auto const& shape = _p.primitive;
    auto const stage = _p.stage;
    if (stage == check::stage::mesh || stage == check::stage::task)
    {
        u32 const threads[]
            = {_m.md_i32(u32(_p.workgroup[0])), _m.md_i32(u32(_p.workgroup[1])), _m.md_i32(u32(_p.workgroup[2]))};
        _summary.max_vertices = shape.max_vertices;
        _summary.max_primitives = shape.max_primitives;
        if (stage == check::stage::mesh)
            _summary.mesh_topology = shape.writes_lines ? k_mesh_lines : k_mesh_triangles;
        u32 const mesh_state[] = {_m.md_node(threads), _m.md_i32(shape.max_vertices), _m.md_i32(shape.max_primitives),
                                  _m.md_i32(_summary.mesh_topology), _m.md_i32(_summary.payload_bytes)};
        u32 const task_state[] = {_m.md_node(threads), _m.md_i32(_summary.payload_bytes)};
        properties.push_back(_m.md_i32(stage == check::stage::mesh ? k_tag_mesh_state : k_tag_task_state));
        properties.push_back(stage == check::stage::mesh ? _m.md_node(mesh_state) : _m.md_node(task_state));
        return;
    }
    if (stage == check::stage::geometry)
    {
        // DXIL numbers an input primitive by its vertices, but for the two with adjacency
        _summary.input_primitive = shape.input_vertices == 4 ? 6 : shape.input_vertices == 6 ? 7 : shape.input_vertices;
        // a point list, a line strip or a triangle strip
        _summary.output_topology = shape.output_vertices == 1 ? 1 : shape.output_vertices == 2 ? 3 : 5;
        _summary.max_vertices = shape.max_vertices;
        u32 const state[] = {_m.md_i32(_summary.input_primitive), _m.md_i32(shape.max_vertices), _m.md_i32(1),
                             _m.md_i32(_summary.output_topology), _m.md_i32(1)};
        properties.push_back(_m.md_i32(k_tag_geometry_state));
        properties.push_back(_m.md_node(state));
        return;
    }
    if (stage != check::stage::tessellation_control && stage != check::stage::tessellation_evaluation)
        return;
    _summary.control_points = shape.input_vertices;
    _summary.domain = u32(shape.domain);
    if (stage == check::stage::tessellation_evaluation)
    {
        u32 const state[] = {_m.md_i32(_summary.domain), _m.md_i32(shape.input_vertices)};
        properties.push_back(_m.md_i32(k_tag_domain_state));
        properties.push_back(_m.md_node(state));
        return;
    }
    using partitioning = check::tessellation_partitioning;
    auto const spacing = shape.partitioning == partitioning::integer        ? u32(1)
                       : shape.partitioning == partitioning::fractional_odd ? u32(3)
                                                                            : u32(4);
    // a line, or a triangle in the domain's own orientation
    _summary.output_primitive = shape.domain == tessellation_domain::isoline ? 2 : shape.is_output_clockwise ? 3 : 4;
    auto const max_factor = _m.constant_float(_m.float_type(), cc::bit_cast<u32>(float(shape.max_factor)));
    u32 const state[] = {_m.md_value(_m.type_of(_body), _body),
                         _m.md_i32(shape.input_vertices),
                         _m.md_i32(shape.input_vertices),
                         _m.md_i32(_summary.domain),
                         _m.md_i32(spacing),
                         _m.md_i32(_summary.output_primitive),
                         _m.md_value(_m.float_type(), max_factor)};
    properties.push_back(_m.md_i32(k_tag_hull_state));
    properties.push_back(_m.md_node(state));
}

u32 writer::signature_list(signature_table table)
{
    auto const byte_of = [&](u32 value) { return _m.md_value(_m.byte_type(), _m.constant_i8(value)); };
    auto list = cc::vector<u32>();
    for (auto s = isize(0); s < _summary.signature.size(); ++s)
    {
        auto const& e = _summary.signature[s];
        if (e.table != table || e.continues)
            continue;
        // `DXIL::ComponentType` of `DxilProgramSigCompType`: unsigned, signed, float
        auto const component = e.component_type == 3 ? 9u : e.component_type == 2 ? 4u : 5u;
        auto indices = cc::vector<u32>();
        indices.push_back(_m.md_i32(e.semantic_index));
        for (auto next = s + 1; next < _summary.signature.size() && _summary.signature[next].continues; ++next)
            indices.push_back(_m.md_i32(_summary.signature[next].semantic_index));
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
                              _m.md_i32(u32(indices.size())),
                              byte_of(e.lanes),
                              _m.md_i32(u32(e.row)),
                              byte_of(e.row < 0 ? 0xffu : e.first_column),
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
            auto type = undefined_pointer(cc::format("sgl.resource.{}.{}", cls, b.id));
            if (is_library())
            {
                ll_type const members[] = {_m.int_type()};
                auto const pointer = _m.pointer(_m.structure(cc::format("sgl.resource.{}.{}", cls, b.id), members));
                type = _m.md_value(pointer, _m.constant_bitcast(pointer, _handles[b.global].variable));
            }
            auto fields = cc::vector<u32>();
            fields.push_back(_m.md_i32(b.id));
            fields.push_back(type);
            fields.push_back(
                _m.md_string(is_library() ? _p.name_at(_p.globals[b.global].name_first, _p.globals[b.global].name_size)
                                          : cc::string_view("")));
            fields.push_back(_m.md_i32(b.space));
            fields.push_back(_m.md_i32(b.lower));
            fields.push_back(_m.md_i32(b.range));
            auto extra = u32(0);
            auto tags = cc::vector<u32>();
            if (b.stride > 0)
                tags.push_back_range(cc::vector<u32>{_m.md_i32(k_tag_structured_stride), _m.md_i32(b.stride)});
            else if (b.component > 0)
                tags.push_back_range(cc::vector<u32>{_m.md_i32(k_tag_element_type), _m.md_i32(b.component)});
            if (b.used_by_atomic64)
                tags.push_back_range(cc::vector<u32>{_m.md_i32(k_tag_atomic64_use), _m.md_i32(1)});
            if (!tags.empty())
                extra = _m.md_node(tags);
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
                fields.push_back(_m.md_i32(b.is_comparison ? 1 : 0));
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
    auto const model_minor = _p.sees_micromaps || _uses.reorders ? k_model_minor_micromaps
                           : _uses.start_location                ? k_model_minor_start_location
                                                                 : k_model_minor;

    u32 const version[] = {_m.md_i32(1), _m.md_i32(model_minor)};
    u32 const validator[] = {_m.md_i32(1), _m.md_i32(9)};
    u32 const model[] = {_m.md_string(rule->model), _m.md_i32(k_model_major), _m.md_i32(model_minor)};
    u32 const resources[] = {heads[0], heads[1], heads[2], heads[3]};
    auto const has_resources = !_bindings.empty();
    auto const resources_node = has_resources ? _m.md_node(resources) : 0;

    if (_p.stage == check::stage::vertex || _p.stage == check::stage::tessellation_evaluation)
        for (auto const& e : _summary.signature)
            _uses.layer_output = _uses.layer_output || (e.table == signature_table::output && e.system_value == 4);
    auto metadata_flags = k_native_low_precision;
    for (auto const& b : _bindings)
        if (b.cls == 1 && model_minor >= 7)
            metadata_flags |= k_flag_res_may_not_alias;
    auto feature_flags = u64(0);
    for (auto const& f : k_features)
        if (_uses.*f.used)
        {
            metadata_flags |= f.metadata_flag;
            feature_flags |= f.feature_flag;
        }

    if (is_library())
    {
        constexpr u32 k_tag_shader_kind = 8;
        constexpr u32 k_tag_auto_binding_space = 5;
        constexpr u32 k_tag_ray_payload_size = 6;
        constexpr u32 k_tag_ray_attribute_size = 7;
        constexpr u32 k_library_kind = 6;
        auto properties = cc::vector<u32>();
        properties.push_back(_m.md_i32(k_tag_shader_kind));
        properties.push_back(_m.md_i32(rule->kind));
        if (_p.stage != check::stage::raygen && _p.stage != check::stage::intersection)
        {
            properties.push_back(_m.md_i32(k_tag_ray_payload_size));
            properties.push_back(_m.md_i32(_summary.payload_bytes));
        }
        if (_p.stage == check::stage::closest_hit || _p.stage == check::stage::any_hit)
        {
            properties.push_back(_m.md_i32(k_tag_ray_attribute_size));
            properties.push_back(_m.md_i32(_summary.attribute_bytes));
        }
        u32 const space[] = {_m.md_i32(0)};
        properties.push_back(_m.md_i32(k_tag_auto_binding_space));
        properties.push_back(_m.md_node(space));

        u32 const flags[] = {_m.md_i32(k_tag_shader_flags),
                             _m.md_value(_m.long_type(), _m.constant_int(_m.long_type(), metadata_flags))};
        u32 const first[] = {0, _m.md_string(""), 0, resources_node, metadata_flags != 0 ? _m.md_node(flags) : 0};
        u32 const own[]
            = {_m.md_value(_m.type_of(_entry), _entry), _m.md_string(_p.entry_name), 0, 0, _m.md_node(properties)};
        u32 const entries[] = {_m.md_node(first), _m.md_node(own)};
        u32 const version_node[] = {_m.md_node(version)};
        u32 const validator_node[] = {_m.md_node(validator)};
        u32 const model_node[] = {_m.md_node(model)};
        u32 const resources_list[] = {resources_node};
        name_producer(_m);
        _m.md_named("dx.version", version_node);
        _m.md_named("dx.valver", validator_node);
        _m.md_named("dx.shaderModel", model_node);
        if (has_resources)
            _m.md_named("dx.resources", resources_list);
        // DXC states every payload's access, and on NVIDIA two pipelines of one device that disagree lose payload
        // writes: docs/bugs-external/nvidia-reorder-payload-qualifiers-across-pipelines
        if (!_p.payloads.empty())
        {
            constexpr u32 k_tag_payload_struct = 0;
            constexpr u32 k_tag_field_access = 0;
            auto payloads = cc::vector<u32>();
            payloads.push_back(_m.md_i32(k_tag_payload_struct));
            for (auto const& annotation : _p.payloads)
            {
                auto const type = payload_type(annotation.type);
                auto fields = cc::vector<u32>();
                for (auto const mask : annotation.field_masks)
                {
                    u32 const field[] = {_m.md_i32(k_tag_field_access), _m.md_i32(mask)};
                    fields.push_back(_m.md_node(field));
                }
                payloads.push_back(_m.md_value(type, _m.constant_undef(type)));
                payloads.push_back(_m.md_node(fields));
            }
            u32 const annotations[] = {_m.md_node(payloads)};
            _m.md_named("dx.dxrPayloadAnnotations", annotations);
        }
        _m.md_named("dx.entryPoints", entries);
        _summary.is_library = true;
        _summary.shader_kind = rule->kind;
        if (_p.stage == check::stage::intersection)
            _summary.attribute_bytes = 0;
        _summary.min_shader_minor = (_p.sees_micromaps && _uses.ray_query) || _uses.reorders ? 9
                                  : _uses.ray_query                                          ? 5
                                                                                             : 3;
        _summary.metadata_flags = metadata_flags;
        _summary.feature_flags = feature_flags;
        _summary.program_kind = k_library_kind;
        _summary.program_version = (k_model_major << 4) | model_minor;
        _summary.dxil_version = 0x100 | model_minor;
        _summary.entry_name = _p.entry_name;
        for (auto const cls : {2u, 3u, 0u, 1u})
            for (auto const& b : _bindings)
                if (b.cls == cls)
                    _summary.resources.push_back(
                        {.psv_type = b.psv_type,
                         .space = b.space,
                         .lower = b.lower,
                         .upper = b.upper,
                         .kind = b.kind,
                         .psv_flags = b.used_by_atomic64 ? k_psv_used_by_atomic64 : 0,
                         .cls = cls,
                         .id = b.id,
                         .name = _p.name_at(_p.globals[b.global].name_first, _p.globals[b.global].name_size)});
        return;
    }

    u32 const threads[]
        = {_m.md_i32(u32(_p.workgroup[0])), _m.md_i32(u32(_p.workgroup[1])), _m.md_i32(u32(_p.workgroup[2]))};
    auto properties = cc::vector<u32>();
    properties.push_back(_m.md_i32(k_tag_shader_flags));
    properties.push_back(_m.md_value(_m.long_type(), _m.constant_int(_m.long_type(), metadata_flags)));
    if (is_compute)
    {
        properties.push_back(_m.md_i32(k_tag_num_threads));
        properties.push_back(_m.md_node(threads));
    }
    stage_properties(properties);
    auto signatures = u32(0);
    if (!is_compute && _p.stage != check::stage::task)
    {
        fill_dependencies();
        u32 const lists_of_signature[] = {signature_list(signature_table::input), signature_list(signature_table::output),
                                          signature_list(signature_table::patch)};
        signatures = _m.md_node(lists_of_signature);
        if (!_failure.empty())
            return;

        auto const state = view_id_state();
        auto const array = _m.array(_m.int_type(), u64(state.size()));
        u32 const state_node[] = {_m.md_node(
            cc::span<u32 const>(std::initializer_list<u32>{_m.md_value(array, _m.constant_struct(array, state))}))};
        _m.md_named("dx.viewIdState", state_node);
    }
    u32 const entry[] = {_m.md_value(_m.type_of(_entry), _entry), _m.md_string(_p.entry_name), signatures,
                         resources_node, _m.md_node(properties)};

    u32 const version_node[] = {_m.md_node(version)};
    u32 const validator_node[] = {_m.md_node(validator)};
    u32 const model_node[] = {_m.md_node(model)};
    u32 const resources_list[] = {resources_node};
    u32 const entries[] = {_m.md_node(entry)};
    name_producer(_m);
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
    if (_p.stage != check::stage::pixel)
        for (auto const& e : _summary.signature)
            _summary.writes_position
                = _summary.writes_position || (e.table == signature_table::output && e.system_value == 1);
    if (_p.stage == check::stage::pixel)
        for (auto const& e : _summary.signature)
        {
            auto const is_input = e.table == signature_table::input;
            auto const is_sampled = e.mode == interpolation::sample || e.mode == interpolation::noperspective_sample;
            _summary.writes_depth
                = _summary.writes_depth || (!is_input && (e.system_value == 65 || e.system_value >= 67));
            _summary.runs_per_sample = _summary.runs_per_sample || (is_input && (is_sampled || e.system_value == 10));
        }
    // the PSV lists a module's bindings in this order, whichever order they were found in
    for (auto const cls : {2u, 3u, 0u, 1u})
        for (auto const& b : _bindings)
            if (b.cls == cls)
                _summary.resources.push_back({.psv_type = b.psv_type,
                                              .space = b.space,
                                              .lower = b.lower,
                                              .upper = b.upper,
                                              .kind = b.kind,
                                              .psv_flags = b.used_by_atomic64 ? k_psv_used_by_atomic64 : 0});
    if (is_compute || _p.stage == check::stage::mesh || _p.stage == check::stage::task)
        for (auto axis = 0; axis < 3; ++axis)
            _summary.workgroup[axis] = _p.workgroup[axis];
}

// ---- DXIL's vocabulary

ll_type writer::hit_object_type()
{
    ll_type const members[] = {_m.pointer(_m.byte_type())};
    return _m.structure("dx.types.HitObject", members);
}

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
    if (m.is_structure(overload))
        return m.name_of(overload);
    auto const bits = m.bits_of(overload);
    if (m.is_float(overload))
        return bits == 16 ? "f16" : bits == 64 ? "f64" : "f32";
    return bits == 1 ? "i1" : bits == 16 ? "i16" : bits == 64 ? "i64" : "i32";
}

u8 writer::overload_mask(module_builder const& m, ll_type overload)
{
    if (m.is_structure(overload))
        return k_udt;
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
    case 'p':
        return _m.pointer(overload);
    case 'q':
        return _m.pointer(overload, 3);
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
    case 'H':
        return hit_object_type();
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
    case 'S':
    {
        ll_type const members[] = {_m.int_type(), _m.int_type()};
        return _m.structure("dx.types.splitdouble", members);
    }
    case 'F':
    {
        ll_type const members[] = {_m.int_type(), _m.int_type(), _m.int_type(), _m.int_type()};
        return _m.structure("dx.types.fouri32", members);
    }
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
    case dx_class::no_return:
    {
        ll_attribute const list[] = {{.kind = k_attr_noreturn}, {.kind = k_attr_nounwind}};
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
