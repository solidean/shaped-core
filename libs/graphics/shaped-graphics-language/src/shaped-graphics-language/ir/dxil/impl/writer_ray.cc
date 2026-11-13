#include <clean-core/string/format.hh>
#include <shaped-graphics-language/ir/dxil/impl/dx_ops.hh>
#include <shaped-graphics-language/ir/dxil/impl/writer.hh>

using namespace sgl;
using namespace sgl::ir;
using namespace sgl::ir::dxil;
using namespace sgl::ir::dxil::impl;

// What a ray-tracing stage adds: a library's one function, its pointers, and what it reads of the ray.

namespace
{
/// How a `built_in` of a ray-tracing stage reads.
struct ray_read_rule
{
    ir::built_in input = ir::built_in::none;
    dx_op op = dx_op::instance_id;
};
constexpr ray_read_rule k_ray_reads[] = {
    {ir::built_in::launch_id, dx_op::dispatch_rays_index},
    {ir::built_in::launch_size, dx_op::dispatch_rays_dimensions},
    {ir::built_in::world_ray_origin, dx_op::world_ray_origin},
    {ir::built_in::world_ray_direction, dx_op::world_ray_direction},
    {ir::built_in::object_ray_origin, dx_op::object_ray_origin},
    {ir::built_in::object_ray_direction, dx_op::object_ray_direction},
    {ir::built_in::ray_t_min, dx_op::ray_t_min},
    {ir::built_in::ray_t_current, dx_op::ray_t_current},
    {ir::built_in::ray_instance_id, dx_op::instance_id},
    {ir::built_in::ray_instance_index, dx_op::instance_index},
    {ir::built_in::ray_geometry_index, dx_op::geometry_index},
    {ir::built_in::ray_primitive_index, dx_op::primitive_index},
    {ir::built_in::hit_kind, dx_op::hit_kind},
    {ir::built_in::object_to_world, dx_op::object_to_world},
    {ir::built_in::world_to_object, dx_op::world_to_object},
};

[[nodiscard]] constexpr bool is_ray_space(storage s)
{
    return s == storage::ray_input || s == storage::hit_attribute || s == storage::ray_output
        || s == storage::callable_output;
}
} // namespace

bool writer::is_library() const
{
    return _p.stage >= check::stage::raygen;
}

ll_type writer::entry_function_type()
{
    auto const is_hit = _p.stage == check::stage::closest_hit || _p.stage == check::stage::any_hit;
    _payload_roots.resize_to_filled(_p.globals.size(), {});
    auto parameters = cc::vector<ll_type>();
    auto input = -1;
    auto attributes = -1;
    for (auto g = isize(0); g < _p.globals.size(); ++g)
    {
        auto const space = _p.at(_p.globals[g].type).space;
        if (space == storage::ray_input)
            input = i32(g);
        if (space == storage::hit_attribute)
            attributes = i32(g);
    }
    if (input >= 0)
        parameters.push_back(_m.pointer(root_type(u32(input))));
    if (is_hit)
    {
        if (input < 0)
            unsupported("a hit stage that takes no payload, which only metal builds");
        if (attributes >= 0)
            parameters.push_back(_m.pointer(root_type(u32(attributes))));
        else
            parameters.push_back(_m.pointer(triangle_attributes_type()));
    }
    _m.set_arguments(parameters);
    return _m.function(_m.void_type(), parameters);
}

ll_type writer::triangle_attributes_type()
{
    ll_type const members[] = {_m.array(_m.float_type(), 2)};
    return _m.structure("struct.BuiltInTriangleIntersectionAttributes", members);
}

ll_type writer::root_type(u32 global)
{
    auto const pointee = _p.at(_p.globals[global].type).element;
    if (_p.at(pointee).kind == type_kind::structure)
        return payload_type(pointee);
    // a triangle's barycentrics are the one attribute that is no struct of the program's
    _payload_roots[global].is_wrapped = true;
    return triangle_attributes_type();
}

void writer::declare_ray_roots()
{
    auto const has_attributes_parameter = _p.stage == check::stage::closest_hit || _p.stage == check::stage::any_hit;
    for (auto g = isize(0); g < _p.globals.size(); ++g)
    {
        auto const& pointer = _p.at(_p.globals[g].type);
        if (!is_ray_space(pointer.space))
            continue;
        auto& root = _payload_roots[g];
        auto alignment = u32(1);
        auto const bytes = payload_layout(pointer.element, alignment);
        root.type = root_type(u32(g));
        switch (pointer.space)
        {
        case storage::ray_input:
            root.pointer = _m.argument(0);
            _summary.payload_bytes = bytes;
            break;
        case storage::hit_attribute:
            root.pointer = has_attributes_parameter ? _m.argument(1) : _m.alloca_of(root.type, alignment);
            _summary.attribute_bytes = bytes;
            break;
        default:
            root.pointer = _m.alloca_of(root.type, alignment);
            break;
        }
    }
    if (has_attributes_parameter && _summary.attribute_bytes == 0)
        _summary.attribute_bytes = 8;
}

void writer::ray_element(u32 index, stage_element const& e, instruction const& i)
{
    ray_read_rule const* found = nullptr;
    for (auto const& r : k_ray_reads)
        if (r.input == e.system)
            found = &r;
    if (found == nullptr)
        return unsupported(cc::format("the stage input {} on DXIL", int(e.system)));
    auto const number = _m.constant_i32(k_dx_ops[isize(found->op)].number);
    auto const& info = _p.at(i.type);
    // brought by raytracing tier 1.1, as a query was
    _uses.ray_query = _uses.ray_query || e.system == ir::built_in::ray_geometry_index;
    _scratch.clear();
    if (info.kind == type_kind::matrix)
    {
        for (auto column = u32(0); column < info.count; ++column)
            for (auto row = u32(0); row < 3; ++row)
            {
                ll_value const args[] = {number, _m.constant_i32(row), _m.constant_i8(column)};
                _scratch.push_back(call(found->op, _m.float_type(), args));
            }
        return define(index);
    }
    if (info.kind == type_kind::vector)
    {
        auto const overload = scalar_type(info.element);
        for (auto lane = u32(0); lane < info.count; ++lane)
        {
            ll_value const args[] = {number, _m.constant_i8(lane)};
            _scratch.push_back(call(found->op, overload, args));
        }
        return define(index);
    }
    ll_value const args[] = {number};
    define_one(index, call(found->op, scalar_type(i.type), args));
}

// ---- EMIT-263: the nvapi fill, NVIDIA's placeholder protocol as nvHLSLExtns.h writes it ----
//
// What follows encodes the protocol of NVAPI's HLSL shader extensions, R615, as `nvHLSLExtns.h` and
// `nvHLSLExtnsInternal.h` write it, so that SGL's DXIL says to NVIDIA's driver what DXC's compile of that header says.
// The opcodes and the record's layout below are those headers', which carry this notice, a sentence a line:
//
//   SPDX-FileCopyrightText: Copyright (c) 2019-2026 NVIDIA CORPORATION & AFFILIATES.
//   All rights reserved.
//   SPDX-License-Identifier: MIT
//
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
//   documentation files (the "Software"), to deal in the Software without restriction, including without limitation
//   the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
//   permit persons to whom the Software is furnished to do so, subject to the following conditions:
//
//   The above copyright notice and this permission notice shall be included in all copies or substantial portions of
//   the Software.
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
//   THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
//   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
//   AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
//   OTHER DEALINGS IN THE SOFTWARE.
namespace
{
/// `nvShaderExtnEnums.h`'s opcodes of the hit object.
constexpr u32 k_nv_trace_ray = 67;
constexpr u32 k_nv_reorder_thread = 71;
constexpr u32 k_nv_invoke = 72;
constexpr u32 k_nv_is_miss = 73;
constexpr u32 k_nv_get_instance_id = 74;
constexpr u32 k_nv_get_instance_index = 75;
constexpr u32 k_nv_get_primitive_index = 76;
constexpr u32 k_nv_get_geometry_index = 77;
constexpr u32 k_nv_get_ray_desc = 79;
constexpr u32 k_nv_is_hit = 83;
/// `NvShaderExtnStruct`'s fields by their byte offset: `opcode`, `src0u` and `numOutputsForIncCounter`.
constexpr u32 k_nv_opcode = 0;
constexpr u32 k_nv_src0 = 76;
constexpr u32 k_nv_outputs = 144;
/// What `GetRayDesc` reads back after its record, of which `TMax` is the second: TMin, TMax, origin, direction.
constexpr u32 k_nv_ray_desc_words = 8;
} // namespace

ll_value writer::nvapi_increment()
{
    ll_value const args[]
        = {_m.constant_i32(k_dx_ops[isize(dx_op::buffer_update_counter)].number), _nvapi, _m.constant_i8(1)};
    return call(dx_op::buffer_update_counter, _m.int_type(), args);
}

void writer::nvapi_store(ll_value record, u32 offset, ll_value value)
{
    auto const undefined = _m.constant_undef(_m.int_type());
    ll_value const args[] = {_m.constant_i32(k_dx_ops[isize(dx_op::raw_buffer_store)].number),
                             _nvapi,
                             record,
                             _m.constant_i32(offset),
                             value,
                             undefined,
                             undefined,
                             undefined,
                             _m.constant_i8(1),
                             _m.constant_i32(4)};
    (void)call(dx_op::raw_buffer_store, _m.int_type(), args);
}

void writer::nvapi_hit_object_op(u32 index, instruction const& i)
{
    auto const operands = _p.operands_of(i);
    auto const at = [&](isize k) { return leaf_at(value_ref(operands[k])); };
    auto const word = [&](u32 v) { return _m.constant_i32(v); };
    auto const number = [&](dx_op code) { return _m.constant_i32(k_dx_ops[isize(code)].number); };
    /// A record of `opcode` about the hit object `hit`, as each of NvHitObject's members starts.
    auto const asked = [&](u32 opcode, ll_value hit)
    {
        auto const record = nvapi_increment();
        nvapi_store(record, k_nv_opcode, word(opcode));
        nvapi_store(record, k_nv_src0, hit);
    };
    /// `NvReorderThread`: by the hit object where there is one, then by the hint and its bits.
    auto const reorder = [&](ll_value hit, ll_value hint, ll_value bits)
    {
        auto const record = nvapi_increment();
        nvapi_store(record, k_nv_opcode, word(k_nv_reorder_thread));
        nvapi_store(record, k_nv_src0, word(hit != ll_value::none ? 1 : 0));
        nvapi_store(record, k_nv_src0 + 4, hit != ll_value::none ? hit : word(0));
        nvapi_store(record, k_nv_src0 + 8, hint);
        nvapi_store(record, k_nv_src0 + 12, bits);
        (void)nvapi_increment();
    };
    /// DXIL's `TraceRay`, which the driver takes for the hit object's call by the miss index the protocol handed it.
    auto const trace = [&](ll_value structure, ll_value const(&ray)[12], ll_value handle, payload_root const& root)
    {
        ll_value const args[] = {number(dx_op::trace_ray),
                                 structure,
                                 ray[0],
                                 ray[1],
                                 ray[2],
                                 ray[3],
                                 handle,
                                 ray[4],
                                 ray[5],
                                 ray[6],
                                 ray[7],
                                 ray[8],
                                 ray[9],
                                 ray[10],
                                 ray[11],
                                 root.pointer};
        (void)call(dx_op::trace_ray, root.type, args);
    };
    /// `NvTraceRayHitObject`: its record names the miss shader, and hands back the hit object and the trace's handle.
    auto const traced = [&](payload_root const& root)
    {
        auto const origin = value_ref(operands[6]);
        auto const direction = value_ref(operands[8]);
        auto const record = nvapi_increment();
        nvapi_store(record, k_nv_opcode, word(k_nv_trace_ray));
        nvapi_store(record, k_nv_outputs, word(2));
        nvapi_store(record, k_nv_src0, at(5));
        auto const hit = nvapi_increment();
        auto const handle = nvapi_increment();
        ll_value const ray[] = {at(1),
                                at(2),
                                at(3),
                                at(4),
                                leaf_at(origin, 0),
                                leaf_at(origin, 1),
                                leaf_at(origin, 2),
                                at(7),
                                leaf_at(direction, 0),
                                leaf_at(direction, 1),
                                leaf_at(direction, 2),
                                at(9)};
        trace(at(0), ray, handle, root);
        return hit;
    };
    switch (i.code)
    {
    case op::hit_object_trace:
        return define_one(index, traced(_payload_roots[index_of(value_ref(operands[10]))]));
    case op::trace_reorder_execute:
    {
        auto const& root = _payload_roots[index_of(value_ref(operands[10]))];
        auto const hit = traced(root);
        if (operands.size() > 11)
            reorder(hit, at(11), at(12));
        else
            reorder(hit, word(0), word(0));
        // `NvInvokeHitObject`: a trace of no ray through the same structure, by the handle its record hands back
        auto const record = nvapi_increment();
        nvapi_store(record, k_nv_opcode, word(k_nv_invoke));
        nvapi_store(record, k_nv_src0, hit);
        auto const handle = nvapi_increment();
        auto const zero = word(0);
        auto const none = _m.constant_float(_m.float_type(), 0);
        ll_value const ray[] = {zero, zero, zero, zero, none, none, none, none, none, none, none, none};
        trace(at(0), ray, handle, root);
        return define_one(index, hit);
    }
    case op::hit_object_reorder:
        if (operands.size() > 1)
            return reorder(at(0), at(1), at(2));
        return reorder(at(0), word(0), word(0));
    case op::reorder_thread:
        return reorder(ll_value::none, at(0), at(1));
    case op::hit_object_invoke:
        // the emit refuses it under this fill: NVAPI invokes through the structure its trace took
        return unsupported("a hit object invoked apart from its trace, through NVAPI");
    default:
        break;
    }
    auto const hit = at(0);
    switch (hit_object_value(i.sub))
    {
    case hit_object_value::is_hit:
    case hit_object_value::is_miss:
    {
        asked(hit_object_value(i.sub) == hit_object_value::is_hit ? k_nv_is_hit : k_nv_is_miss, hit);
        return define_one(index, _m.compare(predicate::i_ne, nvapi_increment(), word(0)));
    }
    case hit_object_value::t:
    {
        // `GetRayDesc().TMax`, the hit's distance as vulkan's instruction reads it; every word is read back regardless
        asked(k_nv_get_ray_desc, hit);
        auto t_max = ll_value::none;
        for (auto w = u32(0); w < k_nv_ray_desc_words; ++w)
        {
            auto const read = nvapi_increment();
            if (w == 1)
                t_max = read;
        }
        return define_one(index, _m.cast(cast_op::bitcast, t_max, _m.float_type()));
    }
    case hit_object_value::instance_id:
        asked(k_nv_get_instance_id, hit);
        break;
    case hit_object_value::instance_index:
        asked(k_nv_get_instance_index, hit);
        break;
    case hit_object_value::geometry_index:
        asked(k_nv_get_geometry_index, hit);
        break;
    case hit_object_value::primitive_index:
        asked(k_nv_get_primitive_index, hit);
        break;
    }
    return define_one(index, nvapi_increment());
}

void writer::hit_object_op(u32 index, instruction const& i)
{
    if (_p.reorders_through_nvapi)
        return nvapi_hit_object_op(index, i);
    /// Parallel to `hit_object_value`.
    constexpr dx_op k_reads[] = {
        dx_op::hit_object_is_hit,          dx_op::hit_object_is_miss,        dx_op::hit_object_t,
        dx_op::hit_object_instance_id,     dx_op::hit_object_instance_index, dx_op::hit_object_geometry_index,
        dx_op::hit_object_primitive_index,
    };
    _uses.reorders = true;
    auto const operands = _p.operands_of(i);
    auto const at = [&](isize k) { return leaf_at(value_ref(operands[k])); };
    auto const number = [&](dx_op code) { return _m.constant_i32(k_dx_ops[isize(code)].number); };
    auto const reorder = [&](ll_value hit, ll_value hint, ll_value bits)
    {
        ll_value const args[] = {number(dx_op::maybe_reorder_thread), hit, hint, bits};
        (void)call(dx_op::maybe_reorder_thread, _m.void_type(), args);
    };
    auto const invoke = [&](ll_value hit, payload_root const& root)
    {
        ll_value const args[] = {number(dx_op::hit_object_invoke), hit, root.pointer};
        (void)call(dx_op::hit_object_invoke, root.type, args);
    };
    auto const traced = [&](payload_root const& root)
    {
        auto const origin = value_ref(operands[6]);
        auto const direction = value_ref(operands[8]);
        ll_value const args[] = {number(dx_op::hit_object_trace_ray),
                                 at(0),
                                 at(1),
                                 at(2),
                                 at(3),
                                 at(4),
                                 at(5),
                                 leaf_at(origin, 0),
                                 leaf_at(origin, 1),
                                 leaf_at(origin, 2),
                                 at(7),
                                 leaf_at(direction, 0),
                                 leaf_at(direction, 1),
                                 leaf_at(direction, 2),
                                 at(9),
                                 root.pointer};
        return call(dx_op::hit_object_trace_ray, root.type, args);
    };
    switch (i.code)
    {
    case op::hit_object_trace:
        return define_one(index, traced(_payload_roots[index_of(value_ref(operands[10]))]));
    case op::trace_reorder_execute:
    {
        auto const& root = _payload_roots[index_of(value_ref(operands[10]))];
        auto const hit = traced(root);
        if (operands.size() > 11)
            reorder(hit, at(11), at(12));
        else
            reorder(hit, _m.constant_undef(_m.int_type()), _m.constant_i32(0));
        invoke(hit, root);
        return define_one(index, hit);
    }
    case op::hit_object_reorder:
        if (operands.size() > 1)
            return reorder(at(0), at(1), at(2));
        return reorder(at(0), _m.constant_undef(_m.int_type()), _m.constant_i32(0));
    case op::reorder_thread:
    {
        ll_value const args[] = {number(dx_op::hit_object_make_nop)};
        return reorder(call(dx_op::hit_object_make_nop, _m.void_type(), args), at(0), at(1));
    }
    case op::hit_object_invoke:
        return invoke(at(0), _payload_roots[index_of(value_ref(operands[1]))]);
    default:
    {
        auto const code = k_reads[i.sub];
        ll_value const args[] = {number(code), at(0)};
        return define_one(index, call(code, scalar_type(i.type), args));
    }
    }
}

void writer::ray_stage_op(u32 index, instruction const& i)
{
    if (i.code != op::trace_ray && i.code != op::execute_callable && i.code != op::report_hit)
        return hit_object_op(index, i);
    auto const operands = _p.operands_of(i);
    auto const at = [&](isize k) { return leaf_at(value_ref(operands[k])); };
    auto const number = [&](dx_op code) { return _m.constant_i32(k_dx_ops[isize(code)].number); };
    auto const& root = _payload_roots[index_of(value_ref(operands[operands.size() - 1]))];
    switch (i.code)
    {
    case op::trace_ray:
    {
        auto const origin = value_ref(operands[6]);
        auto const direction = value_ref(operands[8]);
        ll_value const args[] = {number(dx_op::trace_ray),
                                 at(0),
                                 at(1),
                                 at(2),
                                 at(3),
                                 at(4),
                                 at(5),
                                 leaf_at(origin, 0),
                                 leaf_at(origin, 1),
                                 leaf_at(origin, 2),
                                 at(7),
                                 leaf_at(direction, 0),
                                 leaf_at(direction, 1),
                                 leaf_at(direction, 2),
                                 at(9),
                                 root.pointer};
        (void)call(dx_op::trace_ray, root.type, args);
        return;
    }
    case op::execute_callable:
    {
        ll_value const args[] = {number(dx_op::call_shader), at(0), root.pointer};
        (void)call(dx_op::call_shader, root.type, args);
        return;
    }
    default:
    {
        ll_value const args[] = {number(dx_op::report_hit), at(0), at(1), root.pointer};
        return define_one(index, call(dx_op::report_hit, root.type, args));
    }
    }
}

void writer::end_of_hit(dx_op code)
{
    ll_value const args[] = {_m.constant_i32(k_dx_ops[isize(code)].number)};
    (void)call(code, _m.void_type(), args);
    _m.unreachable();
}
