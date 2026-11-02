#include "fake_compiler.hh"

#include <clean-core/container/pinned_data.hh>
#include <nexus/test.hh>
#include <shaped-shader-library/impl/shader_bundle.hh>
#include <shaped-shader-library/shader_bundle.hh>
#include <shaped-shader-library/shader_library.hh>

#include <memory>

using namespace cc::primitive_defines;

// A shader bundle: bytecode a recording run compiled, served by a later run without compiling.
// The registry is process-wide, so each test bundles a source no other test compiles.

using slib_test::fake_compiler;

namespace
{
sg::compiled_shader const& await(sg::async_compiled_shader const& shader)
{
    REQUIRE(shader != nullptr);
    REQUIRE(shader->is_ready());
    REQUIRE(shader->has_value());
    return *shader->try_value();
}

sg::compiled_shader precompiled(cc::string_view bytecode)
{
    auto shader = sg::compiled_shader();
    shader.stage = sg::shader_stage::compute;
    shader.format = sg::shader_format::dxil;
    shader.entry_point = "main";
    shader.bytecode = cc::pinned_data<byte const>(cc::pinned_data<byte>::create_copy_of(bytecode.as_bytes()));
    shader.compiler = sg::compiler_info{.name = "recorded", .version = "1", .signature = "main"};
    return shader;
}
} // namespace

TEST("slib - a bundled shader is served without compiling it", exclusive("slib-shader-library"))
{
    slib::shader_library lib;
    auto compiler = std::make_unique<fake_compiler>(slib::shader_language::hlsl, sg::shader_format::dxil);
    auto const* const fake = compiler.get();
    lib.add_compiler(cc::move(compiler));

    // HLSL is keyed once flattened, and this text includes nothing, so its flattened form is itself.
    auto const key
        = slib::impl::bundle_key(slib::shader_language::hlsl, sg::shader_format::dxil, sg::shader_stage::compute,
                                 "main", {}, {}, "a body only the bundle test compiles", {});
    // Leaked on purpose: a registered bundle must outlive the process, as embedded data does.
    auto* const bytes = new cc::vector<byte>(slib::impl::make_bundle(key, precompiled("from the bundle")));
    slib::register_shader_bundle(*bytes);

    auto const node = lib.compile_source("a body only the bundle test compiles", sg::shader_stage::compute, "main",
                                         sg::shader_format::dxil);
    auto const& shader = await(node);
    CHECK(fake_compiler::source_of(shader) == "from the bundle");
    CHECK(shader.compiler.name == "recorded");
    CHECK(fake->compile_count() == 0);

    // Anything else still compiles as it always did.
    auto const other_node
        = lib.compile_source("a body no bundle holds", sg::shader_stage::compute, "main", sg::shader_format::dxil);
    auto const& other = await(other_node);
    CHECK(fake_compiler::source_of(other) == "a body no bundle holds");
    CHECK(fake->compile_count() == 1);
}

TEST("slib - a bundle key ignores carriage returns and the order modules are listed in")
{
    auto const a = slib::module_source{.path = "mods/a.sgl", .text = "module a\n"};
    auto const b = slib::module_source{.path = "mods/b.sgl", .text = "module b\n"};
    auto const b_crlf = slib::module_source{.path = "mods/b.sgl", .text = "module b\r\n"};

    auto const key = [](cc::string_view source, std::initializer_list<slib::module_source> modules)
    {
        return slib::impl::bundle_key(slib::shader_language::sgl, sg::shader_format::dxil, sg::shader_stage::compute,
                                      "main", {}, "ns", source,
                                      cc::span<slib::module_source const>(modules.begin(), modules.size()));
    };

    // A checkout may hold CRLF files while the embedded copy holds LF, and the two must find each other's shaders.
    CHECK(key("use a\nbody\n", {a, b}) == key("use a\r\nbody\r\n", {a, b_crlf}));
    // The disk and the embedded copy may list a module directory in different orders.
    CHECK(key("use a\nbody\n", {a, b}) == key("use a\nbody\n", {b, a}));

    // What a compile reads still separates two keys.
    CHECK(key("use a\nbody\n", {a, b}) != key("use a\nother\n", {a, b}));
    CHECK(key("use a\nbody\n", {a, b}) != key("use a\nbody\n", {a}));
}

TEST("slib - a bundled SGL shader skips the front end and the compiler both", exclusive("slib-shader-library"))
{
    // No compiler is registered, and the text is not SGL: a hit reaches neither.
    slib::shader_library lib;

    auto const source = cc::string_view("not SGL, so only a bundle can serve it");
    auto const key = slib::impl::bundle_key(slib::shader_language::sgl, sg::shader_format::dxil,
                                            sg::shader_stage::compute, "main", {}, {}, source, {});
    auto* const bytes = new cc::vector<byte>(slib::impl::make_bundle(key, precompiled("sgl from the bundle")));
    slib::register_shader_bundle(*bytes);

    auto const node = lib.compile_source(source, sg::shader_stage::compute, "main", sg::shader_format::dxil,
                                         {.language = slib::shader_language::sgl});
    CHECK(fake_compiler::source_of(await(node)) == "sgl from the bundle");
}

TEST("slib - a key a bundle holds twice serves its first record", exclusive("slib-shader-library"))
{
    auto const key = slib::impl::bundle_key(slib::shader_language::hlsl, sg::shader_format::dxil,
                                            sg::shader_stage::compute, "main", {}, {}, "a key recorded twice", {});

    // A recording appends every compile it sees, so the second record arrives after the header of the first.
    auto* const bytes = new cc::vector<byte>(slib::impl::make_bundle(key, precompiled("first")));
    auto const second = slib::impl::make_bundle(key, precompiled("second"));
    bytes->push_back_range(cc::span<byte const>(second).subspan(12)); // past the header
    slib::register_shader_bundle(*bytes);

    auto const found = slib::impl::find_bundled_shader(key);
    REQUIRE(found.has_value());
    CHECK(fake_compiler::source_of(found.value()) == "first");
}

TEST("slib - a bundle cut short serves the records before the cut", exclusive("slib-shader-library"))
{
    nx::expect_error("*does not parse*", nx::exactly(1));

    auto const whole = slib::impl::bundle_key(slib::shader_language::hlsl, sg::shader_format::dxil,
                                              sg::shader_stage::compute, "main", {}, {}, "a record before the cut", {});
    auto const cut = slib::impl::bundle_key(slib::shader_language::hlsl, sg::shader_format::dxil,
                                            sg::shader_stage::compute, "main", {}, {}, "a record the cut ends", {});

    // What a recording leaves when its run dies mid-write: one record whole, then part of the next.
    auto* const bytes = new cc::vector<byte>(slib::impl::make_bundle(whole, precompiled("whole")));
    auto const partial = slib::impl::make_bundle(cut, precompiled("partial"));
    bytes->push_back_range(cc::span<byte const>(partial).subspan({.offset = 12, .size = partial.size() - 12 - 1}));
    slib::register_shader_bundle(*bytes);

    auto const found = slib::impl::find_bundled_shader(whole);
    REQUIRE(found.has_value());
    CHECK(fake_compiler::source_of(found.value()) == "whole");
    CHECK(!slib::impl::find_bundled_shader(cut).has_value());
}
