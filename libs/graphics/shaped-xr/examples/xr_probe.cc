// What the active OpenXR runtime offers for AR: run it once per runtime, with the headset connected and awake.
// No session starts, so nothing appears in the headset.

#include <clean-core/string/print.hh>
#include <nexus/test.hh>
#include <shaped-xr/runtime_report.hh>

EXAMPLE("shaped-xr/xr-probe")
{
    auto const report = sxr::inspect_runtime();
    if (report.has_error())
    {
        cc::eprintln("no report: {}", report.error().to_string());
        FAIL("xr-probe cannot run, as printed above");
        return;
    }
    auto const& r = report.value();
    cc::println("runtime: {} {}", r.runtime_name, r.runtime_version);
    cc::println("headset: {}",
                r.system_name.empty() ? cc::string_view("none connected") : cc::string_view(r.system_name));
    for (auto const& f : r.features)
        cc::println("  {} {}", f.is_supported ? "yes" : " no", f.name);
    cc::println("{} extensions:", r.extensions.size());
    for (auto const& e : r.extensions)
        cc::println("  {}", e);
}
