#include <blob-cache/blob_cache.hh>
#include <blob-cache/default_cache.hh>
#include <clean-core/bytes/hash256.hh>
#include <clean-core/thread/async.hh>
#include <nexus/run.hh>
#include <shaped-graphics-language/driver/prelude.hh>

using namespace cc::primitive_defines;

// `sgl`, the command line of SGL's toolchain: every job is a COMMAND of this one nexus binary.
// `sgl <command> …` runs the command named first with the rest of the line, and `sgl` alone lists what it holds.
// A new tool of the toolchain - a formatter, a linter, the language server - is one more file with one more COMMAND.
int main(int argc, char** argv)
{
    // Checked layers live in the user's blob cache under slib's key, so a build's `describe` and an app's compile share them.
    // Only a command uses it: this binary's tests and listings are selected by a flag, and stay off the user's cache.
    if (argc > 1 && argv[1][0] != '-')
    {
        auto const key_of = [](cc::hash128 id) -> bcache::cache_key
        {
            return {.space = bcache::cache_namespace("sgl.layer"),
                    .key = bcache::logical_key::create_from_hash(
                        cc::hash256::create(cc::span<cc::hash128 const>(&id, 1).as_bytes())),
                    .version = bcache::version(1)};
        };
        sgl::set_artifact_store({
            .load = [key_of](cc::hash128 id) -> cc::optional<cc::vector<byte>>
            {
                // no answer within a second is a miss, so another process holding the file never stalls a build
                auto const got = cc::try_async_blocking_get_for(bcache::default_cache().get(key_of(id)), 1000);
                if (!got.has_value() || !got.value().has_value() || !got.value().value().has_value())
                    return {};
                return cc::vector<byte>::create_copy_of(got.value().value().value().data.span());
            },
            // fire and forget: the default cache drains what it accepted as the process ends
            .save =
                [key_of](cc::hash128 id, cc::span<byte const> bytes)
            {
                (void)bcache::default_cache().put(key_of(id),
                                                  cc::make_pinned_data(cc::vector<byte>::create_copy_of(bytes)),
                                                  {.compute_time_secs = 0.01});
            },
        });
    }
    return nx::run(argc, argv);
}
