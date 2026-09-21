#include <clean-core/common/log.hh>
#include <clean-core/platform/environment.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/context/cold_caches.hh>

sg::cold_caches sg::cold_caches_from_environment()
{
    auto const value = cc::environment_variable("SC_SG_COLD");
    return value.has_value() ? parse_cold_caches(value.value()) : cold_caches{};
}

sg::cold_caches sg::parse_cold_caches(cc::string_view text)
{
    auto out = cold_caches{};
    auto begin = isize(0);
    for (auto i = isize(0); i <= text.size(); ++i)
    {
        if (i < text.size() && text[i] != ',')
            continue;
        auto const name = text.subview({.start = begin, .end = i});
        begin = i + 1;

        if (name == "all")
            out = {.pipelines = true, .shaders = true};
        else if (name == "pipelines")
            out.pipelines = true;
        else if (name == "shaders")
            out.shaders = true;
        else if (!name.empty())
            CC_LOG_WARNING("SC_SG_COLD names '{}', which is none of 'pipelines', 'shaders' or 'all'", name);
    }
    return out;
}
