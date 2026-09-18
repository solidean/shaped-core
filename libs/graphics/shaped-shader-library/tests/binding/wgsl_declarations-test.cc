#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-shader-library/binding/wgsl_declarations.hh>

using namespace cc::primitive_defines;

namespace
{
sg::binding const* find(slib::wgsl_declarations const& d, cc::string_view name)
{
    for (auto const& b : d.bindings)
        if (b.name == name)
            return &b;
    return nullptr;
}

cc::string error_of(cc::string_view source)
{
    auto const r = slib::parse_wgsl_declarations(source);
    return r.has_error() ? r.error().to_string() : cc::string();
}
} // namespace

TEST("slib wgsl - a compute module reports its entry point, workgroup size and bindings")
{
    auto const r = slib::parse_wgsl_declarations(R"(
        @group(0) @binding(0) var<storage, read_write> values: array<f32>;
        @group(0) @binding(1) var<storage> input: array<u32>;

        @compute @workgroup_size(64)
        fn main(@builtin(global_invocation_id) id: vec3u) { values[id.x] = values[id.x] * 2.0; }
    )");
    REQUIRE(r.has_value());
    auto const& d = r.value();
    CHECK(d.stage == sg::shader_stage::compute);
    CHECK(d.entry_point == "main");
    REQUIRE(d.workgroup_size.has_value());
    CHECK(d.workgroup_size.value().x == 64);
    CHECK(d.workgroup_size.value().y == 1);
    CHECK(d.workgroup_size.value().z == 1);

    auto const* values = find(d, "values");
    REQUIRE(values != nullptr);
    CHECK(values->type == sg::binding_type::readwrite_structured_buffer);
    CHECK(values->group_index == 0u);
    CHECK(values->index == 0u);
    CHECK(!values->space.has_value());
    CHECK(values->visibility.has(sg::shader_stage::compute));

    auto const* input = find(d, "input");
    REQUIRE(input != nullptr);
    CHECK(input->type == sg::binding_type::readonly_structured_buffer); // storage defaults to read
}

TEST("slib wgsl - a workgroup size may name module-scope consts")
{
    auto const r = slib::parse_wgsl_declarations(R"(
        const tile = 8u;
        @compute @workgroup_size(tile, tile, 1) fn main() {}
    )");
    REQUIRE(r.has_value());
    CHECK(r.value().workgroup_size.value().x == 8);
    CHECK(r.value().workgroup_size.value().y == 8);
}

TEST("slib wgsl - a const may be declared below its use, since WGSL module scope has no order")
{
    auto const r = slib::parse_wgsl_declarations(R"(
        struct lights { colors: array<vec4f, light_count> }
        @group(slot) @binding(0) var<uniform> l: lights;
        @compute @workgroup_size(tile, tile) fn main() {}
        const tile = 8u;
        const light_count = 4;
        const slot = 2;
    )");
    REQUIRE(r.has_value());
    REQUIRE(r.value().workgroup_size.has_value());
    CHECK(r.value().workgroup_size.value().x == 8);
    CHECK(r.value().workgroup_size.value().y == 8);
    auto const* l = find(r.value(), "l");
    REQUIRE(l != nullptr);
    CHECK(l->group_index == 2u);
    CHECK(l->block_size == isize(64)); // a fixed-size array, not a runtime-sized one
}

TEST("slib wgsl - an array count naming no const is refused, never read as runtime-sized")
{
    CHECK(error_of(R"(
        struct lights { colors: array<vec4f, light_count> }
        @group(0) @binding(0) var<uniform> l: lights;
        @fragment fn fs() {}
    )")
              .contains("'light_count' is not a module-scope const"));
}

TEST("slib wgsl - integer arguments may be hex literals")
{
    auto const r = slib::parse_wgsl_declarations(R"(
        @group(0x1) @binding(0xAu) var s: sampler;
        @compute @workgroup_size(0x10) fn main() {}
    )");
    REQUIRE(r.has_value());
    CHECK(find(r.value(), "s")->group_index == 1u);
    CHECK(find(r.value(), "s")->index == 10u);
    CHECK(r.value().workgroup_size.value().x == 16);
}

TEST("slib wgsl - a workgroup size sg would have to evaluate is refused as an expression")
{
    CHECK(error_of(R"(
        const a = 1;
        const b = 2;
        @compute @workgroup_size(select(1, 2, a < b)) fn main() {}
    )")
              .contains("an expression sg does not evaluate"));
}

TEST("slib wgsl - an override-sized workgroup is refused by name")
{
    auto const e = error_of(R"(
        override tile: u32 = 8;
        @compute @workgroup_size(tile) fn main() {}
    )");
    CHECK(e.contains("'tile' is an override"));
}

TEST("slib wgsl - one entry point per module, refused otherwise")
{
    CHECK(error_of("fn helper() {}").contains("declares no entry point"));
    auto const e = error_of(R"(
        @vertex fn vs() -> @builtin(position) vec4f { return vec4f(); }
        @fragment fn fs() -> @location(0) vec4f { return vec4f(); }
    )");
    CHECK(e.contains("2 entry points"));
    CHECK(e.contains("'vs'"));
}

TEST("slib wgsl - function bodies are skipped however they nest")
{
    auto const r = slib::parse_wgsl_declarations(R"(
        fn helper(x: f32) -> f32 { if (x > 0.0) { return x; } else { return -x; } }
        @fragment fn fs() -> @location(0) vec4f { let s = struct_like { a: 1 }; return vec4f(helper(1.0)); }
        @group(1) @binding(2) var tex: texture_2d<f32>;
    )");
    REQUIRE(r.has_value());
    CHECK(r.value().stage == sg::shader_stage::fragment);
    REQUIRE(find(r.value(), "tex") != nullptr);
    CHECK(find(r.value(), "tex")->group_index == 1u);
}

TEST("slib wgsl - comments are skipped, block comments nesting")
{
    auto const r = slib::parse_wgsl_declarations(R"(
        // @group(0) @binding(0) var<uniform> commented: f32;
        /* outer /* inner @group(0) @binding(1) var hidden: sampler; */ still comment */
        @group(0) @binding(2) var visible: sampler;
        @fragment fn fs() {}
    )");
    REQUIRE(r.has_value());
    CHECK(r.value().bindings.size() == 1);
    CHECK(find(r.value(), "visible") != nullptr);
}

TEST("slib wgsl - sampled textures report dimension and sample type")
{
    auto const r = slib::parse_wgsl_declarations(R"(
        @group(0) @binding(0) var a: texture_2d<f32>;
        @group(0) @binding(1) var b: texture_2d_array<i32>;
        @group(0) @binding(2) var c: texture_cube<u32>;
        @group(0) @binding(3) var d: texture_depth_2d;
        @group(0) @binding(4) var e: texture_3d<f32>;
        @group(0) @binding(5) var f: texture_multisampled_2d<f32>;
        @group(0) @binding(6) var g: texture_cube_array<f32>;
        @fragment fn fs() {}
    )");
    REQUIRE(r.has_value());
    auto const& d = r.value();
    CHECK(find(d, "a")->type == sg::binding_type::readonly_texture);
    CHECK(find(d, "a")->texture_dimension == sg::texture_view_dimension::tex_2d);
    CHECK(find(d, "a")->sample_type == sg::texture_sample_type::filterable_float);
    CHECK(find(d, "b")->texture_dimension == sg::texture_view_dimension::tex_2d_array);
    CHECK(find(d, "b")->sample_type == sg::texture_sample_type::sint);
    CHECK(find(d, "c")->texture_dimension == sg::texture_view_dimension::cube);
    CHECK(find(d, "c")->sample_type == sg::texture_sample_type::uint);
    CHECK(find(d, "d")->sample_type == sg::texture_sample_type::depth);
    CHECK(find(d, "e")->texture_dimension == sg::texture_view_dimension::tex_3d);
    CHECK(find(d, "f")->texture_dimension == sg::texture_view_dimension::tex_2d_ms);
    CHECK(find(d, "f")->sample_type == sg::texture_sample_type::unfilterable_float); // WebGPU never filters one
    CHECK(find(d, "g")->texture_dimension == sg::texture_view_dimension::cube_array);
}

TEST("slib wgsl - a storage texture reports the format WGSL declares, which HLSL cannot")
{
    auto const r = slib::parse_wgsl_declarations(R"(
        @group(0) @binding(0) var out_color: texture_storage_2d<rgba16float, write>;
        @group(0) @binding(1) var out_mask: texture_storage_3d<r32uint, read_write>;
        @compute @workgroup_size(8, 8) fn main() {}
    )");
    REQUIRE(r.has_value());
    auto const* color = find(r.value(), "out_color");
    REQUIRE(color != nullptr);
    CHECK(color->type == sg::binding_type::readwrite_texture);
    CHECK(color->storage_format == sg::pixel_format::rgba16_float);
    CHECK(color->texture_dimension == sg::texture_view_dimension::tex_2d);
    CHECK(find(r.value(), "out_mask")->storage_format == sg::pixel_format::r32_uint);
    CHECK(find(r.value(), "out_mask")->texture_dimension == sg::texture_view_dimension::tex_3d);

    // The access mode is part of what a WebGPU layout entry must match, so `write` must not come back as read-write.
    CHECK(color->storage_access == sg::storage_access::write);
    CHECK(find(r.value(), "out_mask")->storage_access == sg::storage_access::read_write);
}

TEST("slib wgsl - a read-only storage texture reports read access")
{
    auto const r = slib::parse_wgsl_declarations(R"(
        @group(0) @binding(0) var src: texture_storage_2d<r32float, read>;
        @compute @workgroup_size(8, 8) fn main() {}
    )");
    REQUIRE(r.has_value());
    REQUIRE(find(r.value(), "src") != nullptr);
    CHECK(find(r.value(), "src")->type == sg::binding_type::readwrite_texture);
    CHECK(find(r.value(), "src")->storage_access == sg::storage_access::read);
}

TEST("slib wgsl - a storage texture format sg has no pixel format for is refused by name")
{
    CHECK(error_of(R"(
        @group(0) @binding(0) var t: texture_storage_2d<r16unorm, write>;
        @compute @workgroup_size(1) fn main() {}
    )")
              .contains("'r16unorm'"));
}

TEST("slib wgsl - samplers report their kind")
{
    auto const r = slib::parse_wgsl_declarations(R"(
        @group(0) @binding(0) var linear: sampler;
        @group(0) @binding(1) var shadow: sampler_comparison;
        @fragment fn fs() {}
    )");
    REQUIRE(r.has_value());
    CHECK(find(r.value(), "linear")->type == sg::binding_type::sampler);
    CHECK(find(r.value(), "linear")->sampler_type == sg::sampler_binding_type::filtering);
    CHECK(find(r.value(), "shadow")->sampler_type == sg::sampler_binding_type::comparison);
}

TEST("slib wgsl - a uniform's block size follows WGSL's layout rules")
{
    auto const r = slib::parse_wgsl_declarations(R"(
        struct light { position: vec3f, intensity: f32, color: vec3f }
        struct frame { view: mat4x4f, lights: array<light, 2>, count: u32 }
        @group(0) @binding(0) var<uniform> f: frame;
        @group(0) @binding(1) var<uniform> scalar: f32;
        @group(0) @binding(2) var<uniform> v3: vec3<f32>;
        @vertex fn vs() -> @builtin(position) vec4f { return vec4f(); }
    )");
    REQUIRE(r.has_value());
    // light: vec3f at 0 (align 16), f32 at 12, vec3f at 16 -> 28, rounded to align 16 = 32.
    // frame: mat4x4f 64, array<light, 2> at 64 -> 128, u32 at 128 -> 132, rounded to 16 = 144.
    CHECK(find(r.value(), "f")->block_size == isize(144));
    CHECK(find(r.value(), "scalar")->block_size == isize(4));
    CHECK(find(r.value(), "v3")->block_size == isize(12));
    CHECK(find(r.value(), "f")->type == sg::binding_type::uniform_buffer);
}

TEST("slib wgsl - explicit @size and @align are honored")
{
    auto const r = slib::parse_wgsl_declarations(R"(
        struct padded { @size(16) a: f32, @align(32) b: f32 }
        @group(0) @binding(0) var<uniform> p: padded;
        @fragment fn fs() {}
    )");
    REQUIRE(r.has_value());
    // a occupies 16 bytes, b starts at 32 and ends at 36, rounded to the struct's align 32 = 64.
    CHECK(find(r.value(), "p")->block_size == isize(64));
}

TEST("slib wgsl - aliases resolve before layout and binding kind")
{
    auto const r = slib::parse_wgsl_declarations(R"(
        alias color = vec4f;
        alias image = texture_2d<f32>;
        @group(0) @binding(0) var<uniform> tint: color;
        @group(0) @binding(1) var source: image;
        @fragment fn fs() {}
    )");
    REQUIRE(r.has_value());
    CHECK(find(r.value(), "tint")->block_size == isize(16));
    CHECK(find(r.value(), "source")->type == sg::binding_type::readonly_texture);
}

TEST("slib wgsl - the reserved group's binding 0 is the inline-constants block")
{
    auto const r = slib::parse_wgsl_declarations(R"(
        struct draw { transform: mat4x4f, tint: vec4f }
        @group(3) @binding(0) var<uniform> constants: draw;
        @vertex fn vs() -> @builtin(position) vec4f { return vec4f(); }
    )");
    REQUIRE(r.has_value());
    auto const* c = find(r.value(), "constants");
    REQUIRE(c != nullptr);
    CHECK(c->type == sg::binding_type::uniform_buffer);
    CHECK(!c->group_index.has_value()); // reported the way SPIR-V reports a push-constant block
    CHECK(c->block_size == isize(80));
}

TEST("slib wgsl - a register-bound static sampler in the reserved group is numbered one below its binding")
{
    auto const r = slib::parse_wgsl_declarations(R"(
        @group(3) @binding(1) var clamp_linear: sampler;
        @group(3) @binding(4) var shadow: sampler_comparison;
        @fragment fn fs() {}
    )");
    REQUIRE(r.has_value());
    CHECK(find(r.value(), "clamp_linear")->group_index == 3u);
    CHECK(find(r.value(), "clamp_linear")->index == 0u);
    CHECK(find(r.value(), "shadow")->index == 3u);
}

TEST("slib wgsl - anything else in the reserved group is refused")
{
    CHECK(error_of(R"(
        @group(3) @binding(1) var t: texture_2d<f32>;
        @fragment fn fs() {}
    )")
              .contains("reserved group"));
    CHECK(error_of(R"(
        @group(3) @binding(0) var s: sampler;
        @fragment fn fs() {}
    )")
              .contains("inline-constants block"));
}

TEST("slib wgsl - binding arrays and external textures are refused by name")
{
    CHECK(error_of(R"(
        @group(0) @binding(0) var textures: binding_array<texture_2d<f32>, 16>;
        @fragment fn fs() {}
    )")
              .contains("binding array"));
    CHECK(error_of(R"(
        @group(0) @binding(0) var video: texture_external;
        @fragment fn fs() {}
    )")
              .contains("texture_external"));
}

TEST("slib wgsl - module-private vars and enable directives are not bindings")
{
    auto const r = slib::parse_wgsl_declarations(R"(
        enable f16;
        var<private> scratch: f32;
        const scale = 2.0;
        @group(0) @binding(0) var<uniform> gain: f32;
        @fragment fn fs() {}
    )");
    REQUIRE(r.has_value());
    CHECK(r.value().bindings.size() == 1);
}

TEST("slib wgsl - a var missing half its address is refused")
{
    CHECK(error_of(R"(
        @binding(0) var<uniform> half: f32;
        @fragment fn fs() {}
    )")
              .contains("needs both @group and @binding"));
}

TEST("slib wgsl - every binding is visible to the module's stage and no other")
{
    auto const r = slib::parse_wgsl_declarations(R"(
        @group(0) @binding(0) var<storage> data: array<f32>;
        @vertex fn vs() -> @builtin(position) vec4f { return vec4f(); }
    )");
    REQUIRE(r.has_value());
    auto const* data = find(r.value(), "data");
    CHECK(data->visibility.has(sg::shader_stage::vertex));
    CHECK(!data->visibility.has(sg::shader_stage::fragment));
    CHECK(!data->visibility.has(sg::shader_stage::compute));
}
