#include "registry.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/container/vector.hh>
#include <rules/cpp-style/blessed-includes/blessed_includes.hh>
#include <rules/cpp-style/blocking-wait/blocking_wait.hh>
#include <rules/cpp-style/co-await-temporary/co_await_temporary.hh>
#include <rules/cpp-style/default-init-assignment/default_init_assignment.hh>
#include <rules/cpp-style/qualified-primitive/qualified_primitive.hh>
#include <rules/cpp-style/qualified-type-definition/qualified_type_definition.hh>
#include <rules/prose/no-bare-todo/no_bare_todo.hh>
#include <rules/prose/no-flow-prose/no_flow_prose.hh>
#include <rules/prose/no-long-prose-line/no_long_prose_line.hh>
#include <rules/prose/no-stock-words/no_stock_words.hh>
#include <rules/prose/no-whole-point/no_whole_point.hh>
#include <rules/prose/no-why-it-matters/no_why_it_matters.hh>

namespace scl
{
cc::span<rule const> all_rules()
{
    static cc::vector<rule> const rules = []
    {
        cc::vector<rule> v;
        v.push_back(blessed_includes_rule());
        v.push_back(blocking_wait_rule());
        v.push_back(co_await_temporary_rule());
        v.push_back(default_init_assignment_rule());
        v.push_back(qualified_primitive_rule());
        v.push_back(qualified_type_definition_rule());
        v.push_back(no_bare_todo_rule());
        v.push_back(no_flow_prose_rule());
        v.push_back(no_long_prose_line_rule());
        v.push_back(no_stock_words_rule());
        v.push_back(no_whole_point_rule());
        v.push_back(no_why_it_matters_rule());

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
