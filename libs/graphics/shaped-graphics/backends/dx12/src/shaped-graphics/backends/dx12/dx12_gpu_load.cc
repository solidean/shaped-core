#include <clean-core/platform/win32_sanitized.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/mutex.hh>
#include <pdh.h>
#include <pdhmsg.h>
#include <shaped-graphics/backends/dx12/dx12_context.hh>

// GPU busy time on Windows, from the "GPU Engine" performance counters.
//
// D3D12 has no utilization query at all, so this comes from the OS.
// The counters here are the same ones a task manager reads, which is why its number and this one agree.
//
// **Raw rather than formatted values.** `Running Time` is a cumulative 100 ns counter, and PDH will happily format it
// into a rate for you — but that rate is computed against PDH's own previous sample rather than the caller's, which
// would put the hidden baseline back that sg::gpu_load_sampler exists to avoid.
//
// **Per engine class, not per process.** An instance name looks like
//   pid_1234_luid_0x00000000_0x0000C4B7_phys_0_eng_1_engtype_3D
// so every process using the 3D engine has its own instance, and the device's 3D busy time is their sum.
// Grouping by `engtype` is what turns dozens of per-process instances into the handful of numbers a reader wants.

namespace sg::backend::dx12
{
namespace
{
/// The PDH query, opened once and reused.
///
/// Opening one costs milliseconds — it walks the performance registry — and a dashboard reads this every frame.
/// Guarded rather than thread-local: a PDH query handle is not thread-safe, and one shared handle behind a mutex is
/// simpler than one per thread that each pay the open.
struct pdh_query
{
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER counter = nullptr;
    bool tried = false;
};

cc::mutex<pdh_query> g_pdh;

/// The engine class out of a GPU Engine instance name, which is everything after the last "engtype_".
/// Empty when the name does not carry one, which is how a future instance format degrades rather than mis-grouping.
cc::string_view engine_type_of(cc::string_view instance)
{
    constexpr auto k_marker = cc::string_view("engtype_");

    auto const at = instance.rfind(k_marker);
    if (at < 0)
        return {};
    return instance.subview(at + k_marker.size());
}
} // namespace
} // namespace sg::backend::dx12

cc::result<sg::gpu_counters> sg::backend::dx12::dx12_context::read_gpu_counters() const
{
    auto opened = g_pdh.lock(
        [](pdh_query& p)
        {
            if (p.tried)
                return p.counter != nullptr;
            p.tried = true;

            if (::PdhOpenQueryW(nullptr, 0, &p.query) != ERROR_SUCCESS)
                return false;

            // The English name, so a localized Windows resolves the same counter.
            if (::PdhAddEnglishCounterW(p.query, L"\\GPU Engine(*)\\Running Time", 0, &p.counter) != ERROR_SUCCESS)
            {
                ::PdhCloseQuery(p.query);
                p.query = nullptr;
                p.counter = nullptr;
                return false;
            }
            return true;
        });

    if (!opened)
        return cc::error("the GPU Engine performance counters are unavailable on this machine");

    return g_pdh.lock(
        [](pdh_query& p) -> cc::result<sg::gpu_counters>
        {
            if (::PdhCollectQueryData(p.query) != ERROR_SUCCESS)
                return cc::error("PdhCollectQueryData failed");

            DWORD bytes = 0;
            DWORD count = 0;
            auto status = ::PdhGetRawCounterArrayW(p.counter, &bytes, &count, nullptr);
            if (status != PDH_MORE_DATA)
                return cc::error("PdhGetRawCounterArray did not report a size");

            auto storage = cc::vector<byte>();
            storage.resize_to_uninitialized(isize(bytes));

            auto* const items = reinterpret_cast<PDH_RAW_COUNTER_ITEM_W*>(storage.data());
            status = ::PdhGetRawCounterArrayW(p.counter, &bytes, &count, items);
            if (status != ERROR_SUCCESS)
                return cc::error("PdhGetRawCounterArray failed");

            auto out = sg::gpu_counters();

            for (DWORD i = 0; i < count; ++i)
            {
                if (items[i].RawValue.CStatus != PDH_CSTATUS_VALID_DATA
                    && items[i].RawValue.CStatus != PDH_CSTATUS_NEW_DATA)
                    continue;

                // The instance names are ASCII in practice, and only the engtype suffix is read.
                auto narrow = cc::string();
                for (auto const* w = items[i].szName; w != nullptr && *w != 0; ++w)
                    narrow += char(*w < 128 ? *w : '?');

                auto const engine = engine_type_of(narrow);
                if (engine.empty())
                    continue;

                // A cumulative counter has no negative value to report, so one is PDH answering with something
                // this cannot use — summing it would hand back a total a caller would draw as a negative bar.
                // Erroring is the whole response: the counter is re-read from scratch, so the next call is fine again.
                if (items[i].RawValue.FirstValue < 0)
                    return cc::error(cc::format("the GPU Engine counter for {} reported a negative running time", narrow));

                // 100 ns units, like every other Windows tick.
                auto const busy = f64(items[i].RawValue.FirstValue) * 1e-7;

                auto merged = false;
                for (auto& existing : out.engines)
                    if (cc::string_view(existing.engine) == engine)
                    {
                        existing.busy_secs += busy;
                        merged = true;
                        break;
                    }

                if (!merged)
                    out.engines.push_back({.engine = cc::string(engine), .busy_secs = busy});
            }

            if (out.engines.empty())
                return cc::error("the GPU Engine counters reported no engines");

            return out;
        });
}
