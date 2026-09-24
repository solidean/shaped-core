#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>
#include <shaped-shader-compiler-msl/impl/msl_reflection.hh>

// Reflection reads the MSL text, which is the only source that works for every stage on every host.
// These run anywhere: nothing here needs a Metal toolchain or a GPU.

namespace
{
using namespace cc::primitive_defines;

/// An argument buffer in the shape the metal backend encodes — group at [[buffer(N)]], member at [[id(n)]].
constexpr char const* k_grouped = R"(
#include <metal_stdlib>
using namespace metal;

struct frame_bindings
{
    texture2d<float>       albedo   [[id(0)]];
    sampler                linear   [[id(1)]];
    device float4*         lights   [[id(2)]];
    device float4 const*   readonly [[id(3)]];
    constant float4&       tint     [[id(4)]];
};

kernel void shade(constant frame_bindings& f [[buffer(0)]], uint i [[thread_position_in_grid]])
{
    (void)f; (void)i;
}
)";

[[nodiscard]] sg::binding const* binding_named(cc::span<sg::binding const> bindings, cc::string_view name)
{
    for (auto const& b : bindings)
    {
        if (cc::string_view(b.name) == name)
            return &b;
    }
    return nullptr;
}
} // namespace

TEST("ssc::msl reflect - an argument buffer becomes one group, member ids intact")
{
    auto r = ssc::msl::impl::reflect(k_grouped, "shade", sg::shader_stage::compute);
    REQUIRE(r.has_value());

    auto const& bindings = r.value().bindings;
    REQUIRE(bindings.size() == 5);

    // The group is the argument table's buffer index, which is what sg calls group_index.
    for (auto const& b : bindings)
    {
        REQUIRE(b.group_index.has_value());
        CHECK(b.group_index.value() == 0);
        CHECK(!b.space.has_value()); // MSL has no register spaces at all
        CHECK(b.visibility.has(sg::shader_stage::compute));
    }

    auto const* albedo = binding_named(bindings, "albedo");
    REQUIRE(albedo != nullptr);
    CHECK(albedo->index == 0);
    CHECK(albedo->type == sg::binding_type::readonly_texture);
    REQUIRE(albedo->texture_dimension.has_value());
    CHECK(albedo->texture_dimension.value() == sg::texture_view_dimension::tex_2d);

    auto const* linear = binding_named(bindings, "linear");
    REQUIRE(linear != nullptr);
    CHECK(linear->index == 1);
    CHECK(linear->type == sg::binding_type::sampler);

    // Every `device T*` is structured, and const is what makes it readonly — see msl_reflection.hh.
    auto const* lights = binding_named(bindings, "lights");
    REQUIRE(lights != nullptr);
    CHECK(lights->index == 2);
    CHECK(lights->type == sg::binding_type::readwrite_structured_buffer);

    auto const* readonly = binding_named(bindings, "readonly");
    REQUIRE(readonly != nullptr);
    CHECK(readonly->type == sg::binding_type::readonly_structured_buffer);

    auto const* tint = binding_named(bindings, "tint");
    REQUIRE(tint != nullptr);
    CHECK(tint->type == sg::binding_type::uniform_buffer);
}

TEST("ssc::msl reflect - a declared-but-unreferenced binding is still reported")
{
    // The defect DXIL reflection has and a parser does not: the shader declares it, so the layout carries it.
    auto r = ssc::msl::impl::reflect(k_grouped, "shade", sg::shader_stage::compute);
    REQUIRE(r.has_value());
    CHECK(binding_named(r.value().bindings, "readonly") != nullptr);
}

TEST("ssc::msl reflect - an array member takes count consecutive indices")
{
    constexpr char const* source = R"(
struct bindless
{
    texture2d<float> textures[8] [[id(0)]];
    sampler          s           [[id(8)]];
};
kernel void sample_many(constant bindless& b [[buffer(1)]]) { (void)b; }
)";

    auto r = ssc::msl::impl::reflect(source, "sample_many", sg::shader_stage::compute);
    REQUIRE(r.has_value());
    REQUIRE(r.value().bindings.size() == 2);

    auto const* textures = binding_named(r.value().bindings, "textures");
    REQUIRE(textures != nullptr);
    CHECK(textures->count == 8);
    CHECK(textures->is_array());
    CHECK(textures->index == 0);
    CHECK(textures->group_index.value() == 1);

    // The layout rule the metal backend states: an array occupies count slots, so the next index is 8.
    auto const* s = binding_named(r.value().bindings, "s");
    REQUIRE(s != nullptr);
    CHECK(s->index == 8);
}

TEST("ssc::msl reflect - a storage texture is readwrite, and says how it is accessed")
{
    constexpr char const* source = R"(
struct outputs { texture2d<float, access::read_write> target [[id(0)]]; };
kernel void blur(constant outputs& o [[buffer(0)]]) { (void)o; }
)";

    auto r = ssc::msl::impl::reflect(source, "blur", sg::shader_stage::compute);
    REQUIRE(r.has_value());
    REQUIRE(r.value().bindings.size() == 1);
    CHECK(r.value().bindings[0].type == sg::binding_type::readwrite_texture);
    CHECK(r.value().bindings[0].storage_access == sg::storage_access::read_write);
}

TEST("ssc::msl reflect - a resource bound straight on the entry point is refused, since the backend binds none")
{
    // The metal backend sets group N's argument buffer at [[buffer(N)]] and never a texture or a sampler slot.
    char const* const sources[] = {
        "kernel void k(device float* data [[buffer(0)]]) { (void)data; }",
        "kernel void k(texture2d<float> tex [[texture(0)]]) { (void)tex; }",
        "kernel void k(sampler s [[sampler(0)]]) { (void)s; }",
    };

    for (auto const* const source : sources)
    {
        auto r = ssc::msl::impl::reflect(source, "k", sg::shader_stage::compute);
        REQUIRE(r.has_error());
        CHECK(r.error().to_string().contains("argument buffers only")).context(r.error().to_string());
    }
}

TEST("ssc::msl reflect - a built-in parameter is a value the hardware supplies, never a binding")
{
    constexpr char const* source = R"(
struct work { device float* data [[id(0)]]; };
kernel void k(constant work& w [[buffer(0)]], uint i [[thread_position_in_grid]]) { (void)w; (void)i; }
)";

    auto r = ssc::msl::impl::reflect(source, "k", sg::shader_stage::compute);
    REQUIRE(r.has_value());
    REQUIRE(r.value().bindings.size() == 1);
    CHECK(binding_named(r.value().bindings, "i") == nullptr);
}

TEST("ssc::msl reflect - an argument buffer past the reserved group names no group, and is refused")
{
    constexpr char const* source = R"(
struct work { device float* data [[id(0)]]; };
kernel void k(constant work& w [[buffer(4)]]) { (void)w; }
)";

    CHECK(ssc::msl::impl::reflect(source, "k", sg::shader_stage::compute).has_error());
}

TEST("ssc::msl reflect - the inline-constants block has no group and no space, whatever it is called")
{
    // SGL's cube vertex shader, as `sgl emit --target msl` writes it: the test binary links no sgl, so it is text here.
    constexpr char const* source = R"(
#include <metal_stdlib>
using namespace metal;

struct cube_vertex
{
    float3 position [[attribute(0)]];
    float3 color [[attribute(1)]];
};

struct pixel_input
{
    float4 position [[position]];
    float3 color [[user(sgl0)]];
};

struct constants_data
{
    float4x4 view_projection;
};

vertex pixel_input main_vs(cube_vertex v [[stage_in]], constant constants_data& constants [[buffer(4)]])
{
    pixel_input result;
    result.position = constants.view_projection * float4(v.position, 1.0);
    result.color = v.color;
    return result;
}
)";

    auto r = ssc::msl::impl::reflect(source, "main_vs", sg::shader_stage::vertex);
    REQUIRE(r.has_value()).context(r.has_error() ? r.error().to_string() : cc::string());
    REQUIRE(r.value().bindings.size() == 1);

    // No group and no space is what sg's layout fit recognizes as the inline block, by that clause and not its name.
    auto const& constants = r.value().bindings[0];
    CHECK(constants.name == "constants");
    CHECK(constants.type == sg::binding_type::uniform_buffer);
    CHECK(!constants.group_index.has_value());
    CHECK(!constants.space.has_value());
}

TEST("ssc::msl reflect - only a constant block may sit at the inline-constants index")
{
    constexpr char const* source = "kernel void k(device float* data [[buffer(4)]]) { (void)data; }";
    CHECK(ssc::msl::impl::reflect(source, "k", sg::shader_stage::compute).has_error());
}

TEST("ssc::msl reflect - the threadgroup shape comes from the pragma, since MSL states none")
{
    constexpr char const* source = R"(
struct work { device float* d [[id(0)]]; };
#pragma sc numthreads 64 2 1
kernel void reduce(constant work& w [[buffer(0)]]) { (void)w; }
)";

    auto r = ssc::msl::impl::reflect(source, "reduce", sg::shader_stage::compute);
    REQUIRE(r.has_value());
    REQUIRE(r.value().workgroup_size.has_value());
    CHECK(r.value().workgroup_size.value().x == 64);
    CHECK(r.value().workgroup_size.value().y == 2);
    CHECK(r.value().workgroup_size.value().z == 1);
}

TEST("ssc::msl reflect - a kernel that states no shape leaves it absent rather than guessing 1x1x1")
{
    constexpr char const* source = R"(
struct work { device float* d [[id(0)]]; };
kernel void flat(constant work& w [[buffer(0)]]) { (void)w; }
)";

    auto r = ssc::msl::impl::reflect(source, "flat", sg::shader_stage::compute);
    REQUIRE(r.has_value());
    CHECK(!r.value().workgroup_size.has_value());
}

TEST("ssc::msl reflect - each kernel of a file takes its own pragma, never the one above another kernel")
{
    constexpr char const* source = R"(
struct work { device float* d [[id(0)]]; };

#pragma sc numthreads 64 1 1
kernel void first(constant work& w [[buffer(0)]]) { (void)w; }

kernel void second(constant work& w [[buffer(0)]]) { (void)w; }

#pragma sc numthreads 8 8 1
kernel void third(constant work& w [[buffer(0)]]) { (void)w; }
)";

    auto const first = ssc::msl::impl::reflect(source, "first", sg::shader_stage::compute);
    REQUIRE(first.has_value());
    REQUIRE(first.value().workgroup_size.has_value());
    CHECK(first.value().workgroup_size.value().x == 64);

    // `second` states none, so it has none: the pragma above `first` is not its own.
    auto const second = ssc::msl::impl::reflect(source, "second", sg::shader_stage::compute);
    REQUIRE(second.has_value());
    CHECK(!second.value().workgroup_size.has_value());

    auto const third = ssc::msl::impl::reflect(source, "third", sg::shader_stage::compute);
    REQUIRE(third.has_value());
    REQUIRE(third.value().workgroup_size.has_value());
    CHECK(third.value().workgroup_size.value().x == 8);
    CHECK(third.value().workgroup_size.value().y == 8);
}

TEST("ssc::msl reflect - the shape needs `#pragma sc`, and a bare `numthreads` elsewhere is no pragma")
{
    constexpr char const* source = R"(
struct work { device float* numthreads [[id(0)]]; };
#pragma numthreads 64 1 1
kernel void k(constant work& w [[buffer(0)]]) { (void)w; }
)";

    auto r = ssc::msl::impl::reflect(source, "k", sg::shader_stage::compute);
    REQUIRE(r.has_value());
    CHECK(!r.value().workgroup_size.has_value());
}

TEST("ssc::msl reflect - an entry point the text does not declare is an error")
{
    auto r = ssc::msl::impl::reflect(k_grouped, "not_here", sg::shader_stage::compute);
    CHECK(r.has_error());
}

TEST("ssc::msl reflect - a stage whose qualifier does not match is an error")
{
    // `shade` is a kernel, so asking for it as a vertex stage is a mistake worth catching before pipeline creation.
    auto r = ssc::msl::impl::reflect(k_grouped, "shade", sg::shader_stage::vertex);
    CHECK(r.has_error());
}

TEST("ssc::msl reflect - the stages metal does not have are refused by name")
{
    CHECK(ssc::msl::impl::entry_qualifier_for(sg::shader_stage::geometry).has_error());
    CHECK(ssc::msl::impl::entry_qualifier_for(sg::shader_stage::tessellation_control).has_error());

    // raygen is a kernel here, because Metal schedules no raygen of its own.
    auto const raygen = ssc::msl::impl::entry_qualifier_for(sg::shader_stage::raygen);
    REQUIRE(raygen.has_value());
    CHECK(raygen.value() == "kernel");

    // The other five ray-tracing stages are visible functions, which is what a shader table links.
    auto const miss = ssc::msl::impl::entry_qualifier_for(sg::shader_stage::miss);
    REQUIRE(miss.has_value());
    CHECK(miss.value() == "visible");
}

TEST("ssc::msl reflect - comments never contribute a binding")
{
    constexpr char const* source = R"(
struct real { device float* used [[id(0)]]; };
// struct fake { device float* commented_out [[id(1)]]; };
/* struct also_fake { device float* block_commented [[id(2)]]; }; */
kernel void go(constant real& r [[buffer(0)]]) { (void)r; }
)";

    auto r = ssc::msl::impl::reflect(source, "go", sg::shader_stage::compute);
    REQUIRE(r.has_value());
    CHECK(r.value().bindings.size() == 1);
    CHECK(binding_named(r.value().bindings, "used") != nullptr);
}
