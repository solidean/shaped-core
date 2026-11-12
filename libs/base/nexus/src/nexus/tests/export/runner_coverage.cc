// The runner report's covered functions: clang's profile counters, read and reset at each top-level test's boundaries.

#include "runner_report.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/mutex.hh>

using namespace cc::primitive_defines;

#if defined(__LLVM_INSTR_PROFILE_GENERATE)

#include <cstdint>
#include <profile/InstrProfData.inc> // its shared definitions, once, as the runtime includes it first

namespace
{
// The runtime's per-function record, defined from the header the same clang installs beside its runtime, as the
// runtime itself defines it; the layout follows the compiler, so a nexus built by another clang reads it right.
enum ValueKind
{
#define VALUE_PROF_KIND(Enumerator, Value, Descr) Enumerator = Value,
#include <profile/InstrProfData.inc>
};

using IntPtrT = void*;

struct alignas(INSTR_PROF_DATA_ALIGNMENT) profile_data
{
#define INSTR_PROF_DATA(Type, LLVMType, Name, Initializer) Type Name;
#include <profile/InstrProfData.inc>
};
} // namespace

// The profile runtime's sections and reset, linked into every binary built with -fprofile-instr-generate.
extern "C" profile_data const* __llvm_profile_begin_data(void);
extern "C" profile_data const* __llvm_profile_end_data(void);
extern "C" char* __llvm_profile_begin_counters(void);
extern "C" char* __llvm_profile_end_counters(void);
extern "C" void __llvm_profile_reset_counters(void);

namespace
{
struct coverage_state
{
    /// The context counting now: the running top-level test's declaration, 0 between tests.
    u64 current = 0;
    /// Per context, the profile records entered, in no order and possibly repeated, until the run ends.
    cc::map<u64, cc::vector<i32>> entered;
};

cc::mutex<coverage_state> g_coverage;

/// Read at every boundary, so a run that does not cover pays one load for it.
cc::atomic<bool> g_covers = false;

/// The counters of record `d`: since raw profile version 8 its counter pointer is relative to the record.
u64* counters_of(profile_data const& d)
{
    return reinterpret_cast<u64*>(const_cast<char*>(reinterpret_cast<char const*>(&d))
                                  + reinterpret_cast<intptr_t>(d.CounterPtr));
}

/// Files what ran since the last boundary under the current context, and zeroes the counters that moved.
///
/// A function ran when any of its counters is non-zero; its first is not always its entry count, so all of them are read.
/// Only a function that ran is zeroed: resetting the whole section would write all of it at every boundary.
void harvest(coverage_state& s)
{
    auto const* const begin = __llvm_profile_begin_data();
    auto const records = isize(__llvm_profile_end_data() - begin);
    auto const* const first_counter = reinterpret_cast<u64 const*>(__llvm_profile_begin_counters());
    auto const* const end_counter = reinterpret_cast<u64 const*>(__llvm_profile_end_counters());
    auto& entered = s.entered[s.current];
    for (isize i = 0; i < records; ++i)
    {
        // A linker may pad the section with zeroed records.
        auto const& d = begin[i];
        if (d.NumCounters == 0)
            continue;
        auto* const c = counters_of(d);
        if (c < first_counter || c + d.NumCounters > end_counter)
            continue;
        auto ran = false;
        for (u32 k = 0; k < d.NumCounters && !ran; ++k)
            ran = c[k] != 0;
        if (!ran)
            continue;
        entered.push_back(i32(i));
        for (u32 k = 0; k < d.NumCounters; ++k)
            c[k] = 0;
    }
}

void boundary(u64 next)
{
    if (!g_covers.load(cc::memory_order_acquire))
        return;
    g_coverage.lock(
        [&](coverage_state& s)
        {
            harvest(s);
            s.current = next;
        });
}
} // namespace

bool nx::build_counts_coverage()
{
    return true;
}

void nx::impl::begin_report_coverage()
{
    g_coverage.lock(
        [](coverage_state& s)
        {
            // What ran before, static setup above all, is no test's.
            s = coverage_state();
            harvest(s);
            __llvm_profile_reset_counters();
        });
    g_covers.store(true, cc::memory_order_release);
}

void nx::impl::note_coverage_begin(test_declaration const* slot)
{
    boundary(u64(reinterpret_cast<uintptr_t>(slot)));
}

void nx::impl::note_coverage_end()
{
    boundary(0);
}

cc::optional<nx::covered_functions> nx::impl::end_report_coverage()
{
    if (!g_covers.exchange(false, cc::memory_order_acq_rel))
        return cc::nullopt;
    auto out = covered_functions();
    g_coverage.lock(
        [&](coverage_state& s)
        {
            harvest(s);
            auto const* const begin = __llvm_profile_begin_data();
            auto const records = isize(__llvm_profile_end_data() - begin);
            for (isize i = 0; i < records; ++i)
                out.functions.push_back(begin[i].NameRef);
            for (auto&& [context, entered] : s.entered)
            {
                cc::sort(entered);
                auto& v = out.by_context[context];
                for (auto const i : entered)
                    if (v.empty() || v.back() != i)
                        v.push_back(i);
            }
            s = coverage_state();
        });
    return out;
}

#else

bool nx::build_counts_coverage()
{
    return false;
}

void nx::impl::begin_report_coverage()
{
}

void nx::impl::note_coverage_begin(test_declaration const*)
{
}

void nx::impl::note_coverage_end()
{
}

cc::optional<nx::covered_functions> nx::impl::end_report_coverage()
{
    return cc::nullopt;
}

#endif
