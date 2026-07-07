#include <nexus/test.hh>
#include <shaped-shader-compiler-dxc/all.hh>

// The preprocess step flattens #includes via a caller-supplied resolver (no file I/O) and expands
// macros, without generating bytecode.

TEST("ssc::dxc preprocess - resolves an include via the resolver and expands its macro")
{
    auto comp = ssc::dxc::compiler::create();
    REQUIRE(comp.has_value());

    ssc::dxc::shader_description desc;
    desc.stage = sg::shader_stage::compute;
    desc.source = "#include \"common.hlsli\"\n"
                  "static const uint kValue = MAGIC;\n";

    // Virtual file system: map the include path to in-memory source. DXC may hand us the path with a
    // leading "./" (or a normalized form), so match on the basename.
    auto resolver = [](cc::string_view path) -> cc::optional<cc::string>
    {
        if (path.contains("common.hlsli"))
            return cc::string("#define MAGIC 1234u\n");
        return {};
    };

    auto pp = comp.value().preprocess(desc, resolver);
    REQUIRE(pp.has_value());

    // The macro should be expanded in the flattened output...
    CHECK(cc::string_view(pp.value().source).contains("1234u"));
    // ...and the directive itself gone (it was preprocessed away).
    CHECK(!cc::string_view(pp.value().source).contains("#include"));
}

TEST("ssc::dxc preprocess - an unresolved include is an error")
{
    auto comp = ssc::dxc::compiler::create();
    REQUIRE(comp.has_value());

    ssc::dxc::shader_description desc;
    desc.stage = sg::shader_stage::compute;
    desc.source = "#include \"does_not_exist.hlsli\"\n";

    auto resolver = [](cc::string_view) -> cc::optional<cc::string> { return {}; };

    auto pp = comp.value().preprocess(desc, resolver);
    CHECK(pp.has_error());
}
