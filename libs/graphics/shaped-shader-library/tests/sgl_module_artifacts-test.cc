#include <blob-cache/blob_cache.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/ast/build.hh>
#include <shaped-graphics-language/driver/shared_library.hh>
#include <shaped-shader-library/compiler/sgl_compiler.hh>
#include <shaped-shader-library/compiler/wgsl_compiler.hh>
#include <shaped-shader-library/impl/sgl_library.hh>
#include <shaped-shader-library/impl/sgl_module_artifacts.hh>

using namespace cc::primitive_defines;

// SGL's module artifacts kept in a persistent store: made by one process's compile, loaded by the next one's.

namespace
{
/// A store of the test's own, which starts empty, is shared with nothing, and is removed when the test ends.
struct private_store
{
    cc::string path = cc::temp_file_path("slib-sgl-module-test", ".db");
    cc::unique_ptr<bcache::blob_cache> cache = bcache::blob_cache::create({.path = path});

    ~private_store()
    {
        cache->close();
        (void)cc::remove_file(path);
        (void)cc::remove_file(cc::format("{}-wal", path));
        (void)cc::remove_file(cc::format("{}-shm", path));
    }
};

constexpr auto k_program = "use kept_top\n"
                           "const scale = kept_top.kept_both(1.0)\n"
                           "@inline binding constants:\n"
                           "    view_projection: mat4\n"
                           "@vertex struct cube_vertex:\n"
                           "    position: pos3\n"
                           "struct pixel_input:\n"
                           "    @position position: hpos4\n"
                           "@vertex fun main_vs(v: cube_vertex){constants} -> pixel_input:\n"
                           "    return {\n"
                           "        position = constants.view_projection * v.position\n"
                           "    }\n";
} // namespace

ASYNC_TEST("slib sgl module artifacts - a module one process checked is loaded by the next from the store, and never "
           "checked there")
{
    if (!bcache::blob_cache::is_storage_available())
        SKIP("no storage for a blob cache in this build");
    if (!CC_HAS_THREADS)
        SKIP("the store answers on its own thread, which this build does not have");

    auto const store = private_store();
    // sources no other test and no earlier run holds, so this process never checked these modules before
    auto const base_text
        = cc::format("// {}\nmodule kept_base\nfun kept_twice(x: float) -> float => x + x\n", store.path);
    auto const top_text = cc::format("// {}\nmodule kept_top\nuse kept_base\nfun kept_both(x: float) -> float => "
                                     "kept_base.kept_twice(x) * 3.0\n",
                                     store.path);
    auto const compiler = slib::create_sgl_compiler(
        slib::create_wgsl_compiler(), {.persistent_cache = store.cache.get(), .keeps_module_artifacts = true});
    auto resolve = [](cc::string_view) -> cc::optional<cc::string> { return cc::nullopt; };
    auto const compile = [&](cc::span<slib::module_source const> modules)
    {
        return compiler->preprocess(
            {.source = k_program, .entry_point = "main_vs", .stage = sg::shader_stage::vertex, .modules = modules},
            resolve);
    };

    // The first process: nothing is kept yet, so its compile checks both modules and keeps what it checked.
    auto const first = cc::vector<slib::module_source>{
        {.path = "first/kept_base.sgl", .text = base_text},
        {.path = "first/kept_top.sgl", .text = top_text},
    };
    auto const read_first = [&]
    {
        return slib::module_library{.paths = {"first/kept_base.sgl", "first/kept_top.sgl"},
                                    .texts = {base_text, top_text},
                                    .revisions = {{}, {}}};
    };
    auto const first_fetch = compiler->prefetch_modules(read_first);
    co_await first_fetch;
    REQUIRE(compile(first).has_value());

    // The next process, as far as SGL's links go: the same sources under other names, a library sharing no link with
    // the first, which never asked the store for anything.
    // Its prefetch finds both modules, and its compile loads them.
    slib::impl::forget_sgl_module_fetches();
    auto const next = cc::vector<slib::module_source>{
        {.path = "next/kept_base.sgl", .text = base_text},
        {.path = "next/kept_top.sgl", .text = top_text},
    };
    auto const next_fetch = slib::impl::prefetch_sgl_modules(next, *store.cache);
    co_await next_fetch;
    REQUIRE(compile(next).has_value());

    auto const library = slib::impl::sgl_library_of(next);
    auto const shared = sgl::shared_library_of(library, "<sgl>");
    REQUIRE(shared != nullptr);
    auto const program_file = sgl::parse(k_program);
    auto const program_ast = sgl::ast::build(program_file);
    auto const loaded = sgl::layer_of(*shared, {.file = program_file, .ast = program_ast});
    REQUIRE(loaded != nullptr);
    CHECK(loaded->layer->is_relocated());
    REQUIRE(loaded->before != nullptr);
    CHECK(loaded->before->layer->is_relocated());
    sgl::keep_module_artifacts(false);
}
