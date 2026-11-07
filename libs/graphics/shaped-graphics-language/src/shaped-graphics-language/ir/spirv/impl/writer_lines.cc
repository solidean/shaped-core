#include "writer.hh"

using namespace sgl;
using namespace sgl::ir;
using namespace sgl::ir::spirv::impl;

namespace
{
/// The most text one `OpSource` or `OpSourceContinued` holds, well under an instruction's 65535 words.
constexpr isize k_source_chunk = 60000 * 4;
} // namespace

void writer::line_before(u32 index)
{
    if (_p.lines.is_empty())
        return;
    // SPIR-V has no inlining in its core lines, so a line names the innermost place, where the operation stands
    auto const place = _p.lines.place_of(_p.instructions[index].statement);
    if (place.file < 0 || place.file >= i32(_p.lines.names.size()))
        return;
    if (place.file == _line_file && place.line == _line)
        return;
    _line_file = place.file;
    _line = place.line;
    emit(_code, spv::Line, {u32(place.file), u32(place.line), 0u});
    _line_files.push_back(_code.size() - 3);
}

void writer::declare_files()
{
    if (_line_files.empty())
        return;
    auto ids = cc::vector<u32>::create_filled(_p.lines.names.size(), 0u);
    for (auto const at : _line_files)
    {
        auto const f = _code[at];
        if (ids[f] == 0)
        {
            ids[f] = _bound++;
            auto const name = cc::string_view(_p.lines.names[f]);
            head(_sources, spv::String, 2 + string_words(name));
            _sources.push_back(ids[f]);
            string_into(_sources, name);

            // the text of the file, so a debugger shows it beside the line without opening it
            constexpr u32 k_language_unknown = 0;
            auto const text = cc::string_view(_p.lines.texts[f]);
            auto const first = text.subview({.offset = 0, .size = cc::min(text.size(), k_source_chunk)});
            head(_sources, spv::Source, 4 + string_words(first));
            _sources.push_back(k_language_unknown);
            _sources.push_back(0);
            _sources.push_back(ids[f]);
            string_into(_sources, first);
            for (auto from = first.size(); from < text.size(); from += k_source_chunk)
            {
                auto const piece = text.subview({.offset = from, .size = cc::min(text.size() - from, k_source_chunk)});
                head(_sources, spv::SourceContinued, 1 + string_words(piece));
                string_into(_sources, piece);
            }
        }
        _code[at] = ids[f];
    }
}
