#include <clean-core/string/print.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>

EXAMPLE("clean-core/string")
{
    auto s = cc::string("shaped-core");
    cc::println("{} ({} bytes, small: {})", s, s.size(), s.is_small());

    // A string_view is free to take from a string; the other direction copies, and says so by allocating.
    cc::string_view const view = s;
    cc::println("starts_with('shaped'): {}", view.starts_with("shaped"));
    cc::println("find('-'): {}", view.find('-'));

    // subview borrows, substring owns — the name is the whole difference.
    cc::println("subview:   {}", s.subview({.offset = 0, .size = 6}));
    cc::println("substring: {}", s.substring({.start = 7, .end = 11}));

    s.append(" examples");
    s.replace_all('-', ' ');
    cc::println("after append + replace_all: {}", s);

    // Small strings live inline; the transition out of SSO is what the byte count above is measuring.
    auto grown = cc::string::create_filled(64, 'x');
    cc::println("64 x's small: {}", grown.is_small());
    grown.resize_down_to(4);
    grown.shrink_to_fit();
    cc::println("shrunk to '{}' small: {}", grown, grown.is_small());

    // data() is NOT null-terminated: reaching a C API is an explicit request, not an accident.
    cc::println("c_str_materialize: {}", s.c_str_materialize());
}
