#include "../module_dirs.hh"
#include "features.hh"

#include <shaped-graphics-language/ast/build.hh>
#include <shaped-graphics-language/driver/outline.hh>
#include <shaped-graphics-language/syntax/parsed_file.hh>

using namespace cc::primitive_defines;

namespace
{
[[nodiscard]] lsp::symbol_kind kind_of(sgl::outline_kind k)
{
    using s = lsp::symbol_kind;
    switch (k)
    {
    case sgl::outline_kind::function:
        return s::function;
    case sgl::outline_kind::method:
        return s::method;
    case sgl::outline_kind::property:
        return s::property;
    case sgl::outline_kind::structure:
        return s::struct_;
    case sgl::outline_kind::enumeration:
        return s::enum_;
    case sgl::outline_kind::enum_case:
        return s::enum_member;
    case sgl::outline_kind::field:
        return s::field;
    case sgl::outline_kind::binding:
        return s::namespace_;
    case sgl::outline_kind::constant:
        return s::constant;
    case sgl::outline_kind::sampler:
        return s::variable;
    case sgl::outline_kind::pipeline:
        return s::object;
    case sgl::outline_kind::type:
        return s::type_parameter;
    case sgl::outline_kind::test:
        return s::event;
    case sgl::outline_kind::module:
        return s::module;
    }
    return s::function;
}

/// Spans of one text as LSP ranges.
struct ranger
{
    cc::string_view text;
    lsp::text_index const& index;
    lsp::position_encoding e;

    [[nodiscard]] lsp::range operator()(sgl::source_span where) const
    {
        return {.start = index.position_of(text, where.offset, e), .end = index.position_of(text, where.end(), e)};
    }
};

[[nodiscard]] lsp::document_symbol symbol_of(sgl::outline_entry const& entry, ranger const& range)
{
    auto out = lsp::document_symbol{.name = entry.name,
                                    .detail = entry.detail,
                                    .kind = kind_of(entry.kind),
                                    .range = range(entry.where),
                                    .selection_range = range(entry.name_span)};
    for (auto const& child : entry.children)
        out.children.push_back(symbol_of(child, range));
    return out;
}

/// Whether `name` holds `query`'s characters in order, ignoring ASCII case.
[[nodiscard]] bool matches(cc::string_view name, cc::string_view query)
{
    auto const lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
    auto at = isize(0);
    for (auto const c : name)
        if (at < query.size() && lower(c) == lower(query[at]))
            ++at;
    return at == query.size();
}

void collect(cc::vector<lsp::symbol_information>& out,
             cc::span<sgl::outline_entry const> entries,
             cc::string_view container,
             cc::string_view uri,
             ranger const& range,
             cc::string_view query)
{
    for (auto const& entry : entries)
    {
        if (matches(entry.name, query))
            out.push_back({.name = entry.name,
                           .kind = kind_of(entry.kind),
                           .location = {.uri = cc::string(uri), .range = range(entry.name_span)},
                           .container_name = cc::string(container)});
        collect(out, entry.children, entry.name, uri, range, query);
    }
}
} // namespace

cc::vector<lsp::document_symbol> sgl_lsp::document_symbols_of(analysis const& a, lsp::position_encoding e)
{
    auto const user = a.user_file();
    auto const range = ranger{.text = a.text_of(user), .index = a.index_of(user), .e = e};
    auto out = cc::vector<lsp::document_symbol>();
    for (auto const& entry : sgl::outline_of(a.file, a.ast))
        out.push_back(symbol_of(entry, range));
    return out;
}

cc::vector<lsp::folding_range> sgl_lsp::folding_ranges_of(analysis const& a)
{
    auto out = cc::vector<lsp::folding_range>();
    for (auto const& f : sgl::folds_of(a.file))
        out.push_back({.start_line = i32(f.first_line), .end_line = i32(f.last_line), .is_comment = f.is_comment});
    return out;
}

cc::vector<lsp::symbol_information> sgl_lsp::workspace_symbols_of(lsp::snapshot const& open,
                                                                  cc::string_view query,
                                                                  lsp::position_encoding e)
{
    auto out = cc::vector<lsp::symbol_information>();
    auto const add = [&](cc::string_view uri, cc::string_view text)
    {
        auto const file = sgl::parse(cc::string(text));
        auto const ast = sgl::ast::build(file);
        auto const index = lsp::text_index(text);
        collect(out, sgl::outline_of(file, ast), {}, uri, ranger{.text = text, .index = index, .e = e}, query);
    };
    auto const uris = open.uris();
    for (auto const& uri : uris)
        if (auto const* doc = open.find(uri); doc != nullptr)
            add(uri, doc->text);
    // a module file open in the editor was added as its open text
    if (auto const library = sgl_tool::read_module_dirs(module_dirs()); library.has_value())
        for (auto i = isize(0); i < library.value().names.size(); ++i)
        {
            auto const uri = lsp::uri_of_path(library.value().names[i]);
            if (open.find(uri) == nullptr)
                add(uri, library.value().sources[i]);
        }
    return out;
}
