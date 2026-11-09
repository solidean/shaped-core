#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <nexus/source_root.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-shader-compiler-dxc/compiler.hh>

#include <filesystem>

using namespace cc::primitive_defines;

// SPV_EXT_shader_invocation_reorder has a hit object live in a variable its instructions fill by pointer, and forbids
// reading or writing that variable as a value.
// Neither spirv-val nor the validation layer says so, and a driver may crash on it, so this reads DXC's module itself.
// It walks the words rather than asking spirv-dis, so nothing here depends on a tool being on the path.

namespace
{
namespace fs = std::filesystem;

constexpr u32 k_op_undef = 1;
constexpr u32 k_op_type_array = 28;
constexpr u32 k_op_type_runtime_array = 29;
constexpr u32 k_op_type_struct = 30;
constexpr u32 k_op_type_pointer = 32;
constexpr u32 k_op_function_parameter = 55;
constexpr u32 k_op_variable = 59;
constexpr u32 k_op_load = 61;
constexpr u32 k_op_store = 62;
constexpr u32 k_op_copy_memory = 63;
constexpr u32 k_op_copy_memory_sized = 64;
constexpr u32 k_op_access_chain = 65;
constexpr u32 k_op_in_bounds_access_chain = 66;
constexpr u32 k_op_composite_construct = 80;
constexpr u32 k_op_composite_extract = 81;
constexpr u32 k_op_copy_object = 83;
constexpr u32 k_op_select = 169;
constexpr u32 k_op_phi = 245;
constexpr u32 k_op_type_hit_object = 5313;

struct hit_object_walk
{
    /// How many pointer types reach a hit object, which every module that holds a variable of one has.
    i32 pointer_types = 0;
    i32 variables = 0;
    /// Each instruction that moves a hit object as a value, by its name.
    cc::vector<cc::string> copies;
};

[[nodiscard]] hit_object_walk walk_hit_objects(cc::span<u32 const> module)
{
    auto result = hit_object_walk();
    REQUIRE(module.size() >= 5);
    // the header: magic, version, generator, bound, schema, and every id is below the bound
    auto const bound = isize(module[3]);
    auto is_hit_type = cc::vector<bool>::create_filled(bound, false);
    auto is_hit_pointer = cc::vector<bool>::create_filled(bound, false);
    auto is_hit_place = cc::vector<bool>::create_filled(bound, false);
    auto const has = [](cc::vector<bool> const& set, u32 id) { return id < u32(set.size()) && set[id]; };
    auto const add = [](cc::vector<bool>& set, u32 id) { set[id] = true; };
    for (auto at = isize(5); at < module.size();)
    {
        auto const count = isize(module[at] >> 16);
        auto const op = module[at] & 0xffffu;
        if (count <= 0 || at + count > module.size())
        {
            result.copies.push_back(cc::string("a module the walk cannot read"));
            break;
        }
        auto const word = [&](isize i) { return i < count ? module[at + i] : 0xffffffffu; };
        switch (op)
        {
        case k_op_type_hit_object:
            add(is_hit_type, word(1));
            break;
        case k_op_type_array:
        case k_op_type_runtime_array:
        case k_op_type_struct:
            for (auto i = isize(2); i < count; ++i)
                if (has(is_hit_type, word(i)))
                {
                    result.copies.push_back(cc::string("an array or a struct of a hit object"));
                    add(is_hit_type, word(1));
                }
            break;
        case k_op_type_pointer:
            if (has(is_hit_type, word(3)))
            {
                add(is_hit_pointer, word(1));
                ++result.pointer_types;
            }
            break;
        case k_op_variable:
            if (has(is_hit_pointer, word(1)))
            {
                add(is_hit_place, word(2));
                ++result.variables;
            }
            break;
        case k_op_access_chain:
        case k_op_in_bounds_access_chain:
            if (has(is_hit_pointer, word(1)))
                add(is_hit_place, word(2));
            break;
        case k_op_load:
            if (has(is_hit_type, word(1)) || has(is_hit_place, word(3)))
                result.copies.push_back(cc::string("OpLoad"));
            break;
        case k_op_store:
            if (has(is_hit_place, word(1)))
                result.copies.push_back(cc::string("OpStore"));
            break;
        case k_op_copy_memory:
        case k_op_copy_memory_sized:
            if (has(is_hit_place, word(1)) || has(is_hit_place, word(2)))
                result.copies.push_back(cc::string("OpCopyMemory"));
            break;
        case k_op_copy_object:
        case k_op_composite_construct:
        case k_op_composite_extract:
        case k_op_select:
        case k_op_phi:
        case k_op_undef:
        case k_op_function_parameter:
            if (has(is_hit_type, word(1)) || has(is_hit_pointer, word(1)))
                result.copies.push_back(cc::format("a value of opcode {}", op));
            break;
        default:
            break;
        }
        at += count;
    }
    return result;
}

[[nodiscard]] cc::vector<u32> words_of(sg::compiled_shader const& shader)
{
    auto words = cc::vector<u32>();
    auto const bytes = cc::span<byte const>(shader.bytecode.data(), shader.bytecode.size());
    for (auto i = isize(0); i + 3 < bytes.size(); i += 4)
        words.push_back(u32(bytes[i]) | u32(bytes[i + 1]) << 8 | u32(bytes[i + 2]) << 16 | u32(bytes[i + 3]) << 24);
    return words;
}

[[nodiscard]] cc::result<sg::compiled_shader> built(ssc::dxc::compiler& compiler,
                                                    sgl::emitted_source const& text,
                                                    ssc::dxc::optimization_level level)
{
    return compiler.compile({.source = text.text, .entry_point = text.entry_point, .stage = sg::shader_stage::raygen},
                            {.target = ssc::dxc::compile_target::spirv,
                             .optimization = level,
                             .warnings_as_errors = false,
                             .extra_args = {cc::string("-enable-16bit-types")}});
}

[[nodiscard]] sgl::check::feature_set reordering_device()
{
    auto device = sgl::check::feature_set();
    device.set(sgl::check::feature::shader_execution_reordering);
    return device;
}

[[nodiscard]] cc::string read_file(cc::string_view path)
{
    auto adapter = cc::file_read_stream_adapter::open(path);
    REQUIRE(adapter.has_value());
    auto stream = adapter.value().stream();
    auto bytes = stream.read_all();
    REQUIRE(bytes.has_value());
    return cc::string(cc::string_view(reinterpret_cast<char const*>(bytes.value().data()), bytes.value().size()));
}

/// A raygen around `body`, whose lines stand under the reordering device's `const if`, after `declarations`.
[[nodiscard]] cc::string raygen(cc::string_view declarations, cc::string_view body)
{
    auto source = cc::string("require raytracing_pipeline\n\n"
                             "struct radiance:\n    color: float\n\n"
                             "rays rs:\n    primary: radiance\n\n"
                             "binding work:\n"
                             "    world: acceleration_structure[.triangles]\n"
                             "    values: mut buffer[float]\n\n");
    source += declarations;
    source += "\n@raygen fun main_cs(@launch_id id: int3){work}:\n"
              "    let mut p = radiance(0.0)\n"
              "    let r = ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, 0.0, 1.0))\n"
              "    const if has(.shader_execution_reordering):\n";
    auto is_line_start = true;
    for (auto const ch : body)
    {
        if (is_line_start)
            source += "        ";
        source += ch;
        is_line_start = ch == '\n';
    }
    source += "    work.values[id.x] = p.color\n";
    return source;
}

constexpr cc::string_view k_trace = "trace_hit(work.world, r, rs.primary, mut p)";
} // namespace

// EMIT-246: the native route's own module, which no DXC stands behind.
// Each shape is one that would tempt a pass: two traces in two arms, a read in a branch, one trace in a loop.
TEST("sgl hit object spirv - SGL's own binaries hold each hit object in its one variable or value")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    constexpr cc::string_view bodies[] = {
        "let h = trace_hit(work.world, r, rs.primary, mut p)\nif h.is_hit => h.invoke(mut p)\n",
        "if id.x > 3:\n    let h = trace_hit(work.world, r, rs.primary, mut p)\n    h.reorder()\n    h.invoke(mut p)\n"
        "else:\n    let h = trace_hit(work.world, r, rs.primary, mut p)\n    h.reorder(id.x as uint, 2)\n    "
        "h.invoke(mut p)\n",
        "let h = trace_hit(work.world, r, rs.primary, mut p)\nlet mut t = 0.0\nif h.is_hit => t = h.t\nh.invoke(mut "
        "p)\n",
        "let mut i = 0\nwhile i < 2:\n    let h = trace_hit(work.world, r, rs.primary, mut p)\n    h.invoke(mut p)\n   "
        " i = i + 1\n",
    };
    for (auto const body : bodies)
    {
        auto const source = raygen("", body);
        auto const on_vulkan = sgl::compile_to_text({.source = source,
                                                     .entry_point = "main_cs",
                                                     .target = sgl::emit::target::hlsl_vulkan,
                                                     .device_features = reordering_device(),
                                                     .native = sgl::emit::native_route::require});
        REQUIRE(on_vulkan.has_value()).dump("error", on_vulkan.has_error() ? cc::string(on_vulkan.error()) : cc::string());
        auto const walk = walk_hit_objects(on_vulkan.value().spirv);
        CHECK(walk.copies.empty()).dump("body", body).dump("first", walk.copies.empty() ? cc::string() : walk.copies[0]);
        CHECK(walk.variables >= 1);

        auto const on_dx12 = sgl::compile_to_text({.source = source,
                                                   .entry_point = "main_cs",
                                                   .target = sgl::emit::target::hlsl_dx12,
                                                   .device_features = reordering_device(),
                                                   .native = sgl::emit::native_route::require});
        REQUIRE(on_dx12.has_value());
#ifdef CC_OS_WINDOWS
        auto const refused = compiler.value().validate(on_dx12.value().dxil);
        CHECK(!refused.has_value()).dump("validator", refused.value_or(cc::string()));
        auto const text = compiler.value().disassemble(on_dx12.value().dxil);
        REQUIRE(text.has_value());
        CHECK(text.value().contains("@dx.op.hitObject_TraceRay"));
        CHECK(!text.value().contains("phi %dx.types.HitObject"));
        auto selects_one = false;
        for (auto rest = cc::string_view(text.value()); !rest.empty();)
        {
            auto const end = rest.find('\n');
            auto const line = end < 0 ? rest : rest.subview({.offset = 0, .size = end});
            selects_one = selects_one || (line.contains(" select ") && line.contains("HitObject"));
            rest = end < 0 ? cc::string_view() : rest.subview({.offset = end + 1, .size = rest.size() - end - 1});
        }
        CHECK(!selects_one);
#endif
    }
}

TEST("sgl hit object spirv - the variable a trace fills is a pointer, and a copy of it is what the walk finds")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    auto const source = raygen("", cc::format("let h = {}\nif h.is_hit => h.invoke(mut p)\n", k_trace));
    auto emitted = sgl::compile_to_text({.source = source,
                                         .entry_point = "main_cs",
                                         .target = sgl::emit::target::hlsl_vulkan,
                                         .device_features = reordering_device()});
    REQUIRE(emitted.has_value()).dump("error", emitted.has_error() ? cc::string(emitted.error()) : cc::string());

    auto const clean = built(compiler.value(), emitted.value(), ssc::dxc::optimization_level::level_3);
    REQUIRE(clean.has_value());
    auto const clean_walk = walk_hit_objects(words_of(clean.value()));
    // a pointer to a hit object is how SPIR-V's instructions take one, so it is no finding: only moving the value is
    CHECK(clean_walk.pointer_types == 1);
    CHECK(clean_walk.variables == 1);
    CHECK(clean_walk.copies.empty());

    // the copy `let h` wrote before EMIT-225 forbade it, which spirv-val and the validation layer both accept
    auto const text = cc::string_view(emitted.value().text);
    constexpr auto trace_ends = cc::string_view("p = sgl_hit_payload_radiance;\n");
    constexpr auto is_hit = cc::string_view("sgl_hit_object_is_hit(hit_object_1)");
    auto const after_trace = text.find(trace_ends) + trace_ends.size();
    auto const at = text.find(is_hit);
    REQUIRE(after_trace > 0);
    REQUIRE(at >= after_trace);
    auto copied = cc::string(text.subview({.offset = 0, .size = after_trace}));
    copied += "    const vk::SpirvOpaqueType<5313> h = hit_object_1;\n";
    copied += text.subview({.offset = after_trace, .size = at - after_trace});
    copied += "sgl_hit_object_is_hit(h)";
    copied += text.subview(at + is_hit.size());
    auto with_copy = cc::move(emitted.value());
    with_copy.text = cc::move(copied);
    auto const bad = built(compiler.value(), with_copy, ssc::dxc::optimization_level::level_3);
    REQUIRE(bad.has_value()).dump("error", bad.has_error() ? cc::format("{}", bad.error()) : cc::string());
    auto const walk = walk_hit_objects(words_of(bad.value()));
    CHECK(walk.copies.values().any([](cc::string const& c) { return c == "OpLoad"; }));
    CHECK(walk.copies.values().any([](cc::string const& c) { return c == "OpStore"; }));
}

TEST("sgl hit object spirv - every path a hit object may take leaves DXC's module with no copy, at every level")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    struct path
    {
        cc::string_view name;
        cc::string_view declarations;
        cc::string_view body;
    };
    constexpr auto reader = cc::string_view("fun read(x: hit_object) -> float:\n    return x.t\n");
    auto const traced = cc::format("let h = {}\n", k_trace);
    auto const in_helper = cc::format("fun go(r: ray, p: mut radiance){{work}} -> float:\n"
                                      "    let h = {}\n    h.invoke(mut p)\n    return h.t\n",
                                      k_trace);
    path const paths[] = {
        {"let, read and invoke", "", "if h.is_hit => h.invoke(mut p)\n"},
        {"every read", "", "p.color = h.t + float(h.instance_id + h.primitive_index + h.geometry_index)\n"},
        {"both reorders", "", "h.reorder()\nh.reorder(id.x as uint, 2)\nh.invoke(mut p)\n"},
        {"unused", "", ""},
        {"read in a loop", "", "for i in 0 ..< id.x:\n    h.reorder()\n    p.color += h.t\n"},
        {"handed to a function", reader, "p.color = read(h)\n"},
        {"handed to a function, twice", reader, "p.color = read(h) + read(h)\n"},
        {"handed through two functions",
         "fun inner(x: hit_object) -> float:\n    return x.t\n\n"
         "fun outer(x: hit_object) -> float:\n    return inner(x) + inner(x)\n",
         "p.color = outer(h)\n"},
        {"read past an early return",
         "fun early(x: hit_object, i: int) -> float:\n    if i > 0:\n        return x.t\n"
         "    return 0.0\n",
         "p.color = early(h, id.x)\n"},
        {"read in a function's loop",
         "fun looped(x: hit_object, n: int) -> float:\n    let mut v = 0.0\n"
         "    for i in 0 ..< n:\n        v += x.t\n    return v\n",
         "p.color = looped(h, id.x)\n"},
        {"a generic's parameter", "fun go[T](x: T) -> float:\n    return x.t\n", "p.color = go(h)\n"},
        {"caught by a lambda", "fun apply(x: float, f: (float) -> float) -> float:\n    return f(x)\n",
         "p.color = apply(1.0, x => x + h.t)\nh.invoke(mut p)\n"},
        {"a function's parameter of a function",
         "fun via(x: hit_object, f: (hit_object) -> float) -> float:\n"
         "    return f(x) + f(x)\n",
         "p.color = via(h, x => x.t)\n"},
        {"a method of ours", "fun hit_object.doubled => self.t * 2.0\n", "p.color = h.doubled\n"},
        {"a let in a function called twice", in_helper, "p.color = go(r, mut p) + go(r, mut p)\n"},
    };
    constexpr ssc::dxc::optimization_level levels[]
        = {ssc::dxc::optimization_level::level_3, ssc::dxc::optimization_level::disabled};
    for (auto const& p : paths)
        for (auto const diagnostics : {false, true})
        {
            auto const body = p.name.contains("called twice") ? cc::string(p.body) : traced + p.body;
            auto const source = raygen(p.declarations, body);
            auto const emitted = sgl::compile_to_text({.source = source,
                                                       .entry_point = "main_cs",
                                                       .target = sgl::emit::target::hlsl_vulkan,
                                                       .diagnostics = diagnostics,
                                                       .device_features = reordering_device()});
            REQUIRE(emitted.has_value())
                .dump("path", p.name)
                .dump("error", emitted.has_error() ? cc::string(emitted.error()) : cc::string());
            CHECK(!emitted.value().text.contains("= hit_object"));
            for (auto const level : levels)
            {
                auto const shader = built(compiler.value(), emitted.value(), level);
                REQUIRE(shader.has_value())
                    .dump("path", p.name)
                    .dump("error", shader.has_error() ? cc::format("{}", shader.error()) : cc::string());
                auto const walk = walk_hit_objects(words_of(shader.value()));
                CHECK(walk.variables >= 1).dump("path", p.name);
                CHECK(walk.copies.empty())
                    .dump("path", p.name)
                    .dump("first", walk.copies.empty() ? cc::string() : walk.copies[0]);
            }
        }
}

TEST("sgl hit object spirv - no raygen of the repo that holds a hit object copies one",
     exclusive("sgl-hit-object-sources"))
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    auto const here = nx::source_path(SLIB_SOURCE_DIR);
    auto const root = (fs::path(here.data(), here.data() + here.size()) / ".." / ".." / "..").lexically_normal();
    auto found = 0;
    auto holding = 0;
    for (auto const& e : fs::recursive_directory_iterator(root / "libs"))
    {
        if (!e.is_regular_file() || e.path().extension() != ".sgl")
            continue;
        auto const path = cc::string(e.path().generic_string().c_str());
        auto const source = read_file(path);
        if (!source.contains("trace_hit") && !source.contains("reorder"))
            continue;
        // a shader of a directory may `use` a module of its neighbours', or slug's
        auto names = cc::vector<cc::string>();
        auto sources = cc::vector<cc::string>();
        for (auto const& d : {e.path().parent_path(), root / "libs/graphics/shaped-rendering/shaders"})
            for (auto const& neighbour : fs::directory_iterator(d))
                if (neighbour.is_regular_file() && neighbour.path().extension() == ".sgl")
                {
                    names.push_back(cc::string(neighbour.path().generic_string().c_str()));
                    sources.push_back(read_file(names.back()));
                }
        auto library = cc::vector<sgl::library_file>();
        for (auto i = isize(0); i < names.size(); ++i)
            library.push_back({.name = names[i], .source = sources[i]});
        sgl::emit::target const targets[] = {sgl::emit::target::hlsl_vulkan};
        auto const all
            = sgl::compile_all_to_text({.source = source, .source_name = path, .library = library, .targets = targets});
        if (all.has_error())
            continue;
        for (auto const& entry : all.value())
        {
            auto const emitted = sgl::compile_to_text({.source = source,
                                                       .source_name = path,
                                                       .library = library,
                                                       .entry_point = entry.entry_point,
                                                       .target = sgl::emit::target::hlsl_vulkan,
                                                       .device_features = reordering_device()});
            if (emitted.has_error() || emitted.value().stage != sgl::check::stage::raygen)
                continue;
            ++found;
            auto const shader = built(compiler.value(), emitted.value(), ssc::dxc::optimization_level::level_3);
            REQUIRE(shader.has_value())
                .dump("path", path)
                .dump("entry", entry.entry_point)
                .dump("error", shader.has_error() ? cc::format("{}", shader.error()) : cc::string());
            auto const walk = walk_hit_objects(words_of(shader.value()));
            holding += walk.variables > 0 ? 1 : 0;
            CHECK(walk.copies.empty()).dump("path", path).dump("entry", entry.entry_point);
        }
    }
    // a sweep that quietly lost its shaders would pass the check above
    CHECK(found >= 3);
    CHECK(holding >= 2);
}
