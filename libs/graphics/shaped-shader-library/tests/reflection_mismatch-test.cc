#include <nexus/test.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-shader-library/shader_asset.hh>

// A generated package's `check_reflection` is only as strict as this, and a package whose shaders all fit never shows it refusing.

namespace
{
sg::binding const work[] = {
    {.name = "work_src", .index = 0, .type = sg::binding_type::buffer},
    {.name = "work_dst", .index = 1, .type = sg::binding_type::buffer, .access = sg::access_mode::read_write},
};

sg::compiled_shader reflecting(std::initializer_list<sg::binding> bindings)
{
    auto shader = sg::compiled_shader{};
    for (auto const& b : bindings)
        shader.bindings.push_back(b);
    return shader;
}

cc::string mismatch_of(sg::compiled_shader const& compiled, cc::optional<sg::binding> const& inline_block = {})
{
    slib::listed_group const listed[] = {{.position = 1, .bindings = work}};
    return slib::reflection_mismatch("t.sgl:main", compiled, listed, inline_block);
}
} // namespace

TEST("slib - reflection fits the listed groups by name, position, index, count and kind")
{
    // A descriptor set and a register space both say the position, and a binding the shader never reads may be missing.
    CHECK(mismatch_of(reflecting({{.name = "work_dst",
                                   .group_index = 1u,
                                   .index = 1,
                                   .type = sg::binding_type::buffer,
                                   .access = sg::access_mode::read_write}}))
          == "");
    CHECK(mismatch_of(reflecting({{.name = "work_src", .space = 1u, .index = 0, .type = sg::binding_type::buffer}}))
          == "");

    CHECK(mismatch_of(reflecting({{.name = "stray", .group_index = 1u}}))
              .contains("reflects 'stray', which no group of the layout declares"));
    CHECK(mismatch_of(reflecting({{.name = "work_src", .group_index = 0u, .index = 0, .type = sg::binding_type::buffer}}))
              .contains("'work_src' is in set 0, and the layout has its group at slot 1"));
    CHECK(mismatch_of(reflecting({{.name = "work_dst", .group_index = 1u, .index = 1, .type = sg::binding_type::buffer}}))
              .contains("'work_dst' reflects as index 1"));
}

TEST("slib - the listed @inline block is the one constants buffer no group needs to declare")
{
    auto const block = sg::binding{.name = "constants", .space = 9u, .type = sg::binding_type::constants_buffer};
    // dx12 reflects it in the reserved space, whatever it calls it; another target may call it by the binding's name.
    CHECK(mismatch_of(reflecting({{.name = "constants_cb", .space = 9u, .type = sg::binding_type::constants_buffer}}),
                      block)
          == "");
    CHECK(mismatch_of(
              reflecting({{.name = "constants", .group_index = 0u, .type = sg::binding_type::constants_buffer}}), block)
          == "");
    // Without one listed, it is a binding like any other.
    CHECK(mismatch_of(reflecting({{.name = "constants", .space = 9u, .type = sg::binding_type::constants_buffer}})) != "");
}
