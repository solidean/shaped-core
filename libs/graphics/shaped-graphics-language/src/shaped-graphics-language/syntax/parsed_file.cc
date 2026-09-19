#include "parsed_file.hh"

#include <clean-core/common/utility.hh>
#include <shaped-graphics-language/forms/form_parser.hh>
#include <shaped-graphics-language/groups/grouper.hh>
#include <shaped-graphics-language/lines/line_tree.hh>
#include <shaped-graphics-language/tokens/tokenizer.hh>

sgl::parsed_file sgl::parse(cc::string source)
{
    auto file = build_line_tree(cc::move(source));
    tokenize(file);
    group_tokens(file);
    parse_forms(file, default_keywords());
    return file;
}
