#include <versioned-document-file/diagnostics.hh>

namespace vdoc::file
{
isize load_report::count_of(load_issue_kind kind) const
{
    isize n = 0;
    for (auto const& issue : issues)
        if (issue.kind == kind)
            ++n;
    return n;
}

load_issue const* load_report::find_first(load_issue_kind kind) const
{
    for (auto const& issue : issues)
        if (issue.kind == kind)
            return &issue;
    return nullptr;
}
} // namespace vdoc::file
