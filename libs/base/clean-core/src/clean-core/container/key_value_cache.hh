#pragma once

#include <clean-core/common/hash.hh> // cc::make_hash_finalized
#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/fwd.hh>
#include <clean-core/thread/mutex.hh>

#include <memory>        // std::shared_ptr, std::make_shared
#include <unordered_map> // TODO: migrate to cc::map once clean-core's own hash map lands

/// A thread-safe, tiered get-or-create cache. Providers are queried front-to-back (fastest first);
/// the first hit backfills the faster tiers, a full miss runs the factory and writes every tier. The
/// tier interface is the extension seam for on-disk / networked caches; only an in-memory tier ships
/// today. Keys are hashed through cc's hashing, so cc::hash128 (and any cc-hashable key) works as-is.

namespace cc
{
/// One tier of a key_value_cache. Implementations are always called under the owning cache's lock, so
/// they need not be individually thread-safe.
template <class K, class V>
struct key_value_provider
{
    /// The cached value for key, or nullopt if this tier does not have it.
    [[nodiscard]] virtual cc::optional<V> try_get(K const& key) = 0;

    /// Stores or overwrites the value for key.
    virtual void set(K const& key, V const& value) = 0;

    /// Periodic maintenance (e.g. eviction), driven by the owning cache.
    virtual void apply_bookkeeping() = 0;

    virtual ~key_value_provider() = default;
};

namespace impl
{
/// std::unordered_map hasher routed through cc's finalized hashing — lets cc::hash128 and any
/// cc-hashable key be a map key without needing a std::hash specialization.
template <class K>
struct cc_key_hash
{
    [[nodiscard]] size_t operator()(K const& k) const { return size_t(cc::make_hash_finalized(k)); }
};
} // namespace impl

/// In-memory tier backed by std::unordered_map. Eviction is deliberately crude — apply_bookkeeping
/// clears the whole map once it exceeds max_entries; subclass for a smarter policy.
///
/// TODO: migrate std::unordered_map -> cc::map once clean-core's own hash map lands.
template <class K, class V, class Hash = impl::cc_key_hash<K>>
struct in_memory_key_value_provider final : key_value_provider<K, V>
{
    explicit in_memory_key_value_provider(cc::isize max_entries) : _max_entries(max_entries) {}

    [[nodiscard]] cc::optional<V> try_get(K const& key) override
    {
        auto const it = _map.find(key);
        if (it == _map.end())
            return cc::nullopt;
        return it->second;
    }

    void set(K const& key, V const& value) override { _map.insert_or_assign(key, value); }

    void apply_bookkeeping() override
    {
        if (cc::isize(_map.size()) > _max_entries)
            _map.clear();
    }

private:
    cc::isize _max_entries = 0;
    std::unordered_map<K, V, Hash> _map;
};

/// Thread-safe, layered key-value cache. All operations serialize under an internal cc::mutex.
template <class K, class V>
struct key_value_cache
{
    /// Adds a provider as the last (slowest) tier.
    void add_provider(std::shared_ptr<key_value_provider<K, V>> provider)
    {
        _state.lock([&](state& s) { s.providers.push_back(cc::move(provider)); });
    }

    /// Convenience: append a default in-memory tier holding up to max_entries entries.
    void add_default_in_memory_provider(cc::isize max_entries = 4096)
    {
        this->add_provider(std::make_shared<in_memory_key_value_provider<K, V>>(max_entries));
    }

    /// The cached value for key, or the result of factory() stored into every tier. The first tier to
    /// hit backfills all preceding tiers that missed.
    [[nodiscard]] V acquire(K const& key, cc::function_ref<V()> factory)
    {
        return _state.lock(
            [&](state& s) -> V
            {
                for (cc::isize i = 0; i < s.providers.size(); ++i)
                {
                    auto hit = s.providers[i]->try_get(key);
                    if (hit.has_value())
                    {
                        for (cc::isize j = 0; j < i; ++j)
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
} // namespace cc
