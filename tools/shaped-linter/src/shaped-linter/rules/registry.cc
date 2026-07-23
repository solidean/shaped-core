#include "registry.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/container/vector.hh>
#include <shaped-linter/rules/member_default_init_assignment.hh>

namespace scl
{
cc::span<rule const> all_rules()
{
    static cc::vector<rule> const rules = []
    {
        cc::vector<rule> v;
        v.push_back(member_default_init_assignment_rule());

        // A rationale is mandatory — the reporter leads every group with it, mirroring the gate culture.
        for (auto const& r : v)
        {
            CC_ASSERT(!r.id.empty(), "every rule must have an id");
            CC_ASSERT(!r.rationale.empty(), "every rule must carry a rationale");
        }
        return v;
    }();
    return rules;
}
} // namespace scl
