#pragma once

#include <clean-core/common/macros.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/record/domain_fwd.hh>
#include <clean-core/record/fwd.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/atomic.hh>

// cc::rec::domain — which part of the source a recording site belongs to, and the one word every site gates on.
//
// A domain is CONSTANT-INITIALIZED, so it works from the first instruction of static initialization onwards and no
// site ever pays for a lazy-init check.
// Registration into the process-wide list is a separate object with a dynamic constructor: a domain that has not been
// registered yet still records correctly, it is only invisible to configuration until its registrar runs.

namespace cc::rec::impl
{
struct domain_registrar;
} // namespace cc::rec::impl

/// A named part of the source, carrying the enable mask and the per-level policy for everything recorded under it.
///
/// One per library, normally — declared in the library's fwd.hh with CC_REC_DECLARE_DOMAIN, defined in one .cc with
/// CC_REC_DEFINE_DOMAIN, and never named again.
/// Every recording macro resolves its domain by unqualified lookup, so nothing downstream mentions one by name.
///
/// **The mask is read on the hot path and written from anywhere**, so both go through relaxed atomics: a
/// reconfiguration becomes visible promptly but is not ordered against anything, which is all a gate needs.
struct cc::rec::domain
{
    /// `name` must be a string literal, or otherwise outlive the process.
    constexpr explicit domain(char const* name) : _name(name) {}

    domain(domain const&) = delete;
    domain& operator=(domain const&) = delete;

    [[nodiscard]] char const* name() const { return _name; }

    // gating — the hot path
public:
    /// The whole mask, as one relaxed load.
    /// This is what a recording site tests against desc::enable_bit.
    [[nodiscard]] CC_FORCE_INLINE u32 enabled_mask() const { return _enabled_mask.load(cc::memory_order_relaxed); }

    [[nodiscard]] bool is_enabled(rec::level l) const { return (enabled_mask() & rec::enable_bit_of(l)) != 0; }
    [[nodiscard]] bool is_enabled(rec::category c) const { return (enabled_mask() & rec::enable_bit_of(c)) != 0; }

    // configuration
public:
    void set_enabled_mask(u32 mask) { _enabled_mask.store(mask, cc::memory_order_relaxed); }

    void set_enabled(rec::level l, bool on) { _set_bit(_enabled_mask, rec::enable_bit_of(l), on); }
    void set_enabled(rec::category c, bool on) { _set_bit(_enabled_mask, rec::enable_bit_of(c), on); }

    /// Whether a log message at `l` also captures a stack.
    /// Costs orders of magnitude more than the event itself, so the default is errors only.
    [[nodiscard]] bool captures_stacktrace(rec::level l) const
    {
        return (_stacktrace_levels.load(cc::memory_order_relaxed) & rec::enable_bit_of(l)) != 0;
    }
    void set_captures_stacktrace(rec::level l, bool on) { _set_bit(_stacktrace_levels, rec::enable_bit_of(l), on); }

    /// Whether a log message at `l` breaks into the debugger, for the "stop the moment this happens" workflow.
    [[nodiscard]] bool breaks_on(rec::level l) const
    {
        return (_break_levels.load(cc::memory_order_relaxed) & rec::enable_bit_of(l)) != 0;
    }
    void set_breaks_on(rec::level l, bool on) { _set_bit(_break_levels, rec::enable_bit_of(l), on); }

    // registry
public:
    /// The next domain in the process-wide list, or null.
    /// Use cc::rec::for_each_domain instead unless you are writing the traversal itself.
    [[nodiscard]] domain* registry_next() const { return _registry_next; }

    /// Whether this domain has been registered; false only for the window before its registrar's dynamic
    /// initialization runs, during which it records normally but cannot be found by name.
    [[nodiscard]] bool is_registered() const { return _is_registered.load(cc::memory_order_relaxed); }

private:
    friend struct cc::rec::impl::domain_registrar;

    static void _set_bit(cc::atomic<u32>& mask, u32 bit, bool on)
    {
        auto const old = mask.load(cc::memory_order_relaxed);
        mask.store(on ? (old | bit) : (old & ~bit), cc::memory_order_relaxed);
    }

    char const* _name = "";

    /// Every category, plus info and above.
    /// Trace and debug are opt-in, because a build that records them by default teaches everyone to turn logging off.
    cc::atomic<u32> _enabled_mask = rec::all_category_bits | rec::enable_bit_of(rec::level::info)
                                  | rec::enable_bit_of(rec::level::warning) | rec::enable_bit_of(rec::level::error);

    cc::atomic<u32> _stacktrace_levels = rec::enable_bit_of(rec::level::error);
    cc::atomic<u32> _break_levels = 0;

    cc::atomic<bool> _is_registered = false;
    domain* _registry_next = nullptr;
};

namespace cc::rec::impl
{
/// Pushes a domain onto the process-wide list from its own dynamic initialization.
/// Separate from the domain so the domain itself stays constant-initialized; CC_REC_DEFINE_DOMAIN builds one for you.
struct domain_registrar
{
    explicit domain_registrar(rec::domain& d);
};
} // namespace cc::rec::impl

namespace cc::rec
{
/// The domain the recording system uses for its own events — gaps, chunk acquisition, late events.
/// Silencing it hides how the recorder itself is doing, which is occasionally what you want and usually not.
extern domain g_system_domain;

/// Calls `f(domain&)` for every registered domain, in unspecified order.
/// Safe against domains registering concurrently: one registering during the walk is either seen or not.
void for_each_domain(cc::function_ref<void(rec::domain&)> f);

/// The registered domain called `name`, or null.
[[nodiscard]] rec::domain* find_domain(cc::string_view name);

/// Applies `mask` to every registered domain, and to every domain registered afterwards.
/// The follow-on part is what makes this usable from main() before a plugin's static initializers have run.
void set_all_domains_enabled_mask(u32 mask);
} // namespace cc::rec

/// Define a domain declared with CC_REC_DECLARE_DOMAIN; put this in exactly one .cc, in the same namespace.
/// `display_name` is what a listener prints and must outlive the process.
///
///   namespace sg { CC_REC_DEFINE_DOMAIN(g_rec_domain, "shaped-graphics"); }
#define CC_REC_DEFINE_DOMAIN(var_name, display_name) \
    ::cc::rec::domain var_name(display_name);        \
    ::cc::rec::impl::domain_registrar var_name##_cc_rec_registrar(var_name)
