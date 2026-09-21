#include "../emit/emit-test-support.hh"

#include <shaped-graphics-language/driver/describe.hh>

using namespace sgl_test;
using sgl::check::stage;

// `describe` is what a build generates host C++ from, so what it says has to be exactly what the emitter builds.
// Everything below is therefore pinned against a shader the emitter already compiles, or against one it refuses.

namespace
{
sgl::module_description described(cc::string_view source)
{
    auto const r = sgl::describe({.source = source, .source_name = "t.sgl"});
    if (r.has_error())
        FAIL(r.error());
    return r.value();
}

cc::string error_of(cc::string_view source)
{
    auto const r = sgl::describe({.source = source, .source_name = "t.sgl"});
    REQUIRE(r.has_error());
    return r.error();
}
} // namespace

TEST("sgl describe - the cube: an inline block, a vertex input, a target set and two entry points")
{
    auto const d = described(read_text(cc::string(SGL_SAMPLES_DIR) + "/cube.sgl"));

    REQUIRE(d.bindings.size() == 1);
    auto const& constants = d.bindings[0];
    CHECK(constants.name == "constants");
    CHECK(constants.is_inline);
    CHECK(constants.block_size == 64);
    REQUIRE(constants.members.size() == 1);
    CHECK(constants.members[0].kind == sgl::described_member_kind::constant);
    CHECK(constants.members[0].type == "mat4");
    CHECK(constants.members[0].offset == 0);
    CHECK(constants.members[0].size == 64);

    REQUIRE(d.structs.size() == 2);
    CHECK(d.structs[0].name == "cube_vertex");
    CHECK(d.structs[0].edge == stage::vertex);
    REQUIRE(d.structs[0].members.size() == 3);
    // The SGL spelling, not a host one: three different types that are all three `vec3f` to sg.
    CHECK(d.structs[0].members[0].type == "pos3");
    CHECK(d.structs[0].members[1].type == "vec3");
    CHECK(d.structs[0].members[2].type == "float3");
    CHECK(d.structs[0].members[2].location == 2);
    CHECK(d.structs[1].name == "target");
    CHECK(d.structs[1].edge == stage::pixel);

    REQUIRE(d.entry_points.size() == 2);
    CHECK(d.entry_points[0].name == "main_vs");
    CHECK(d.entry_points[0].stage == stage::vertex);
    REQUIRE(d.entry_points[0].bindings.size() == 1);
    CHECK(d.entry_points[0].bindings[0] == "constants");
    CHECK(d.entry_points[1].stage == stage::pixel);
    CHECK(d.entry_points[1].bindings.empty());
}

TEST("sgl describe - a buffer group numbers its buffers and names each as the shader reflects it")
{
    auto const d = described(R"(binding work:
    src: buffer[float]
    dst: mut buffer[float]

@compute(64, 2) fun main(@thread_id id: int3){work}:
    work.dst[id.x] = work.src[id.x]
)");

    REQUIRE(d.bindings.size() == 1);
    auto const& work = d.bindings[0];
    CHECK(!work.is_inline);
    REQUIRE(work.members.size() == 2);
    CHECK(work.members[0].kind == sgl::described_member_kind::buffer);
    CHECK(work.members[0].type == "float");
    CHECK(!work.members[0].is_mut);
    CHECK(work.members[0].slot == 0);
    CHECK(work.members[0].reflected_name == "work_src");
    CHECK(work.members[1].is_mut);
    CHECK(work.members[1].slot == 1);
    CHECK(work.members[1].reflected_name == "work_dst");

    REQUIRE(d.entry_points.size() == 1);
    CHECK(d.entry_points[0].stage == stage::compute);
    CHECK(d.entry_points[0].workgroup[0] == 64);
    CHECK(d.entry_points[0].workgroup[1] == 2);
    CHECK(d.entry_points[0].workgroup[2] == 1);
}

TEST("sgl describe - a group no entry point lists is still described, and still judged")
{
    // A shader file is a library, so the host may bind a group this file's own entry points never name.
    auto const d = described(R"(binding spare:
    values: buffer[float]
)");
    REQUIRE(d.bindings.size() == 1);
    CHECK(d.bindings[0].name == "spare");
    CHECK(d.entry_points.empty());

    // And an unlisted group the emitter could not write is refused as if something listed it.
    auto const error = error_of(R"(binding spare:
    scale: float
)");
    CHECK(error.contains("unsupported"));
    CHECK(error.contains("spare.scale"));
}

TEST("sgl describe - what the emitter refuses is refused here, in the emitter's words")
{
    // An @inline binding that does not stand last would move every group behind it under the host.
    auto const error = error_of(R"(@inline binding c:
    scale: float

binding work:
    values: mut buffer[float]

@compute(64) fun main(@thread_id id: int3){c, work}:
    work.values[id.x] = c.scale
)");
    CHECK(error.contains("not the last of the list"));
}

TEST("sgl describe - a source with errors describes nothing, and says why")
{
    auto const error = error_of("fun f() -> float => nope\n");
    CHECK(error.contains("t.sgl:1:"));
    CHECK(error.contains("unknown-name"));
}
