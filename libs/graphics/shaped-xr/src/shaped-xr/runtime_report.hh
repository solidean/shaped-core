#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <shaped-xr/fwd.hh>

/// One thing the runtime says about this headset, beyond listing an extension.
struct sxr::runtime_feature
{
    cc::string name;
    bool is_supported = false;
};

/// What the active runtime offers, for a person deciding what can be built on it.
/// An extension being listed does not mean the headset has it: `features` is what the system itself answers.
/// Neither promises a call succeeds, since a runtime may list what a setting or a permission then refuses.
struct sxr::runtime_report
{
    cc::string runtime_name;
    cc::string runtime_version;
    /// Empty where no headset is connected, and `features` is then empty too.
    cc::string system_name;
    /// Every extension the runtime lists, by name.
    cc::vector<cc::string> extensions;
    cc::vector<runtime_feature> features;
};

namespace sxr
{
/// Reports on the active runtime through an instance of its own, with no graphics binding and no session.
/// Fails where the build has no OpenXR loader or no runtime is active; a missing headset is a report, not a failure.
/// No `sxr::system` may exist meanwhile, since the loader holds one instance per process.
[[nodiscard]] cc::result<runtime_report> inspect_runtime();
} // namespace sxr
