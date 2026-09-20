#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/ast/build.hh>
#include <shaped-graphics-language/ast/dump.hh>
#include <shaped-graphics-language/debug/dump.hh>
#include <shaped-graphics-language/syntax/parsed_file.hh>

using namespace cc::primitive_defines;

namespace
{
cc::string read_sample(cc::string_view name)
{
    auto adapter = cc::file_read_stream_adapter::open(cc::string(SGL_SAMPLES_DIR) + "/" + name);
    REQUIRE(adapter.has_value());
    auto stream = adapter.value().stream();
    auto const bytes = stream.read_all();
    REQUIRE(bytes.has_value());
    return cc::string(reinterpret_cast<char const*>(bytes.value().data()), bytes.value().size());
}
} // namespace

TEST("sgl samples - a whole raster shader parses without a single diagnostic")
{
    auto const file = sgl::parse(read_sample("basic-raster.sgl"));

    CHECK(sgl::dump_diagnostics(file) == "");
    CHECK(sgl::print_source(file) == file.source);

    auto const forms = sgl::dump_forms(file);
    // A binding list is a fused curly list after the parameters, so a signature is a call of a call.
    CHECK(forms.contains("(call (call id:make_mvp (round (run id:model op:: id:mat4))) (curly id:frame))"));
    // An anonymous return type spans element lines and closes on the line that carries the block colon.
    CHECK(forms.contains("op:-> (curly (run id:pos op:: id:hpos4){@position} (run id:normal op:: id:vec3)"));
    CHECK(forms.contains("(apply id:make_mvp (member model id:instance))"));
    // `sampler` is a keyword, so a static sampler is a keyword form like any other declaration.
    CHECK(forms.contains("(kw kw:sampler id:bilinear)"));
    // A splat is a prefix operator; the postfix spelling stays reserved.
    CHECK(forms.contains("(round (prefix .. id:normal) num:0)"));
}

TEST("sgl samples - the raster shader builds an AST without a diagnostic and without an invalid node")
{
    auto const file = sgl::parse(read_sample("basic-raster.sgl"));
    auto const ast = sgl::ast::build(file);

    CHECK(sgl::ast::dump_diagnostics(ast) == "");
    auto const dump = sgl::ast::dump(file, ast);
    CHECK(!dump.contains("invalid"));
    CHECK(!dump.contains("<missing>"));

    CHECK(dump.starts_with("(module example)\n(binding frame\n  (field view : mat4)\n"));
    CHECK(dump.contains("(field tex_color : (index texture2d rgba8))"));
    // The sample's sampler has only a comment under it.
    CHECK(dump.contains("(sampler bilinear)\n"));
    CHECK(dump.contains("(struct{@vertex} basic_vertex\n  (field pos : pos3)\n"));
    CHECK(dump.contains("(fun make_mvp (params (field model : mat4)) (uses frame) => "
                        "(call:infix * (call:infix * (member frame proj) (member frame view)) model))"));
    // An anonymous return type is a struct type, and a semantic is an attribute on its field.
    CHECK(dump.contains("(fun{@vertex} my_vs (params (field v : basic_vertex)) (uses frame instance) -> "
                        "(struct-type (field{@position} pos : hpos4) (field normal : vec3) (field uv : vec2))\n"
                        "  (let mvp = (call:juxt make_mvp (member instance model)))\n"));
    CHECK(dump.contains("normal=(call:infix * (cast mvp : mat3) (member v normal))"));
    CHECK(dump.contains("  (use brdf_library as brdf)\n"));
    CHECK(dump.contains("base_color=(call:paren (member (member instance tex_color) sample) bilinear uv)"));
    CHECK(dump.contains("normal=(cast (tuple ..normal num:0) : vec4f16)"));
}

TEST("sgl samples - members and bindings parse and build without a diagnostic")
{
    auto const file = sgl::parse(read_sample("members-and-bindings.sgl"));
    CHECK(sgl::dump_diagnostics(file) == "");
    CHECK(sgl::print_source(file) == file.source);

    auto const ast = sgl::ast::build(file);
    CHECK(sgl::ast::dump_diagnostics(ast) == "");
    auto const dump = sgl::ast::dump(file, ast);
    CHECK(!dump.contains("invalid"));
    CHECK(!dump.contains("<missing>"));

    CHECK(dump.contains("(binding scene = (tuple frame timing))\n(binding main_pass = scene)\n"));
    CHECK(dump.contains("  (property is_local => (call:infix != self .directional))\n"));
    CHECK(dump.contains("    (arm _ => num:1.0)"));
    CHECK(dump.contains("  (fun dim (params (field mut self) (field factor : float))\n"
                        "    (assign *= (member self intensity) factor))\n"));
    CHECK(dump.contains("      (arm .directional => (return true))\n"));
    CHECK(dump.contains("position=(call:paren pos3 ..direction)"));
    CHECK(dump.contains("(type radiance_sample : (tuple vec3 float))\n"));
    // An arm block says what it hands on.
    CHECK(dump.contains("        (let base = num:10.0)\n        (yield (call:infix * base intensity)))"));
    // A local binding with a property that reaches a local, then a nested function and a lambda.
    CHECK(dump.contains("  (binding timing\n    (field time : float)\n    (property phase => (call:infix * t "
                        "num:0.5)))\n"));
    CHECK(dump.contains("  (fun weight (params (field l : light)) -> float => "));
    CHECK(dump.contains("  (let contribution = (lambda (params (field l)) => "));
    CHECK(dump.contains("      (branch (call:infix >= bounce max_bounces) => (break total))"));
    CHECK(dump.contains("(chain num:0.0 <= (call:paren (member brdf luminance) total) < num:1.0)"));
}

TEST("sgl samples - control flow parses and builds without a diagnostic")
{
    auto const file = sgl::parse(read_sample("control-flow.sgl"));
    CHECK(sgl::dump_diagnostics(file) == "");
    CHECK(sgl::print_source(file) == file.source);

    auto const ast = sgl::ast::build(file);
    CHECK(sgl::ast::dump_diagnostics(ast) == "");
    auto const dump = sgl::ast::dump(file, ast);
    CHECK(!dump.contains("invalid"));
    CHECK(!dump.contains("<missing>"));

    CHECK(dump.contains("  (case linear = num:0)\n  (case reinhard = num:1)\n  (case filmic = num:2)\n  (case "
                        "custom)\n"));
    CHECK(dump.contains("(const{@slider(num:0.0 max=num:16.0 step=num:0.25)} exposure = num:1.0)\n"));
    CHECK(dump.contains("(type curve : (function-type (params (field : float)) -> float))\n"));
    CHECK(dump.contains("  (field apply_to : (function-type (params (field : float)) -> float))\n"));
    CHECK(dump.contains("  (property shoulder\n"
                        "    (let w = (call:infix * white_point white_point))\n"
                        "    (yield (call:infix / num:1.0 w)))\n"));
    // A `yield` whose value is an arrow lambda, whose block yields in turn.
    CHECK(dump.contains("      (arm .filmic\n"
                        "        (let s = shoulder)\n"
                        "        (yield (lambda (params (field x))\n"
                        "          (let scaled = "));
    CHECK(dump.contains("          (yield (call:infix / scaled (call:infix + num:1.0 x))))))\n"));
    CHECK(dump.contains("(fun apply (params (field f : (function-type (params (field : float)) -> float)) (field x : "
                        "float)) => (call:juxt f x))\n"));
    CHECK(dump.contains("  (let safe = (lambda:fun (params (field x : float)) -> float\n"
                        "    (if\n"
                        "      (branch (call:infix <= x num:0.0) => (return num:0.0)))\n"));
    CHECK(dump.contains("  (let twice = (lambda:fun (type-params (field T)) (params (field g : (function-type (params "
                        "(field : T)) -> T)) (field x : T)) => (call:paren g (call:paren g x))))\n"));
    // A statement block between a `yield` and its arm is looked through.
    CHECK(dump.contains("        (if\n"
                        "          (branch (call:infix > (index color i) (member t white_point))\n"
                        "            (yield num:1.0)))\n"
                        "        (yield (call:paren safe (index color i))))"));
}

TEST("sgl samples - the AST pass is total: every truncation of a sample builds, and every node keeps a form")
{
    for (auto const name : {"basic-raster.sgl", "members-and-bindings.sgl", "control-flow.sgl"})
    {
        auto const source = read_sample(name);
        // A prime stride cuts through every kind of token over the length of a file.
        for (auto length = isize(0); length < source.size(); length += 13)
        {
            auto const file = sgl::parse(cc::string(cc::string_view(source).subview({.offset = 0, .size = length})));
            auto const ast = sgl::ast::build(file);
            auto const dump = sgl::ast::dump(file, ast);

            auto all_have_forms = true;
            for (auto const& e : ast.exprs)
                all_have_forms = all_have_forms && sgl::is_valid(e.form);
            for (auto const& s : ast.stmts)
                all_have_forms = all_have_forms && sgl::is_valid(s.form);
            for (auto const& d : ast.decls)
                all_have_forms = all_have_forms && sgl::is_valid(d.form);
            CHECK(all_have_forms);
            CHECK(dump.empty() == ast.declarations.empty());
        }
    }
}
