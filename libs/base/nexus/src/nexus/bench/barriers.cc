#include "barriers.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/platform/system_info.hh>

using namespace cc::primitive_defines;

namespace
{
// Read-only after construction, so the sharing across threads is not a race.
// A function-local static rather than a namespace one: the allocation happens at the first call rather than at static
// init, and a binary that never evicts never pays the 64 MB.
cc::vector<u64> const& evict_buffer()
{
    static cc::vector<u64> const buffer
        = cc::vector<u64>::create_defaulted(nx::bench::default_evict_bytes() / isize(sizeof(u64)));
    return buffer;
}
} // namespace

void nx::bench::impl::observe(void const volatile* p)
{
    // Deliberately empty, and deliberately out of line.
    // The whole construct is the CALL: an optimizer that cannot see this body cannot prove the pointed-at object is
    // unread, so everything feeding it stays live.
    (void)p;
}

isize nx::bench::default_evict_bytes()
{
    static auto const bytes = []
    {
        constexpr auto k_cap = isize(1024) * 1024 * 1024;

        // The largest level any core class reports, which on a heterogeneous CPU is the P-cluster's.
        auto largest = isize(0);
        for (auto level = 1; level <= 4; ++level)
            if (auto const size = cc::get_system_info().largest_cache_bytes(level); size.has_value())
                largest = cc::max(largest, isize(size.value()));

        if (largest <= 0)
            return nx::bench::min_evict_bytes;

        return cc::clamp(largest * 2, nx::bench::min_evict_bytes, k_cap);
    }();
    return bytes;
}

void nx::bench::evict_data_caches(isize bytes)
{
    auto const& buffer = evict_buffer();
    auto const words = bytes == 0 ? buffer.size() : cc::clamp(bytes / isize(sizeof(u64)), isize(0), buffer.size());

    // One word per cache line is all an eviction needs, and touching every word instead would spend eight times the
    // bandwidth to displace exactly the same lines.
    constexpr isize words_per_line = 64 / isize(sizeof(u64));

    u64 acc = 0;
    for (isize i = 0; i < words; i += words_per_line)
        acc += buffer[i];

    // Without this the whole loop is dead and the function evicts nothing at all — which would fail silently, since
    // there is no result anyone checks.
    bench::sink(acc);
}
