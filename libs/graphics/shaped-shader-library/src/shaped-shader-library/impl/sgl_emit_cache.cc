#include <blob-cache/blob_cache.hh>
#include <blob-cache/default_cache.hh>
#include <clean-core/bytes/hash256.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/common/time.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/byte_stream_builder.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/pinned_data.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-core/thread/thread.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-graphics/binding/impl/shader_codec.hh>
#include <shaped-graphics/context/cold_caches.hh>
#include <shaped-shader-library/impl/sgl_emit_cache.hh>

using namespace cc::primitive_defines;

namespace
{
/// The layout revision `encode_preprocessed` writes; bump it with any change to what it writes.
/// sg's codec version rides in the persistent key beside it, since a stated shader and its bindings are written in that codec.
constexpr u32 k_codec_version = 2;

/// How many results the in-memory tier holds before it starts over; each is a few dozen kilobytes of text.
constexpr isize k_memory_capacity = 512;

void put_u32(cc::vector<byte>& out, u32 value)
{
    for (auto i = 0; i < 4; ++i)
        out.push_back(byte((value >> (8 * i)) & 0xFF));
}

void put_u64(cc::vector<byte>& out, u64 value)
{
    for (auto i = 0; i < 8; ++i)
        out.push_back(byte((value >> (8 * i)) & 0xFF));
}

void put_bytes(cc::vector<byte>& out, cc::span<byte const> bytes)
{
    put_u64(out, u64(bytes.size()));
    for (auto b : bytes)
        out.push_back(b);
}

void put_string(cc::vector<byte>& out, cc::string_view text)
{
    put_bytes(out, cc::span<char const>(text.data(), text.size()).as_bytes());
}

/// A cursor that goes sour on the first bad read and stays that way, so the decode reads straight through and is checked once.
struct reader
{
    cc::span<byte const> data;
    isize pos = 0;
    bool ok = true;

    [[nodiscard]] bool take(isize count)
    {
        if (!ok || count < 0 || pos + count > data.size())
            ok = false;
        return ok;
    }

    [[nodiscard]] u64 get_u64()
    {
        if (!take(8))
            return 0;
        auto value = u64(0);
        for (auto i = 0; i < 8; ++i)
            value |= u64(data[pos + i]) << (8 * i);
        pos += 8;
        return value;
    }

    [[nodiscard]] u32 get_u32()
    {
        if (!take(4))
            return 0;
        auto value = u32(0);
        for (auto i = 0; i < 4; ++i)
            value |= u32(data[pos + i]) << (8 * i);
        pos += 4;
        return value;
    }

    /// A length-prefixed run, as a view into the input.
    [[nodiscard]] cc::span<byte const> get_bytes()
    {
        auto const size = isize(get_u64());
        if (!take(size))
            return {};
        auto const view = data.subspan({.offset = pos, .size = size});
        pos += size;
        return view;
    }

    [[nodiscard]] cc::string get_string()
    {
        auto const view = get_bytes();
        return cc::string(cc::string_view(reinterpret_cast<char const*>(view.data()), view.size()));
    }

    /// A count spent only once the remaining bytes could hold that many entries of at least `bytes_per_entry`.
    [[nodiscard]] isize get_count(isize bytes_per_entry)
    {
        auto const count = isize(get_u64());
        if (count < 0 || count > (data.size() - pos) / bytes_per_entry)
        {
            ok = false;
            return 0;
        }
        return count;
    }
};

/// The in-memory tier, shared by every compiler in the process.
cc::mutex<cc::map<cc::hash128, slib::preprocessed_source>>& memory_tier()
{
    static auto tier = cc::mutex<cc::map<cc::hash128, slib::preprocessed_source>>();
    return tier;
}

/// The store's answer for `key`, waited for without driving anything else on this thread.
/// A compile may run where its caller holds a lock or a reference that other work could invalidate, so it only waits.
/// The store answers on its own thread, which is what lets a wait alone see the answer arrive.
/// Nothing within `patience_secs` is a miss, and nullopt waits for the answer however long it takes.
///
/// TEMPORARY: a hand-rolled poll, standing in for a passive timed wait on a `cc::async` node, which clean-core lacks.
/// `cc::try_async_blocking_get_for` is the timed wait it has, and that one drives the ambient scheduler, which this must not.
/// Replace the loop with that wait once it lands, as libs/base/clean-core/docs/TODO.md records under async.
cc::optional<bcache::cache_hit> passive_get(bcache::blob_cache& store, cc::hash128 key, cc::optional<double> patience_secs)
{
    auto const node = store.get(slib::impl::sgl_emit_persistent_key(key));
    auto const deadline = cc::current_time_steady_secs() + patience_secs.value_or(0.0);
    auto yields = 0;
    while (!node->is_ready())
    {
        if (patience_secs.has_value() && cc::current_time_steady_secs() > deadline)
            return {};
        // A read is usually answered within a few yields; past those, a short sleep keeps the core free.
        if (yields < 256)
        {
            cc::this_thread_yield();
            ++yields;
        }
        else
            cc::this_thread_sleep_secs(0.0002);
    }
    auto const* const value = node->try_value();
    if (value == nullptr)
        return {};
    return *value;
}

void remember(cc::hash128 key, slib::preprocessed_source const& value)
{
    memory_tier().lock(
        [&](cc::map<cc::hash128, slib::preprocessed_source>& tier)
        {
            if (tier.size() >= k_memory_capacity)
                tier.clear();
            tier[key] = value;
        });
}
} // namespace

cc::vector<byte> slib::impl::encode_preprocessed(preprocessed_source const& source)
{
    auto out = cc::vector<byte>();
    put_u32(out, k_codec_version);
    put_string(out, source.source);
    put_string(out, source.entry_point);

    put_u64(out, u64(source.used_modules.size()));
    for (auto const& m : source.used_modules)
        put_string(out, m);

    put_u64(out, u64(source.warnings.size()));
    for (auto const& w : source.warnings)
        put_string(out, w);

    put_u64(out, u64(source.layouts.size()));
    for (auto const& l : source.layouts)
    {
        put_string(out, l.global);
        put_u64(out, u64(l.stride));
        put_u64(out, u64(l.fields.size()));
        for (auto const& f : l.fields)
        {
            put_string(out, f.name);
            put_u64(out, u64(f.offset));
        }
    }

    put_bytes(out, sg::impl::encode_bindings(source.declared_bindings));

    // The stated shader in sg's codec, and after it the fields that codec leaves to whoever states them.
    put_u32(out, source.stated.has_value() ? 1 : 0);
    if (source.stated.has_value())
    {
        auto const& s = source.stated.value();
        put_bytes(out, sg::impl::encode_compiled_shader(s));
        put_u32(out, s.preferred_subgroup_size.has_value() ? 1 : 0);
        put_u32(out, u32(s.preferred_subgroup_size.value_or(0)));
        put_string(out, s.target_set);
        put_u32(out, s.required_features.has_value() ? 1 : 0);
        put_u64(out, u64(s.required_features.has_value() ? s.required_features.value().bits : 0));
    }
    return out;
}

cc::optional<slib::preprocessed_source> slib::impl::decode_preprocessed(cc::span<byte const> bytes)
{
    auto r = reader{.data = bytes};
    if (r.get_u32() != k_codec_version)
        return {};

    auto result = preprocessed_source();
    result.source = r.get_string();
    result.entry_point = r.get_string();

    // Every entry is at least its length prefixes, which bounds a count before anything is reserved for it.
    auto const module_count = r.get_count(8);
    for (auto i = isize(0); r.ok && i < module_count; ++i)
        result.used_modules.push_back(r.get_string());

    auto const warning_count = r.get_count(8);
    for (auto i = isize(0); r.ok && i < warning_count; ++i)
        result.warnings.push_back(r.get_string());

    auto const layout_count = r.get_count(24);
    for (auto i = isize(0); r.ok && i < layout_count; ++i)
    {
        auto layout = block_layout{.global = r.get_string(), .stride = isize(r.get_u64())};
        auto const field_count = r.get_count(16);
        for (auto j = isize(0); r.ok && j < field_count; ++j)
        {
            auto name = r.get_string();
            auto const offset = isize(r.get_u64());
            layout.fields.push_back({.name = cc::move(name), .offset = offset});
        }
        result.layouts.push_back(cc::move(layout));
    }

    auto declared = sg::impl::decode_bindings(r.get_bytes());
    if (!r.ok || !declared.has_value())
        return {};
    result.declared_bindings = cc::move(declared.value());

    auto const has_stated = r.get_u32();
    if (has_stated > 1)
        return {};
    if (has_stated == 1)
    {
        auto stated = sg::impl::decode_compiled_shader(r.get_bytes());
        auto const has_subgroup = r.get_u32();
        auto const subgroup = i32(r.get_u32());
        auto target_set = r.get_string();
        auto const has_features = r.get_u32();
        auto const features = r.get_u64();
        if (!r.ok || !stated.has_value() || has_subgroup > 1 || has_features > 1)
            return {};
        auto& s = stated.value();
        if (has_subgroup == 1)
            s.preferred_subgroup_size = subgroup;
        s.target_set = cc::move(target_set);
        if (has_features == 1)
            s.required_features = sg::feature_set::create_from_bits(sg::feature_set::storage_type(features));
        result.stated = cc::move(s);
    }

    // Trailing bytes mean this is not the blob we think it is.
    if (!r.ok || r.pos != bytes.size())
        return {};
    return result;
}

cc::result<sgl::module_description, cc::string> slib::impl::cached_describe(sgl::describe_request const& request)
{
    static auto memo = cc::mutex<cc::map<cc::hash128, sgl::module_description>>();

    auto& b = cc::byte_stream_builder::thread_local_scratch();
    b.add_string(request.source);
    b.add_string(request.source_name);
    b.add_string(request.module);
    b.add_pod(i64(request.options.size()));
    for (auto const& o : request.options)
    {
        b.add_string(o.name);
        b.add_string(o.value);
    }
    b.add_pod(i64(request.library.size()));
    for (auto const& f : request.library)
    {
        b.add_string(f.name);
        b.add_string(f.source);
    }
    auto const key = cc::hash128::create(b.written_bytes(), 0);

    auto remembered = memo.lock(
        [&](cc::map<cc::hash128, sgl::module_description>& m) -> cc::optional<sgl::module_description>
        {
            if (auto const* const found = m.get_ptr(key); found != nullptr)
                return *found;
            return {};
        });
    if (remembered.has_value())
        return cc::move(remembered.value());

    auto described = sgl::describe(request);
    if (described.has_value())
        memo.lock(
            [&](cc::map<cc::hash128, sgl::module_description>& m)
            {
                if (m.size() >= k_memory_capacity)
                    m.clear();
                m[key] = described.value();
            });
    return described;
}

bcache::cache_key slib::impl::sgl_emit_persistent_key(cc::hash128 key)
{
    auto& b = cc::byte_stream_builder::thread_local_scratch();
    b.add_pod(key);
    b.add_string(sgl::compiler_fingerprint());
    b.add_string(sgl_edge_fingerprint());
    return {.space = bcache::cache_namespace("slib.sgl.emit"),
            .key = bcache::logical_key::create_from_hash(cc::hash256::create(b.written_bytes())),
            .version = bcache::version(i32(k_codec_version * 1000 + sg::impl::k_shader_codec_version))};
}

void slib::impl::forget_sgl_emit(cc::hash128 key)
{
    memory_tier().lock([&](cc::map<cc::hash128, preprocessed_source>& tier) { (void)tier.erase(key); });
}

bcache::blob_cache* slib::impl::sgl_emit_store(cc::optional<bcache::blob_cache*> chosen)
{
#if defined(__EMSCRIPTEN__) || !CC_HAS_THREADS
    // A lookup waits for the store's own thread, which a page must not do and an unthreaded build does not have.
    (void)chosen;
    return nullptr;
#else
    if (chosen.has_value())
        return chosen.value();
    // Cold turns off the default store only: one given explicitly is a choice.
    if (sg::cold_caches_from_environment().shaders)
        return nullptr;
    return &bcache::default_cache();
#endif
}

cc::result<slib::preprocessed_source> slib::impl::cached_sgl_preprocess(
    cc::hash128 key,
    cc::function_ref<cc::result<preprocessed_source>()> compute,
    bcache::blob_cache* persistent,
    cc::optional<double> patience_secs)
{
    CC_RECORD_ACCUM("slib.sgl_emit.requests", cc::rec::unit_count, 1);

    auto remembered = memory_tier().lock(
        [&](cc::map<cc::hash128, preprocessed_source>& tier) -> cc::optional<preprocessed_source>
        {
            if (auto const* const found = tier.get_ptr(key); found != nullptr)
                return *found;
            return {};
        });
    if (remembered.has_value())
        return cc::move(remembered.value());

    if (persistent != nullptr)
    {
        // A miss, an expired entry and a storage failure are one answer, and every one of them falls through to the front end.
        auto const hit = passive_get(*persistent, key, patience_secs);
        if (hit.has_value())
        {
            if (auto decoded = decode_preprocessed(hit.value().data.span()); decoded.has_value())
            {
                CC_RECORD_ACCUM("slib.sgl_emit.persistent_hits", cc::rec::unit_count, 1);
                remember(key, decoded.value());
                return cc::move(decoded.value());
            }
            // The first writer wins, so an entry left in place would answer every later run with the same bytes.
            // The store takes messages in order, so the put below replaces it.
            (void)persistent->invalidate(sgl_emit_persistent_key(key));
        }
    }

    CC_RECORD_ACCUM("slib.sgl_emit.misses", cc::rec::unit_count, 1);
    auto const started_secs = cc::current_time_steady_secs();
    auto computed = compute();
    auto const compute_secs = cc::current_time_steady_secs() - started_secs;
    if (computed.has_error())
        return computed;

    remember(key, computed.value());
    if (persistent != nullptr)
    {
        // Fire and forget: a lost write is a later miss, never a wrong answer.
        // The cost is what eviction weighs against the entry's size.
        (void)persistent->put(sgl_emit_persistent_key(key), cc::make_pinned_data(encode_preprocessed(computed.value())),
                              {.compute_time_secs = compute_secs});
    }
    return computed;
}
