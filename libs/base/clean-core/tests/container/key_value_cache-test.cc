#include <clean-core/container/key_value_cache.hh>
#include <nexus/test.hh>

#include <memory>

TEST("key_value_cache - factory runs once per key")
{
    cc::key_value_cache<int, int> cache;
    cache.add_default_in_memory_provider();

    int factory_calls = 0;
    auto make_for = [&](int k)
    {
        return cache.acquire(k,
                             [&]
                             {
                                 ++factory_calls;
                                 return k * 10;
                             });
    };

    CHECK(make_for(1) == 10);
    CHECK(make_for(1) == 10); // hit, no new factory call
    CHECK(make_for(2) == 20);
    CHECK(factory_calls == 2);
}

TEST("key_value_cache - first-hit backfills faster tiers")
{
    auto fast = std::make_shared<cc::in_memory_key_value_provider<int, int>>(64);
    auto slow = std::make_shared<cc::in_memory_key_value_provider<int, int>>(64);

    // pre-populate only the slow tier
    slow->set(7, 700);

    cc::key_value_cache<int, int> cache;
    cache.add_provider(fast); // fastest (front)
    cache.add_provider(slow);

    CHECK(!fast->try_get(7).has_value());

    int factory_calls = 0;
    int v = cache.acquire(7,
                          [&]
                          {
                              ++factory_calls;
                              return -1;
                          });

    CHECK(v == 700);
    CHECK(factory_calls == 0);           // slow tier satisfied it
    CHECK(fast->try_get(7).has_value()); // backfilled
    CHECK(fast->try_get(7).value() == 700);
}

TEST("key_value_cache - miss writes every tier")
{
    auto fast = std::make_shared<cc::in_memory_key_value_provider<int, int>>(64);
    auto slow = std::make_shared<cc::in_memory_key_value_provider<int, int>>(64);

    cc::key_value_cache<int, int> cache;
    cache.add_provider(fast);
    cache.add_provider(slow);

    cache.acquire(3, [] { return 33; });

    CHECK(fast->try_get(3).value() == 33);
    CHECK(slow->try_get(3).value() == 33);
}

TEST("key_value_cache - bookkeeping evicts past the limit")
{
    auto tier = std::make_shared<cc::in_memory_key_value_provider<int, int>>(/*max_entries=*/1);

    cc::key_value_cache<int, int> cache;
    cache.add_provider(tier);

    int factory_calls = 0;
    cache.acquire(1,
                  [&]
                  {
                      ++factory_calls;
                      return 1;
                  });
    cache.acquire(2,
                  [&]
                  {
                      ++factory_calls;
                      return 2;
                  }); // now 2 entries > max 1
    CHECK(factory_calls == 2);

    cache.apply_bookkeeping(); // clears the whole map

    // both keys must be recomputed after eviction
    cache.acquire(1,
                  [&]
                  {
                      ++factory_calls;
                      return 1;
                  });
    CHECK(factory_calls == 3);
}
