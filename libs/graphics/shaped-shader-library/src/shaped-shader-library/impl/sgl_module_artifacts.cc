#include <blob-cache/blob_cache.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/set.hh>
#include <clean-core/thread/async_backlog.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics-language/driver/shared_library.hh>
#include <shaped-shader-library/impl/sgl_library.hh>
#include <shaped-shader-library/impl/sgl_module_artifacts.hh>

using namespace cc::primitive_defines;

namespace
{
/// The keys this process asked a store for, so a module is fetched once whatever number of compilers ask.
cc::mutex<cc::set<cc::string>> g_asked;

/// The fetches started and the puts handed off, which nobody may await.
cc::async_backlog& backlog()
{
    static auto b = cc::async_backlog("slib.sgl.modules");
    return b;
}

cc::shared_async<cc::unit> all_of(cc::vector<cc::shared_async<cc::unit>> nodes)
{
    co_await cc::async_all(cc::span<cc::shared_async<cc::unit> const>(nodes));
    co_return;
}
} // namespace

bcache::cache_key slib::impl::sgl_module_artifact_key(cc::string_view key)
{
    return {.space = bcache::cache_namespace("slib.sgl.module"),
            .key = bcache::logical_key::create_from_string(key),
            .version = bcache::version(1)};
}

cc::shared_async<cc::unit> slib::impl::prefetch_sgl_modules(cc::span<module_source const> modules,
                                                            bcache::blob_cache& store)
{
    sgl::keep_module_artifacts(true);
    auto const library = sgl_library_of(modules);
    auto fetches = cc::vector<cc::shared_async<cc::unit>>();
    for (auto& key : sgl::module_artifact_keys(library, ""))
    {
        if (!g_asked.lock([&](cc::set<cc::string>& asked) { return asked.insert(key); }))
            continue;
        auto const hit = store.get(sgl_module_artifact_key(key));
        auto handed = cc::make_async_lazy(
            [key = cc::move(key)](cc::optional<bcache::cache_hit> const& found)
            {
                if (found.has_value())
                {
                    auto const artifact
                        = sgl::module_artifact{.key = key,
                                               .bytes = cc::vector<byte>::create_copy_of(found.value().data.span())};
                    sgl::offer_module_artifacts(cc::span<sgl::module_artifact const>(&artifact, 1));
                }
                return cc::unit{};
            },
            hit);
        (void)backlog().start(handed);
        fetches.push_back(cc::move(handed));
    }
    return all_of(cc::move(fetches));
}

void slib::impl::forget_sgl_module_fetches()
{
    g_asked.lock([](cc::set<cc::string>& asked) { asked.clear(); });
}

void slib::impl::keep_made_sgl_modules(bcache::blob_cache& store)
{
    for (auto& made : sgl::take_module_artifacts())
        (void)store.put(sgl_module_artifact_key(made.key), cc::make_pinned_data(cc::move(made.bytes)));
}
