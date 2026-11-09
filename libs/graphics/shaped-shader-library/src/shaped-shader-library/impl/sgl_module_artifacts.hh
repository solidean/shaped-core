#pragma once

#include <blob-cache/keys.hh> // bcache::cache_key, returned by value below
#include <clean-core/container/span.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/async.hh>
#include <shaped-shader-library/compiler/shader_compiler.hh>

namespace bcache
{
class blob_cache;
}

/// SGL's module artifacts kept in a persistent store: each module's checked layer, so a process loads it rather than
/// checking the module (`sgl::offer_module_artifacts`).
///
/// A compile reads nothing but its arguments, so nothing here is fetched during one.
/// A prefetch asks the store for every module's artifact and hands each to SGL as it lands, and never waits.
/// A compile that comes first checks its modules, which is what it would do with nothing kept.
namespace slib::impl
{
/// Where the store keeps the artifact SGL names `key`, which holds SGL's build already.
[[nodiscard]] bcache::cache_key sgl_module_artifact_key(cc::string_view key);

/// Turns keeping SGL's module artifacts on for the process, and asks `store` for each module of `modules` this process
/// has not asked for yet.
/// Ready once every answer was handed to SGL, a miss among them; awaiting it is what makes the process's first compile
/// of these modules load them, and nothing has to await it.
[[nodiscard]] cc::shared_async<cc::unit> prefetch_sgl_modules(cc::span<module_source const> modules,
                                                              bcache::blob_cache& store);

/// Puts the artifacts SGL made since the last call into `store`, fire and forget: a lost write is a later miss.
void keep_made_sgl_modules(bcache::blob_cache& store);

/// Forgets which modules this process asked a store for, so a test can play a process that never asked.
void forget_sgl_module_fetches();
} // namespace slib::impl
