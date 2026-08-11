#include "registry.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/container/vector.hh>
#include <rules/cpp-style/blessed-includes/blessed_includes.hh>
#include <rules/cpp-style/default-init-assignment/default_init_assignment.hh>
#include <rules/cpp-style/qualified-primitive/qualified_primitive.hh>
#include <rules/cpp-style/qualified-record-definition/qualified_record_definition.hh>
#include <rules/prose/no-flow-prose/no_flow_prose.hh>
#include <rules/prose/no-long-prose-line/no_long_prose_line.hh>

namespace scl
{
cc::span<rule const> all_rules()
{
    static cc::vector<rule> const rules = []
    {
        cc::vector<rule> v;
        v.push_back(blessed_includes_rule());
        v.push_back(default_init_assignment_rule());
        v.push_back(qualified_primitive_rule());
        v.push_back(qualified_record_definition_rule());
        v.push_back(no_flow_prose_rule());
        v.push_back(no_long_prose_line_rule());

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
