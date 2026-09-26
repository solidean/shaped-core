#include "features.hh"

#include <shaped-graphics-language/driver/unannotated_bindings.hh>

using namespace cc::primitive_defines;

cc::vector<lsp::inlay_hint> sgl_lsp::inlay_hints_of(analysis const& a, lsp::range visible, lsp::position_encoding e)
{
    auto const user = a.user_file();
    auto const text = a.text_of(user);
    auto const& index = a.index_of(user);
    auto const first = index.offset_of(text, visible.start, e);
    auto const last = index.offset_of(text, visible.end, e);

    auto out = cc::vector<lsp::inlay_hint>();
    for (auto const& b : sgl::unannotated_bindings(a.ast, a.module, user))
    {
        if (b.is_type_named || isize(b.name.end()) < first || isize(b.name.offset) > last)
            continue;
        auto const at = index.position_of(text, b.name.end(), e);
        auto const type = a.module.name_of(b.type);
        // the annotation as the spec writes one, `let x : float`, which accepting the hint inserts
        out.push_back({
            .position = at,
            .label = cc::string(": ") + type,
            .kind = lsp::inlay_hint_kind::type,
            .padding_left = true,
            .text_edits = {{.range = {.start = at, .end = at}, .new_text = cc::string(" : ") + type}},
        });
    }
    return out;
}
