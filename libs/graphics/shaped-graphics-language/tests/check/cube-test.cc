#include "check-test-support.hh"

using namespace sgl_test;

namespace
{
cc::string read_cube()
{
    return read_text(cc::string(SGL_SAMPLES_DIR) + "/cube.sgl");
}
} // namespace

TEST("sgl check - the cube checks without a diagnostic, and its whole dump is pinned")
{
    auto const checked = check_sources(read_prelude(), read_cube());
    CHECK(reports_of(checked) == "");
    // The prelude's symbols stand in front, and the prelude grows: what is pinned whole is the program's share.
    auto const dump = sgl::check::dump(checked.module);
    CHECK(dump.starts_with("(struct float builtin opaque)\n"
                           "(struct float3 builtin (x : float) (y : float) (z : float))\n"));
    CHECK(dump.contains("(fun dot builtin pure (a : vec3) (b : vec3) -> float)\n"));
    CHECK(dump.contains("(fun transform_position builtin pure operator:* (m : mat4) (p : pos3) -> hpos4)\n"));
    auto const own = dump.find("(binding constants");
    REQUIRE(own >= 0);
    CHECK(cc::string_view(dump).subview({.start = own, .end = dump.size()})
          == "(binding constants inline (view_projection : mat4))\n"
             "(struct cube_vertex vertex (position : pos3) (normal : vec3) (color : float3))\n"
             "(struct target pixel (color : float4))\n"
             "(struct pixel_input (position{@position} : hpos4) (normal : vec3) (color : float3))\n"
             "(fun main_vs vertex (v : cube_vertex) (uses constants) -> pixel_input)\n"
             "(fun main_ps pixel (p : pixel_input) -> target)\n"
             "(entry vertex main_vs (v : cube_vertex) (uses constants) -> pixel_input\n"
             "  (return (construct\n"
             "    (call transform_position (binding constants view_projection : mat4) "
             "(member (local v : cube_vertex) position : pos3) : hpos4)\n"
             "    (member (local v : cube_vertex) normal : vec3)\n"
             "    (member (local v : cube_vertex) color : float3) : pixel_input)))\n"
             "(entry pixel main_ps (p : pixel_input) -> target\n"
             "  (let n : vec3 = (call normalize (member (local p : pixel_input) normal : vec3) : vec3))\n"
             "  (let key : float = (call saturate (call dot (local n : vec3) (call normalize (construct "
             "(lit 0.45 : float) (lit 0.8 : float) (lit -0.4 : float) : vec3) : vec3) : float) : float))\n"
             "  (let fill : float = (call saturate (call dot (local n : vec3) (call normalize (construct "
             "(lit -0.7 : float) (lit 0.15 : float) (lit 0.6 : float) : vec3) : vec3) : float) : float))\n"
             "  (let lit : float3 = (call scale_color (member (local p : pixel_input) color : float3) "
             "(call add (call add (lit 0.25 : float) (call multiply (lit 0.8 : float) (local key : float) : float) : "
             "float) "
             "(call multiply (lit 0.25 : float) (local fill : float) : float) : float) : float3))\n"
             "  (return (construct\n"
             "    (construct (member (local lit : float3) x : float) (member (local lit : float3) y : float) "
             "(member (local lit : float3) z : float) (lit 1.0 : float) : float4) : target)))\n");

    // What an emitter needs beyond the tree: the stage, the edge structs, the bindings and whether they ride inline.
    auto const& m = checked.module;
    REQUIRE(m.entry_points.size() == 2);
    auto const& vs = m.entry_points[0];
    CHECK(vs.entry_stage == sgl::check::stage::vertex);
    CHECK(vs.name == "main_vs");
    CHECK(m.name_of(vs.input) == "cube_vertex");
    CHECK(m.name_of(vs.result) == "pixel_input");
    REQUIRE(vs.bindings.size() == 1);
    CHECK(m.at(vs.bindings[0]).name == "constants");
    CHECK(m.bindings[m.at(vs.bindings[0]).info].is_inline);
    CHECK(m.entry_points[1].bindings.empty());
    // every name a local could collide with is taken before the first local is minted
    CHECK(m.entry_points[1].names.is_taken("normalize"));
    CHECK(m.entry_points[1].names.is_taken("main_vs"));
    CHECK(m.entry_points[1].names.is_taken("lit"));

    // a value type: checking twice gives equal modules
    CHECK(check_sources(read_prelude(), read_cube()).module == m);
}

TEST("sgl check - the pass is total: every truncation of the cube checks, settles every symbol and dumps")
{
    auto const prelude = read_prelude();
    auto const cube = read_cube();
    // A prime stride cuts through every kind of token over the length of the file.
    for (auto length = isize(0); length < cube.size(); length += 13)
    {
        auto const checked = check_sources(prelude, cc::string_view(cube).subview({.offset = 0, .size = length}));
        auto const& m = checked.module;

        auto all_settled = true;
        for (auto const& s : m.symbols)
            all_settled = all_settled
                       && (s.state == sgl::check::symbol_state::checked || s.state == sgl::check::symbol_state::failed);
        CHECK(all_settled);
        CHECK(m.files.size() == 2);
        CHECK(m.files[1].type_of.size() == checked.user_ast.exprs.size());
        CHECK(m.entry_points.size() <= 2);
        // the dumps walk every id the module holds
        CHECK(!sgl::check::dump(m).empty());
        (void)sgl::check::dump_diagnostics(m);
    }
}

TEST("sgl check - a truncated prelude checks too")
{
    auto const prelude = read_prelude();
    auto const cube = read_cube();
    for (auto length = isize(0); length < prelude.size(); length += 13)
    {
        auto const checked = check_sources(cc::string_view(prelude).subview({.offset = 0, .size = length}), cube);
        CHECK(checked.module.files.size() == 2);
        (void)sgl::check::dump(checked.module);
    }
}
