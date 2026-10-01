#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-shader-library/compiler/sgl_compiler.hh>
#include <shaped-shader-library/compiler/wgsl_compiler.hh>
#include <shaped-shader-library/filesystem/memory_filesystem.hh>
#include <shaped-shader-library/shader_asset.hh>
#include <shaped-shader-library/shader_library.hh>
#include <shaped-shader-library/shader_package.hh>

#include <memory>

using namespace cc::primitive_defines;

// An SGL package's module directories are the library every SGL compile `use`s from, and a module file is a file a
// shader depends on: an edit to it reloads what reached it.
// WGSL needs no device, so everything here compiles for real on a memory filesystem.

namespace
{
constexpr auto k_view_v1 = cc::string_view("module view\n"
                                           "binding frame:\n"
                                           "    gain: float\n"
                                           "    values: mut buffer[float]\n"
                                           "const scale = 2\n");

constexpr auto k_main = cc::string_view("use view\n"
                                        "@compute(64) fun main(@thread_id id: int3){view.frame}:\n"
                                        "    view.frame.values[id.x] = view.frame.gain * (view.scale as float)\n");

/// The WGSL text of a SETTLED compile, or "error" where it did not compile.
cc::string text_of(sg::async_compiled_shader const& shader)
{
    REQUIRE(shader != nullptr);
    if (!shader->has_value())
        return "error";
    auto const& bytes = shader->try_value()->bytecode;
    return cc::string(cc::string_view(reinterpret_cast<char const*>(bytes.data()), bytes.size()));
}

/// One SGL package on a memory filesystem whose own directory is its module directory, as a generated one's is.
struct module_fixture
{
    std::shared_ptr<slib::memory_filesystem> fs = std::make_shared<slib::memory_filesystem>();
    slib::shader_library lib;
    slib::shader_asset_handle main;
    slib::shader_definition definitions[1] = {
        {.path = "main.sgl", .stage = sg::shader_stage::compute, .entry_point = "main", .asset = &main},
    };
    slib::module_dir module_dirs[1] = {{.path = "", .source_dir = "<memory>"}};

    module_fixture()
    {
        fs->write("view.sgl", k_view_v1);
        fs->write("main.sgl", k_main);
        lib.add_compiler(slib::create_sgl_compiler(slib::create_wgsl_compiler()));
        lib.add_package({.name = "mod_pkg",
                         .language = slib::shader_language::sgl,
                         .definitions = definitions,
                         .module_dirs = module_dirs},
                        fs);
    }

    /// The WGSL text `main` compiles to now.
    [[nodiscard]] cc::shared_async<cc::string> text() const
    {
        auto const shader = main->acquire(sg::shader_format::wgsl);
        co_await cc::async_settled(shader);
        co_return text_of(shader);
    }
};
} // namespace

ASYNC_TEST("slib sgl modules - a shader uses a module of its package's directory, and depends on its file",
           exclusive("slib-shader-library"))
{
    auto f = module_fixture();
    auto const text = co_await f.text();
    CHECK(text != "error");
    CHECK(text.contains("frame"));
    // the module's file is one the shader was built from, beside its own
    auto has_module = false;
    for (auto const& d : f.main->dependencies())
        has_module = has_module || d == "mod_pkg/view.sgl";
    CHECK(has_module);
}

ASYNC_TEST("slib sgl modules - editing a module reloads the shaders that use it", exclusive("slib-shader-library"))
{
    auto f = module_fixture();
    auto const before = co_await f.text();
    REQUIRE(before != "error");
    f.lib.start_hot_reload({.unthreaded = true});

    auto edited = cc::string(k_view_v1);
    edited.replace_all("const scale = 2", "const scale = 7");
    f.fs->write("view.sgl", edited);
    f.lib.poll_hot_reload();
    auto const after = co_await f.text();
    CHECK(after != before);
    CHECK(f.main->last_error() == cc::nullopt);
}

ASYNC_TEST("slib sgl modules - a broken module still has the shaders that used it watch it",
           exclusive("slib-shader-library"))
{
    auto f = module_fixture();
    REQUIRE(co_await f.text() != "error");
    f.lib.start_hot_reload({.unthreaded = true});

    // the edit breaks the module, which keeps the last good shader running and says why
    f.fs->write("view.sgl", "module view\nconst scale = nope\n");
    f.lib.poll_hot_reload();
    (void)co_await f.text();
    CHECK(f.main->last_error().has_value());

    // and fixing it reaches the shader again, though the broken compile could not say which modules it reached
    f.fs->write("view.sgl", k_view_v1);
    f.lib.poll_hot_reload();
    (void)co_await f.text();
    CHECK(f.main->last_error() == cc::nullopt);
}

ASYNC_TEST("slib sgl modules - a source compiled from memory uses the library's modules too",
           exclusive("slib-shader-library"))
{
    auto f = module_fixture();
    auto const node = f.lib.compile_source(k_main, sg::shader_stage::compute, "main", sg::shader_format::wgsl,
                                           {.language = slib::shader_language::sgl, .label = "generated.sgl"});
    co_await cc::async_settled(node);
    CHECK(text_of(node) != "error");
}

ASYNC_TEST("slib sgl modules - a module directory of no package is added by its mounted path",
           exclusive("slib-shader-library"))
{
    auto f = module_fixture();
    auto shared = std::make_shared<slib::memory_filesystem>();
    shared->write("tone.sgl", "module tone\nconst exposure = 3\n");
    f.lib.mount("shared/modules", shared);
    f.lib.add_module_dir("shared/modules");

    auto has_tone = false;
    for (auto const& p : f.lib.read_modules().paths)
        has_tone = has_tone || p == "shared/modules/tone.sgl";
    CHECK(has_tone);

    auto const node = f.lib.compile_source("use tone\n@compute(1) fun main(@thread_id id: int3):\n"
                                           "    let x = tone.exposure\n",
                                           sg::shader_stage::compute, "main", sg::shader_format::wgsl,
                                           {.language = slib::shader_language::sgl, .label = "uses_tone.sgl"});
    co_await cc::async_settled(node);
    CHECK(text_of(node) != "error");
}
