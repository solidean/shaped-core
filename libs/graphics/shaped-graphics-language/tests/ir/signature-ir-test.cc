#include "ir-test-support.hh"

#include <shaped-graphics-language/ir/dxil/signature.hh>

using namespace cc::primitive_defines;

// The packing rule stated by hand, element by element, wherever DXC is not there to compare with.
// libs/graphics/shaped-shader-library/tests/sgl-signature-golden-test.cc holds the same rule against DXC itself.

namespace
{
using namespace sgl::ir;
using sgl::check::stage;
using sgl::ir::dxil::pack_signature;
using sgl::ir::dxil::signature_table;

struct stage_program
{
    program p;
    type_ref floats[5] = {};

    explicit stage_program(stage s)
    {
        p.stage = s;
        p.types.push_back({.kind = type_kind::void_});
        p.types.push_back({.kind = type_kind::float_, .width = 32});
        floats[1] = type_ref(1);
        for (auto lanes = u32(2); lanes <= 4; ++lanes)
        {
            p.types.push_back({.kind = type_kind::vector, .count = lanes, .element = floats[1]});
            floats[lanes] = type_ref(p.types.size() - 1);
        }
        p.names = "SGL";
    }

    void add(stage_part part, u32 lanes, interpolation mode, built_in system = built_in::none, i32 location = -1)
    {
        p.stage_elements.push_back({.part = part,
                                    .system = system,
                                    .type = floats[lanes],
                                    .interpolate = mode,
                                    .location = location,
                                    .semantic_size = 3,
                                    .semantic_index = u32(p.stage_elements.size())});
    }
};
} // namespace

TEST("sgl signature - an element takes the first row with room, whatever came between")
{
    auto s = stage_program(stage::pixel);
    s.add(stage_part::input, 3, interpolation::perspective);
    s.add(stage_part::input, 3, interpolation::perspective);
    s.add(stage_part::input, 1, interpolation::perspective);
    s.add(stage_part::input, 1, interpolation::perspective);
    auto const packed = pack_signature(s.p);
    REQUIRE(packed.size() == 4);
    CHECK(packed[0].row == 0);
    CHECK(packed[1].row == 1);
    CHECK(packed[2].row == 0);
    CHECK(packed[2].first_column == 3);
    CHECK(packed[3].row == 1);
    CHECK(packed[3].first_column == 3);
}

TEST("sgl signature - a row holds one interpolation mode")
{
    auto s = stage_program(stage::pixel);
    s.add(stage_part::input, 1, interpolation::perspective);
    s.add(stage_part::input, 1, interpolation::flat);
    s.add(stage_part::input, 1, interpolation::perspective);
    auto const packed = pack_signature(s.p);
    REQUIRE(packed.size() == 3);
    CHECK(packed[1].row == 1);
    CHECK(packed[2].row == 0);
    CHECK(packed[2].first_column == 1);
}

TEST("sgl signature - an ordinary element never follows a system one in a row, and a system one may follow it")
{
    auto s = stage_program(stage::vertex);
    s.add(stage_part::output, 1, interpolation::flat);
    s.add(stage_part::output, 1, interpolation::flat, built_in::layer);
    s.add(stage_part::output, 1, interpolation::flat);
    auto const packed = pack_signature(s.p);
    REQUIRE(packed.size() == 3);
    CHECK(packed[1].row == 0);
    CHECK(packed[1].first_column == 1);
    CHECK(packed[2].row == 1);
}

TEST("sgl signature - a vertex input takes a row of its own, and a render target the row of its index")
{
    auto vertex = stage_program(stage::vertex);
    vertex.add(stage_part::input, 1, interpolation::none);
    vertex.add(stage_part::input, 1, interpolation::none);
    auto const inputs = pack_signature(vertex.p);
    REQUIRE(inputs.size() == 2);
    CHECK(inputs[1].row == 1);
    CHECK(inputs[1].first_column == 0);

    auto pixel = stage_program(stage::pixel);
    pixel.add(stage_part::output, 4, interpolation::none, built_in::none, 2);
    auto const outputs = pack_signature(pixel.p);
    REQUIRE(outputs.size() == 1);
    CHECK(outputs[0].table == signature_table::output);
    CHECK(outputs[0].row == 2);
}

TEST("sgl signature - tessellation factors keep their places, and links fill round them")
{
    auto s = stage_program(stage::tessellation_control);
    s.add(stage_part::patch, 3, interpolation::none);
    for (auto i = 0; i < 3; ++i)
        s.add(stage_part::patch, 1, interpolation::none, built_in::tess_edge);
    s.add(stage_part::patch, 1, interpolation::none, built_in::tess_inside);
    auto const packed = pack_signature(s.p);
    REQUIRE(packed.size() == 5);
    // the factors take their places ahead of the link, and the list stands as the struct declared it
    CHECK(packed[0].row == 0);
    CHECK(packed[0].first_column == 0);
    CHECK(packed[0].lanes == 3);
    CHECK(packed[1].row == 0);
    CHECK(packed[1].first_column == 3);
    CHECK(!packed[1].continues);
    CHECK(packed[2].continues);
    CHECK(packed[3].row == 2);
    CHECK(packed[3].continues);
    CHECK(packed[4].row == 3);
    CHECK(packed[4].first_column == 0);
    CHECK(!packed[4].continues);
}

TEST("sgl signature - what DXIL states with no row, or as an operation, takes no place")
{
    auto s = stage_program(stage::pixel);
    s.add(stage_part::input, 1, interpolation::flat, built_in::sample_index);
    s.add(stage_part::input, 1, interpolation::none, built_in::view_index);
    s.add(stage_part::input, 1, interpolation::perspective);
    auto const packed = pack_signature(s.p);
    REQUIRE(packed.size() == 2);
    CHECK(packed[0].row == -1);
    CHECK(packed[1].row == 0);

    auto compute = stage_program(stage::compute);
    compute.add(stage_part::input, 3, interpolation::none, built_in::thread_id);
    CHECK(pack_signature(compute.p).empty());
}
