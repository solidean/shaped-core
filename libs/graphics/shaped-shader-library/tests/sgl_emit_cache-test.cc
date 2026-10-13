#include <blob-cache/blob_cache.hh>
#include <clean-core/bytes/hash128.hh>
#include <clean-core/container/pinned_data.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <nexus/test.hh>
#include <shaped-shader-library/compiler/sgl_compiler.hh>
#include <shaped-shader-library/compiler/wgsl_compiler.hh>
#include <shaped-shader-library/impl/sgl_emit_cache.hh>

using namespace cc::primitive_defines;

// The cache in front of SGL's front end: its byte form, and which tier answers when.

namespace
{
/// A preprocess result with every field the byte form writes set to something other than its default.
[[nodiscard]] slib::preprocessed_source rich_result()
{
    // Off the persistent tier, so the value is the front end's own and no store answers it.
    auto const compiler = slib::create_sgl_compiler(slib::create_wgsl_compiler(), {.persistent_cache = nullptr});
    auto resolve = [](cc::string_view) -> cc::optional<cc::string> { return cc::nullopt; };
    auto text = compiler->preprocess({.source = "@inline binding constants:\n"
                                                "    view_projection: mat4\n"
                                                "@vertex struct cube_vertex:\n"
                                                "    position: pos3\n"
                                                "struct pixel_input:\n"
                                                "    @position position: hpos4\n"
                                                "@vertex fun main_vs(v: cube_vertex){constants} -> pixel_input:\n"
                                                "    return {\n"
                                                "        position = constants.view_projection * v.position\n"
                                                "    }\n",
                                      .entry_point = "main_vs",
                                      .stage = sg::shader_stage::vertex},
                                     resolve);
    REQUIRE(text.has_value());
    auto result = cc::move(text.value());
    REQUIRE(result.stated.has_value());

    // What SGL leaves empty for this source, so the byte form is held to every field.
    result.entry_point = "main_vs_renamed";
    result.used_modules.push_back(cc::string("view/frame.sgl"));
    result.warnings.push_back(cc::string("cube.sgl:3:5: warning: no-effect: a statement that does nothing"));
    result.warnings.push_back(cc::string("view/frame.sgl:7:14: warning: unroll-too-long: 257 values\n"
                                         "view/frame.sgl:6:5: note: the loop"));
    result.layouts.push_back({.global = "constants", .stride = 0, .fields = {{.name = "view_projection", .offset = 0}}});
    result.declared_bindings.back().reflected_name = "sgl_constants";
    auto& stated = result.stated.value();
    stated.preferred_subgroup_size = 32;
    stated.target_set = "frame";
    stated.required_features = sg::feature_set::create_from_bits(sg::feature_set::storage_type(0b101));
    return result;
}

[[nodiscard]] bool same_bindings(cc::span<sg::binding const> a, cc::span<sg::binding const> b)
{
    if (a.size() != b.size())
        return false;
    for (auto i = isize(0); i < a.size(); ++i)
        if (a[i].name != b[i].name || a[i].reflected_name != b[i].reflected_name || a[i].type != b[i].type
            || a[i].index != b[i].index || a[i].space != b[i].space || a[i].group_index != b[i].group_index
            || a[i].visibility != b[i].visibility)
            return false;
    return true;
}

/// The fields the byte form writes beyond what sg's own codec test holds it to.
[[nodiscard]] bool same(slib::preprocessed_source const& a, slib::preprocessed_source const& b)
{
    if (a.source != b.source || a.entry_point != b.entry_point || a.used_modules.size() != b.used_modules.size())
        return false;
    for (auto i = isize(0); i < a.used_modules.size(); ++i)
        if (a.used_modules[i] != b.used_modules[i])
            return false;
    if (a.warnings.size() != b.warnings.size())
        return false;
    for (auto i = isize(0); i < a.warnings.size(); ++i)
        if (a.warnings[i] != b.warnings[i])
            return false;
    if (a.layouts.size() != b.layouts.size())
        return false;
    for (auto i = isize(0); i < a.layouts.size(); ++i)
    {
        auto const& x = a.layouts[i];
        auto const& y = b.layouts[i];
        if (x.global != y.global || x.stride != y.stride || x.fields.size() != y.fields.size())
            return false;
        for (auto j = isize(0); j < x.fields.size(); ++j)
            if (x.fields[j].name != y.fields[j].name || x.fields[j].offset != y.fields[j].offset)
                return false;
    }
    if (!same_bindings(a.declared_bindings, b.declared_bindings))
        return false;
    if (a.stated.has_value() != b.stated.has_value())
        return false;
    if (!a.stated.has_value())
        return true;
    auto const& x = a.stated.value();
    auto const& y = b.stated.value();
    return x.stage == y.stage && x.format == y.format && x.entry_point == y.entry_point
        && same_bindings(x.bindings, y.bindings) && x.footprint == y.footprint
        && x.preferred_subgroup_size == y.preferred_subgroup_size && x.target_set == y.target_set
        && x.required_features == y.required_features;
}

/// A key no other test, and no earlier run, has stored anything under.
[[nodiscard]] cc::hash128 fresh_key(cc::string_view what)
{
    auto const path = cc::temp_file_path("slib-sgl-emit-key", ".txt");
    auto const text = cc::format("{}|{}", what, path);
    return cc::hash128::create(cc::span<char const>(text.data(), text.size()).as_bytes(), 0);
}

/// A store of the test's own, which starts empty, is shared with nothing, and is removed when the test ends.
struct private_store
{
    cc::string path = cc::temp_file_path("slib-sgl-emit-test", ".db");
    cc::unique_ptr<bcache::blob_cache> cache = bcache::blob_cache::create({.path = path});

    ~private_store()
    {
        cache->close();
        (void)cc::remove_file(path);
        (void)cc::remove_file(cc::format("{}-wal", path));
        (void)cc::remove_file(cc::format("{}-shm", path));
    }
};
} // namespace

TEST("slib sgl emit cache - a result round-trips its byte form")
{
    auto const original = rich_result();
    auto const decoded = slib::impl::decode_preprocessed(slib::impl::encode_preprocessed(original));
    REQUIRE(decoded.has_value());
    CHECK(same(original, decoded.value()));
}

TEST("slib sgl emit cache - the byte form refuses anything it did not write")
{
    auto const encoded = slib::impl::encode_preprocessed(rich_result());

    // A cache may miss, never lie: no truncation comes back as a result.
    auto truncations_read = isize(0);
    for (auto cut = isize(0); cut < encoded.size(); ++cut)
        if (slib::impl::decode_preprocessed(cc::span<byte const>(encoded).first_n(cut)).has_value())
            ++truncations_read;
    CHECK(truncations_read == 0);

    auto extended = encoded;
    extended.push_back(byte(0));
    CHECK(!slib::impl::decode_preprocessed(extended).has_value());

    auto wrong_version = encoded;
    wrong_version[0] = byte(0xFF);
    CHECK(!slib::impl::decode_preprocessed(wrong_version).has_value());
}

TEST("slib sgl emit cache - a repeat is answered without the front end, and a failure never is")
{
    auto const value = rich_result();
    auto runs = 0;
    auto compute = [&]() -> cc::result<slib::preprocessed_source>
    {
        ++runs;
        return value;
    };

    auto const key = fresh_key("memory");
    auto const first = slib::impl::cached_sgl_preprocess(key, compute, nullptr);
    auto const again = slib::impl::cached_sgl_preprocess(key, compute, nullptr);
    REQUIRE(first.has_value());
    REQUIRE(again.has_value());
    CHECK(runs == 1);
    CHECK(same(first.value(), again.value()));

    CHECK(slib::impl::cached_sgl_preprocess(fresh_key("another"), compute, nullptr).has_value());
    CHECK(runs == 2);

    // A broken source runs every time, so its diagnostics stay those of its current text.
    auto failures = 0;
    auto failing = [&]() -> cc::result<slib::preprocessed_source>
    {
        ++failures;
        return cc::error("SGL reported errors");
    };
    auto const broken = fresh_key("broken");
    CHECK(slib::impl::cached_sgl_preprocess(broken, failing, nullptr).has_error());
    CHECK(slib::impl::cached_sgl_preprocess(broken, failing, nullptr).has_error());
    CHECK(failures == 2);
}

TEST("slib sgl emit cache - the persistent tier answers once memory has forgotten")
{
    if (!bcache::blob_cache::is_storage_available())
        SKIP("no storage for a blob cache in this build");
    if (!CC_HAS_THREADS)
        SKIP("the persistent tier waits for the store's own thread, which this build does not have");

    auto const store = private_store();
    auto const value = rich_result();
    auto runs = 0;
    auto compute = [&]() -> cc::result<slib::preprocessed_source>
    {
        ++runs;
        return value;
    };

    // No patience bound: a store slow to open must not turn into a miss and fail the test.
    auto const key = fresh_key("persistent");
    REQUIRE(slib::impl::cached_sgl_preprocess(key, compute, store.cache.get(), cc::nullopt).has_value());
    CHECK(runs == 1);

    // The store's actor takes messages in order, so the lookup below sees the write the first call queued.
    slib::impl::forget_sgl_emit(key);
    auto const reread = slib::impl::cached_sgl_preprocess(key, compute, store.cache.get(), cc::nullopt);
    REQUIRE(reread.has_value());
    CHECK(runs == 1);
    CHECK(same(value, reread.value()));
}

TEST("slib sgl emit cache - an entry that fails to decode is replaced")
{
    if (!bcache::blob_cache::is_storage_available())
        SKIP("no storage for a blob cache in this build");
    if (!CC_HAS_THREADS)
        SKIP("the persistent tier waits for the store's own thread, which this build does not have");

    // Bytes the codec never wrote, under the key a lookup reads: what a torn write or a stale layout leaves behind.
    auto const store = private_store();
    auto const key = fresh_key("corrupt");
    auto corrupt = cc::vector<byte>();
    corrupt.push_back(byte(0xFF));
    auto const planted
        = store.cache->put(slib::impl::sgl_emit_persistent_key(key), cc::make_pinned_data(cc::move(corrupt)));

    auto const value = rich_result();
    auto runs = 0;
    auto compute = [&]() -> cc::result<slib::preprocessed_source>
    {
        ++runs;
        return value;
    };
    REQUIRE(slib::impl::cached_sgl_preprocess(key, compute, store.cache.get(), cc::nullopt).has_value());
    CHECK(runs == 1);

    // The store answered that lookup after the put queued before it, so the put has settled by now.
    REQUIRE(planted->is_ready());
    REQUIRE(planted->has_value());
    REQUIRE(planted->value().is_present());

    // The first writer wins, so only an entry dropped on the failed decode lets this read the recomputed one.
    slib::impl::forget_sgl_emit(key);
    auto const reread = slib::impl::cached_sgl_preprocess(key, compute, store.cache.get(), cc::nullopt);
    REQUIRE(reread.has_value());
    CHECK(runs == 1);
    CHECK(same(value, reread.value()));
}
