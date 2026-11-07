#include "signature.hh"

#include <shaped-graphics-language/ir/ir.hh>

using namespace sgl;
using namespace sgl::ir;
using namespace sgl::ir::dxil;

using check::stage;

namespace
{
/// How a value the stage hands over reaches DXIL.
enum class reach : u8
{
    /// A signature element with a row.
    row,
    /// A signature element with no register.
    no_row,
    /// An operation of its own, in no signature.
    operation,
};

/// `D3D_NAME`.
constexpr u32 k_position = 1;
constexpr u32 k_render_target_array_index = 4;
constexpr u32 k_vertex_id = 6;
constexpr u32 k_primitive_id = 7;
constexpr u32 k_instance_id = 8;
constexpr u32 k_is_front_face = 9;
constexpr u32 k_sample_index = 10;
constexpr u32 k_quad_edge = 11;
constexpr u32 k_quad_inside = 12;
constexpr u32 k_tri_edge = 13;
constexpr u32 k_tri_inside = 14;
constexpr u32 k_line_detail = 15;
constexpr u32 k_line_density = 16;
constexpr u32 k_target = 64;
constexpr u32 k_depth = 65;
constexpr u32 k_coverage = 66;
constexpr u32 k_depth_greater_equal = 67;
constexpr u32 k_depth_less_equal = 68;

struct system_rule
{
    built_in system = built_in::none;
    reach how = reach::row;
    u32 value = 0;
};

constexpr system_rule k_systems[] = {
    {built_in::position, reach::row, k_position},
    {built_in::layer, reach::row, k_render_target_array_index},
    {built_in::vertex_index, reach::row, k_vertex_id},
    {built_in::instance_index, reach::row, k_instance_id},
    {built_in::is_front_facing, reach::row, k_is_front_face},
    {built_in::primitive_id, reach::row, k_primitive_id},
    {built_in::sample_index, reach::no_row, k_sample_index},
    {built_in::depth, reach::no_row, k_depth},
    {built_in::depth_greater_equal, reach::no_row, k_depth_greater_equal},
    {built_in::depth_less_equal, reach::no_row, k_depth_less_equal},
    {built_in::sample_mask, reach::no_row, k_coverage},
};

[[nodiscard]] constexpr bool is_part_of_a_signature(stage s)
{
    return s == stage::vertex || s == stage::pixel || s == stage::geometry || s == stage::tessellation_control
        || s == stage::tessellation_evaluation || s == stage::mesh;
}

/// `SV_PrimitiveID` is a row in a pixel stage alone, and in no signature but a geometry stage's otherwise.
[[nodiscard]] system_rule rule_of(program const& p, stage_element const& e)
{
    if (e.system == built_in::none)
        return {};
    // an input coverage is `dx.op.coverage`, where an output one is the signature's `oMask`
    if (e.system == built_in::sample_mask && e.part != stage_part::output)
        return {.system = e.system, .how = reach::operation};
    if (e.system == built_in::primitive_id && p.stage == stage::geometry)
        return {.system = e.system, .how = reach::no_row, .value = k_primitive_id};
    if (e.system == built_in::primitive_id && p.stage != stage::pixel)
        return {.system = e.system, .how = reach::operation};
    for (auto const& r : k_systems)
        if (r.system == e.system)
            return r;
    return {.system = e.system, .how = reach::operation};
}

/// `DxilProgramSigCompType`.
[[nodiscard]] u32 component_type_of(program const& p, type_ref t)
{
    auto const& info = p.at(t);
    auto const& scalar = info.kind == type_kind::vector ? p.at(info.element) : info;
    if (scalar.kind == type_kind::float_)
        return scalar.width == 16 ? 6 : scalar.width == 64 ? 9 : 3;
    if (scalar.kind == type_kind::bool_)
        return 1;
    auto const base = scalar.width == 16 ? 4u : scalar.width == 64 ? 7u : 1u;
    return base + (scalar.is_signed ? 1u : 0u);
}

[[nodiscard]] u32 lanes_of(program const& p, type_ref t)
{
    auto const& info = p.at(t);
    return info.kind == type_kind::vector ? info.count : 1;
}

/// One register of a signature: what it holds so far.
struct row
{
    /// Unset on a row only a tessellation factor holds, which takes any mode.
    interpolation mode = interpolation::none;
    bool has_mode = false;
    u32 next_column = 0;
    /// One past its last free column: a tessellation factor keeps column `w`.
    u32 limit = 4;
    bool has_system = false;
};

/// First fit in declared order, DXC's `-pack-prefix-stable`: the rule the golden test holds against DXC.
class packer
{
public:
    /// A row holds one mode, and an ordinary element never follows a system one in it.
    /// Types do not part a row, nor do a system element's lanes after an ordinary element's.
    void place(signature_element& e, bool is_system)
    {
        for (auto r = isize(0); r < _rows.size(); ++r)
        {
            auto& held = _rows[r];
            if ((held.has_mode && held.mode != e.mode) || held.next_column + e.lanes > held.limit
                || (held.has_system && !is_system))
                continue;
            return take(e, r, is_system);
        }
        _rows.push_back({});
        take(e, _rows.size() - 1, is_system);
    }

    /// A factor's own place: column `w` of a row, or `x` of a triangle's inside factor, and the row goes on holding links.
    void place_factor(signature_element& e, isize at, u32 column)
    {
        while (_rows.size() <= at)
            _rows.push_back({});
        e.row = i32(at);
        e.first_column = column;
        if (column == 3)
            _rows[at].limit = 3;
        else
            _rows[at].next_column = column + e.lanes;
    }

private:
    void take(signature_element& e, isize at, bool is_system)
    {
        auto& held = _rows[at];
        e.row = i32(at);
        e.first_column = held.next_column;
        held.next_column += e.lanes;
        held.mode = e.mode;
        held.has_mode = true;
        held.has_system = held.has_system || is_system;
    }

    cc::vector<row> _rows;
};

[[nodiscard]] signature_element element_of(program const& p, u32 index, signature_table table)
{
    auto const& e = p.stage_elements[index];
    return {.table = table,
            .element = index,
            .name = cc::string(p.name_at(e.semantic_first, e.semantic_size)),
            .semantic_index = e.semantic_index,
            .system_value = rule_of(p, e).value,
            .component_type = component_type_of(p, e.type),
            .lanes = lanes_of(p, e.type),
            .mode = e.interpolate};
}

/// A vertex stage's inputs each take a row of their own; a pixel stage's targets take the row of their index.
void pack_list(program const& p, cc::span<u32 const> elements, signature_table table, cc::vector<signature_element>& out)
{
    auto rows = packer();
    auto const is_vertex_input = p.stage == stage::vertex && table == signature_table::input;
    auto const is_target = p.stage == stage::pixel && table == signature_table::output;
    auto next_vertex_row = 0;
    for (auto const index : elements)
    {
        auto const rule = rule_of(p, p.stage_elements[index]);
        if (rule.system != built_in::none && rule.how == reach::operation)
            continue;
        auto e = element_of(p, index, table);
        if (rule.system != built_in::none && rule.how == reach::no_row)
            e.row = -1;
        else if (is_vertex_input)
            e.row = next_vertex_row++;
        else if (is_target)
        {
            e.row = p.stage_elements[index].location;
            e.system_value = k_target;
        }
        else
            rows.place(e, rule.system != built_in::none);
        out.push_back(cc::move(e));
    }
}

/// Patch constants: the tessellator's factors take their fixed places whatever they follow, and the links fill round them.
void pack_patch(program const& p, cc::span<u32 const> elements, cc::vector<signature_element>& out)
{
    auto edges = u32(0);
    auto insides = u32(0);
    for (auto const index : elements)
    {
        auto const system = p.stage_elements[index].system;
        edges += system == built_in::tess_edge ? 1 : 0;
        insides += system == built_in::tess_inside ? 1 : 0;
    }
    auto rows = packer();
    auto next_edge = u32(0);
    auto next_inside = u32(0);
    auto const first_at = out.size();
    for (auto const index : elements)
    {
        auto const system = p.stage_elements[index].system;
        if (system != built_in::tess_edge && system != built_in::tess_inside)
            continue;
        auto e = element_of(p, index, signature_table::patch);
        if (system == built_in::tess_edge)
        {
            e.system_value = edges == 4     ? k_quad_edge
                           : edges == 3     ? k_tri_edge
                           : next_edge == 0 ? k_line_density
                                            : k_line_detail;
            e.continues = next_edge > 0;
            rows.place_factor(e, next_edge++, 3);
        }
        else
        {
            e.system_value = insides == 2 ? k_quad_inside : k_tri_inside;
            e.continues = next_inside > 0;
            if (insides == 2)
                rows.place_factor(e, edges + next_inside++, 3);
            else
                rows.place_factor(e, edges, 0);
        }
        out.push_back(cc::move(e));
    }
    for (auto const index : elements)
    {
        auto const system = p.stage_elements[index].system;
        if (system == built_in::tess_edge || system == built_in::tess_inside)
            continue;
        auto e = element_of(p, index, signature_table::patch);
        e.mode = interpolation::none; // what no stage interpolates
        rows.place(e, false);
        out.push_back(cc::move(e));
    }
    // placed factors first, but `dx.sig` and `PSV0` state them as declared
    for (auto a = first_at + 1; a < out.size(); ++a)
        for (auto at = a; at > first_at && out[at - 1].element > out[at].element; --at)
            cc::swap(out[at - 1], out[at]);
}
} // namespace

cc::vector<signature_element> sgl::ir::dxil::pack_signature(program const& p)
{
    auto result = cc::vector<signature_element>();
    if (!is_part_of_a_signature(p.stage))
        return result;
    auto input = cc::vector<u32>();
    auto output = cc::vector<u32>();
    auto patch = cc::vector<u32>();
    for (auto i = u32(0); i < u32(p.stage_elements.size()); ++i)
        switch (p.stage_elements[i].part)
        {
        case stage_part::input:
            input.push_back(i);
            break;
        case stage_part::output:
            output.push_back(i);
            break;
        case stage_part::patch:
            patch.push_back(i);
            break;
        case stage_part::primitive:
            // a pixel stage packs what a mesh stage wrote per triangle after its link, as one list
            (p.stage == stage::pixel ? input : patch).push_back(i);
            break;
        }
    pack_list(p, input, signature_table::input, result);
    pack_list(p, output, signature_table::output, result);
    if (p.stage == stage::mesh)
        pack_list(p, patch, signature_table::patch, result);
    else
        pack_patch(p, patch, result);
    return result;
}
