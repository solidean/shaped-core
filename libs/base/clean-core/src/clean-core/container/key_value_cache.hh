#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/fwd.hh>
#include <clean-core/thread/mutex.hh>

// COST NOTE: the STL headers below reach MSVC's <xutility>, which pulls <immintrin.h>.
// That is the whole AVX-512 intrinsic surface — ~43 extra files, and most of this header's parse time.
// <memory>, <string>, <string_view>, <mutex>, <system_error>, <ranges> and <chrono> all reach it.
// <type_traits>, <utility> and <atomic> do not, and are cheap by comparison.
// So keeping one of the first group out of a widely-included header is worth real time.
// docs/notes/build-times.md has the measurement and the per-header table.
#include <memory> // std::shared_ptr, std::make_shared

/// A tiered get-or-create cache: key_value_cache over a stack of key_value_provider tiers.
/// The tier interface is the extension seam for on-disk / networked caches; only an in-memory tier ships today.

/// One tier of a key_value_cache, fastest first.
/// Implementations are always called under the owning cache's lock, so they need not be individually thread-safe.
template <class K, class V>
struct cc::key_value_provider
{
    /// The cached value for key, or nullopt if this tier does not have it.
    [[nodiscard]] virtual cc::optional<V> try_get(K const& key) = 0;

    /// Stores or overwrites the value for key.
    virtual void set(K const& key, V const& value) = 0;

    /// Periodic maintenance (e.g. eviction), driven by the owning cache.
    virtual void apply_bookkeeping() = 0;

    virtual ~key_value_provider() = default;
};

/// In-memory tier backed by cc::map.
/// Eviction is crude: apply_bookkeeping clears the whole map once it exceeds max_entries.
/// Subclass for a smarter policy.
template <class K, class V, class Hash>
struct cc::in_memory_key_value_provider final : key_value_provider<K, V>
{
    explicit in_memory_key_value_provider(isize max_entries) : _max_entries(max_entries) {}

    [[nodiscard]] cc::optional<V> try_get(K const& key) override
    {
        if (auto const* v = _map.get_ptr(key))
            return *v;
        return cc::nullopt;
    }

    void set(K const& key, V const& value) override { _map[key] = value; }

    void apply_bookkeeping() override
    {
        if (_map.size() > _max_entries)
            _map.clear();
    }

private:
    isize _max_entries = 0;
    cc::map<K, V, Hash> _map;
};

/// Thread-safe, layered key-value cache.
/// All operations serialize under an internal cc::mutex.
/// Keys are hashed through cc's hashing, so cc::hash128 and any cc-hashable key work as-is.
template <class K, class V>
struct cc::key_value_cache
{
    /// Adds a provider as the last (slowest) tier.
    void add_provider(std::shared_ptr<key_value_provider<K, V>> provider)
    {
        _state.lock([&](state& s) { s.providers.push_back(cc::move(provider)); });
    }

    /// Convenience: append a default in-memory tier holding up to max_entries entries.
    void add_default_in_memory_provider(isize max_entries = 4096)
    {
        this->add_provider(std::make_shared<in_memory_key_value_provider<K, V>>(max_entries));
    }

    /// The cached value for key, or the result of factory() stored into every tier.
    /// Tiers are queried front-to-back, and the first to hit backfills every faster tier that missed.
    [[nodiscard]] V acquire(K const& key, cc::function_ref<V()> factory)
    {
        return _state.lock(
            [&](state& s) -> V
            {
                for (isize i = 0; i < s.providers.size(); ++i)
                {
                    auto hit = s.providers[i]->try_get(key);
                    if (hit.has_value())
                    {
                        for (isize j = 0; j < i; ++j)
                            s.providers[j]->set(key, hit.value());
                        return cc::move(hit.value());
                    }
                }

                V value = factory();
                for (auto const& provider : s.providers)
                    provider->set(key, value);
                return value;
            });
    }

    /// Runs apply_bookkeeping on all providers (e.g. to trigger in-memory eviction).
    void apply_bookkeeping()
    {
        _state.lock(
            [](state& s)
            {
                for (auto const& provider : s.providers)
                    provider->apply_bookkeeping();
            });
    }

private:
    struct state
    {
        cc::vector<std::shared_ptr<key_value_provider<K, V>>> providers;
    };

    cc::mutex<state> _state;
};
