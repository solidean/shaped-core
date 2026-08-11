#include "directive.hh"

#include <clean-core/string/char_predicates.hh>

namespace scl
{
cc::string_view directive_word(cc::string_view text)
{
    auto i = isize(1); // past the '#'
    while (i < text.size() && cc::is_space(text[i]))
        ++i;
    auto const start = i;
    while (i < text.size() && !cc::is_space(text[i]))
        ++i;
    return text.subview({.start = start, .end = i});
}

cc::string_view include_target(cc::string_view text)
{
    auto const word = directive_word(text);
    if (word != "include")
        return {};

    auto i = isize(word.data() - text.data()) + word.size();
    while (i < text.size() && cc::is_space(text[i]))
        ++i;
    if (i >= text.size())
        return {};

    auto const closer = text[i] == '<' ? '>' : (text[i] == '"' ? '"' : char(0));
    if (closer == 0)
        return {}; // a macro spells the header, and this file cannot say which one

    auto const end = text.find(closer, i + 1);
    if (end < 0)
        return {};
    return text.subview({.start = i, .end = end + 1});
}
} // namespace scl
