#pragma once

#include <clean-core/record/log.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics/resource/pixel_format.hh>

namespace sr::impl
{
/// Logs why the pipeline `routine` compiled to write `format` did not build; `built` must have settled without a value.
/// What a routine says before `fail_init()`, which otherwise reads exactly like a member still compiling.
template <class T>
void log_build_failure(cc::shared_async<T> const& built, cc::string_view routine, sg::pixel_format format)
{
    auto const* const why = built->try_error();
    if (why == nullptr)
        CC_LOG_ERROR("{}: the pipeline writing pixel_format {} did not build", routine, int(format));
    else if (why->is_cancelled())
        CC_LOG_ERROR("{}: the pipeline writing pixel_format {} was cancelled", routine, int(format));
    else
        CC_LOG_ERROR("{}: the pipeline writing pixel_format {} did not build: {}", routine, int(format),
                     why->underlying().to_string());
}
} // namespace sr::impl
