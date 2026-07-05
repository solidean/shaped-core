#include "alias.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/string/string_view.hh>

#include <algorithm> // std::sort: stable alias/fragment order for reproducible listings

namespace
{
// Callbacks registered via NX_TEST_SETUP. Function-local static so it is initialized on first use,
// independent of static-init order across translation units (mirrors get_static_test_registry).
cc::vector<void (*)(nx::setup&)>& setup_callbacks()
{
    static cc::vector<void (*)(nx::setup&)> callbacks;
    return callbacks;
}

// Lexicographic order of two section paths (element-wise, shorter-is-less on a common prefix).
bool section_path_less(cc::span<cc::string const> a, cc::span<cc::string const> b)
{
    auto const n = cc::min(a.size(), b.size());
    for (cc::isize i = 0; i < n; ++i)
        if (cc::string_view(a[i]) != cc::string_view(b[i]))
            return cc::string_view(a[i]) < cc::string_view(b[i]);
    return a.size() < b.size();
}
} // namespace

nx::test_declaration const* nx::setup::find_test(cc::string_view name) const
{
    for (auto const& decl : _registry->declarations)
        if (cc::string_view(decl.name) == name)
            return &decl;
    return nullptr;
}

void nx::setup::define_alias(cc::string name, cc::vector<alias_fragment> fragments, cc::source_location loc)
{
    _registry->add_alias(test_alias{
        .name = cc::move(name),
        .fragments = cc::move(fragments),
        .location = loc,
    });
}

void nx::impl::register_setup(void (*fn)(nx::setup&), cc::source_location)
{
    setup_callbacks().push_back(fn);
}

void nx::run_setup_callbacks(test_registry& registry)
{
    // Rebuild from scratch so a second call (or a re-run in the same process) is a no-op rather than a dupe.
    registry.aliases.clear();

    setup s(registry);
    for (auto const fn : setup_callbacks())
        fn(s);

    // Stable order: aliases by name, and each alias's fragments by driver name then section path. Listings
    // and expansion order then do not depend on static-init or callback registration order.
    std::sort(registry.aliases.begin(), registry.aliases.end(), [](test_alias const& a, test_alias const& b)
              { return cc::string_view(a.name) < cc::string_view(b.name); });

    for (auto& alias : registry.aliases)
        std::sort(alias.fragments.begin(), alias.fragments.end(),
                  [](alias_fragment const& a, alias_fragment const& b)
                  {
                      auto const an = a.driver != nullptr ? cc::string_view(a.driver->name) : cc::string_view();
                      auto const bn = b.driver != nullptr ? cc::string_view(b.driver->name) : cc::string_view();
                      if (an != bn)
                          return an < bn;
                      return section_path_less(a.section_path, b.section_path);
                  });
}
