#include "impl/graph.hh"
#include "metrics.hh"

#include <babel-data/shader/dxil_container.hh>
#include <babel-data/shader/llvm37.hh>
#include <babel-data/shader/llvm_bitstream.hh>
#include <babel-data/shader/spirv.hh>
#include <clean-core/record/scope.hh>
#include <clean-core/string/format.hh>

// DXIL's functions as graphs: LLVM's instructions by kind, a `dx.op` call by the operation class its name states.

namespace shan
{
namespace
{
using babel::llvm37::inst_code;
using babel::llvm37::type_kind;

[[nodiscard]] u32 bits_of(babel::llvm37::module const& m, u32 t, int depth = 0)
{
    if (t >= u32(m.types.size()) || depth > 16)
        return 0;
    auto const& x = m.types[t];
    switch (x.kind)
    {
    case type_kind::integer:
        return x.width < 32 ? 32 : x.width;
    case type_kind::half:
        return 16;
    case type_kind::float_:
        return 32;
    case type_kind::double_:
        return 64;
    case type_kind::vector:
    case type_kind::array:
        return u32(x.count) * bits_of(m, x.element, depth + 1);
    case type_kind::structure:
    {
        auto sum = u32(0);
        for (auto k = u32(0); k < x.member_count; ++k)
            sum += bits_of(m, m.members[x.first_member + k], depth + 1);
        return sum;
    }
    default:
        // a pointer, a handle's opaque, void: no register of its own
        return 0;
    }
}

[[nodiscard]] bool is_float(babel::llvm37::module const& m, u32 t)
{
    if (t >= u32(m.types.size()))
        return false;
    auto const& x = m.types[t];
    if (x.kind == type_kind::vector)
        return is_float(m, x.element);
    return x.kind == type_kind::half || x.kind == type_kind::float_ || x.kind == type_kind::double_;
}

/// What a `dx.op.<class>.<overload>` call is, by its class.
[[nodiscard]] op_kind dx_op_kind(cc::string_view name)
{
    auto rest = name.subview(6); // after "dx.op."
    if (auto const dot = rest.find('.'); dot >= 0)
        rest = rest.subview({.offset = 0, .size = dot});
    auto const is = [&](cc::string_view prefix) { return rest.starts_with(prefix); };
    if (is("rawBufferLoad") || is("bufferLoad") || is("cbufferLoad") || is("textureLoad") || is("rawBufferVectorLoad"))
        return op_kind::load;
    if (is("rawBufferStore") || is("bufferStore") || is("textureStore") || is("storeOutput") || is("storeVertexOutput")
        || is("storePrimitiveOutput") || is("storePatchConstant") || is("rawBufferVectorStore"))
        return op_kind::store;
    if (is("sample") || is("textureGather") || is("calculateLOD") || is("getDimensions") || is("checkAccess"))
        return op_kind::image;
    if (is("atomic"))
        return op_kind::atomic;
    if (is("barrier"))
        return op_kind::barrier;
    if (is("wave") || is("quad"))
        return op_kind::subgroup;
    if (is("deriv"))
        return op_kind::derivative;
    if (is("traceRay") || is("rayQuery") || is("reportHit") || is("callShader") || is("hitObject")
        || is("acceptHitAndEndSearch") || is("ignoreHit") || is("allocateRayQuery"))
        return op_kind::ray;
    if (is("createHandle") || is("annotateHandle"))
        return op_kind::address;
    if (is("unary") || is("binary") || is("tertiary") || is("dot") || is("isSpecialFloat") || is("fma") || is("legacy")
        || is("bitcast") || is("makeDouble") || is("splitDouble") || is("quaternary"))
        return op_kind::extended;
    return op_kind::other;
}

/// Whether a `dx.op` call's result differs across a wave by what it is rather than by what it reads.
[[nodiscard]] impl::uniformity_source dx_op_source(cc::string_view name)
{
    auto rest = name.subview(6);
    if (auto const dot = rest.find('.'); dot >= 0)
        rest = rest.subview({.offset = 0, .size = dot});
    auto const is = [&](cc::string_view prefix) { return rest.starts_with(prefix); };
    // a wave's own answer is the same in every lane: an all, an any, a ballot, a reduction, a read of one lane
    if (is("groupId") || is("waveReadLane") || is("waveAll") || is("waveAny") || is("waveActive")
        || is("waveGetLaneCount"))
        return impl::uniformity_source::uniform;
    if (is("threadId") || is("flattenedThreadIdInGroup") || is("loadInput") || is("attributeAtVertex") || is("eval")
        || is("sampleIndex") || is("coverage") || is("innerCoverage") || is("primitiveID") || is("waveGetLaneIndex")
        || is("waveIsFirstLane") || is("wavePrefix") || is("waveMatch") || is("waveMultiPrefix") || is("quad")
        || is("atomic") || is("dispatchRaysIndex") || is("rayQuery") || is("hitObject") || is("domainLocation")
        || is("outputControlPointID") || is("gsInstanceID") || is("isHelperLane"))
        return impl::uniformity_source::varying;
    return impl::uniformity_source::operands;
}

[[nodiscard]] op_kind kind_of(babel::llvm37::module const& m,
                              babel::llvm37::body const& b,
                              babel::llvm37::instruction const& i)
{
    switch (i.code)
    {
    case inst_code::binop:
        return is_float(m, i.type) ? op_kind::float_arithmetic : op_kind::int_arithmetic;
    case inst_code::cast:
        return op_kind::conversion;
    case inst_code::select:
    case inst_code::vselect:
        return op_kind::logic;
    case inst_code::cmp:
    case inst_code::cmp2:
        return op_kind::compare;
    case inst_code::extract_element:
    case inst_code::insert_element:
    case inst_code::shuffle_vector:
    case inst_code::extract_value:
    case inst_code::insert_value:
        return op_kind::composite;
    case inst_code::gep:
    case inst_code::gep_old:
        return op_kind::address;
    case inst_code::alloca_:
        return op_kind::variable;
    case inst_code::load:
        return op_kind::load;
    case inst_code::store:
        return op_kind::store;
    case inst_code::phi:
        return op_kind::phi;
    case inst_code::ret:
    case inst_code::br:
    case inst_code::switch_:
    case inst_code::unreachable:
        return op_kind::control;
    case inst_code::atomic_rmw:
    case inst_code::cmpxchg:
        return op_kind::atomic;
    case inst_code::fence:
        return op_kind::barrier;
    case inst_code::call:
    {
        auto const name = m.callee_name(b, i);
        if (name.starts_with("dx.op."))
            return dx_op_kind(name);
        if (name.starts_with("llvm."))
            return op_kind::declaration;
        return op_kind::control;
    }
    default:
        return op_kind::other;
    }
}

[[nodiscard]] function_metrics measure_body(babel::llvm37::module const& m, babel::llvm37::body const& b)
{
    auto f = function_metrics();
    auto const& fn = m.functions[b.function];
    f.name = fn.name.empty() ? cc::format("@{}", fn.value_id) : fn.name;
    f.id = fn.value_id;

    // the body's values from its first argument on are the graph's, densely; the module's are constants and globals
    auto g = impl::graph();
    for (auto const& v : b.values)
        g.widths.push_back(v.kind == babel::llvm37::value_kind::constant ? 0 : i32((bits_of(m, v.type) + 31) / 32));
    auto const local = [&](u32 id) -> i32
    {
        if (id < b.first_value || id - b.first_value >= u32(b.values.size()))
            return -1;
        auto const at = i32(id - b.first_value);
        return b.values[at].kind == babel::llvm37::value_kind::constant ? -1 : at;
    };
    for (auto k = u32(0); k < b.block_count; ++k)
        g.blocks.push_back({});
    auto current = u32(0xffffffffu);
    for (auto idx = isize(0); idx < b.instructions.size(); ++idx)
    {
        auto const& i = b.instructions[idx];
        if (i.block >= u32(g.blocks.size()))
            continue;
        if (i.block != current)
        {
            current = i.block;
            g.blocks[i.block].first = u32(g.instructions.size());
        }
        auto const kind = kind_of(m, b, i);
        auto gi = impl::graph_instruction{
            .kind = kind,
            .result = i.result == 0xffffffffu ? -1 : local(i.result),
            .first = u32(g.operands.size()),
            .phi_first = u32(g.phi_values.size()),
            .counts = kind != op_kind::declaration,
        };
        if (i.code == inst_code::call)
            if (auto const name = m.callee_name(b, i); name.starts_with("dx.op."))
                gi.source = dx_op_source(name);
        // what a load from a function's own variable holds, or an atomic returns, is not followed
        if (i.code == inst_code::atomic_rmw || i.code == inst_code::cmpxchg)
            gi.source = impl::uniformity_source::varying;
        auto const operands = m.operands_of(b, i);
        auto const roles = cc::span<babel::llvm37::operand_role const>(b.roles).subspan(
            {.offset = isize(i.first), .size = isize(i.count)});
        if (i.code == inst_code::phi)
        {
            for (auto k = isize(0); k + 1 < operands.size(); k += 2)
            {
                g.phi_values.push_back(local(operands[k]));
                g.phi_blocks.push_back(i32(operands[k + 1]));
            }
            gi.phi_count = u32(g.phi_values.size()) - gi.phi_first;
        }
        else
        {
            for (auto k = isize(0); k < operands.size(); ++k)
            {
                if (roles[k] == babel::llvm37::operand_role::value)
                {
                    if (auto const v = local(operands[k]); v >= 0)
                    {
                        g.operands.push_back(v);
                        if ((i.code == inst_code::br || i.code == inst_code::switch_) && g.blocks[i.block].condition < 0)
                            g.blocks[i.block].condition = v;
                        if (i.code == inst_code::load && v < i32(b.values.size())
                            && b.values[v].kind == babel::llvm37::value_kind::instruction
                            && b.instructions[b.values[v].index].code == inst_code::alloca_)
                            gi.source = impl::uniformity_source::varying;
                    }
                }
                else if (roles[k] == babel::llvm37::operand_role::block)
                    g.blocks[i.block].successors.push_back(i32(operands[k]));
            }
        }
        gi.count = u32(g.operands.size()) - gi.first;
        if (i.code == inst_code::alloca_ && i.type < u32(m.types.size()))
            f.variable_words += i32((bits_of(m, m.types[i.type].element) + 31) / 32);
        if (i.code == inst_code::call && kind == op_kind::control)
            ++f.calls;
        // a conditional branch or a switch is a selection
        if ((i.code == inst_code::br && i.count == 3) || i.code == inst_code::switch_)
            ++f.selections;
        g.instructions.push_back(gi);
        g.blocks[i.block].end = u32(g.instructions.size());
    }
    impl::measure(g, f);
    f.loops = impl::back_edges(g);
    return f;
}
} // namespace

cc::result<module_metrics> analyze(babel::llvm37::module const& m)
{
    CC_RECORD_SCOPE("shan.analyze");
    auto result = module_metrics();
    result.total.name = "total";
    for (auto const& b : m.bodies)
    {
        auto f = measure_body(m, b);
        result.entry_points.push_back(f.name);
        impl::accumulate(f, result.total);
        result.functions.push_back(cc::move(f));
    }
    return result;
}

cc::result<module_metrics> analyze_binary(cc::span<byte const> bytes)
{
    if (bytes.size() >= 4 && bytes[0] == byte('D') && bytes[1] == byte('X') && bytes[2] == byte('B')
        && bytes[3] == byte('C'))
    {
        auto c = babel::dxil::read(bytes);
        if (c.has_error())
            return cc::error(c.error().to_string());
        auto const* const part = c.value().part_named("DXIL");
        if (part == nullptr)
            return cc::error("shan: the container holds no DXIL part");
        auto p = babel::dxil::program_of(*part);
        if (p.has_error())
            return cc::error(p.error().to_string());
        auto bc = babel::llvm::read_bitstream(p.value().bitcode);
        if (bc.has_error())
            return cc::error(bc.error().to_string());
        auto m = babel::llvm37::read(bc.value());
        if (m.has_error())
            return cc::error(m.error().to_string());
        return analyze(m.value());
    }
    auto s = babel::spirv::read(bytes);
    if (s.has_error())
        return cc::error(cc::format("shan: neither a DXIL container nor a SPIR-V module: {}", s.error().to_string()));
    return analyze(s.value());
}
} // namespace shan
