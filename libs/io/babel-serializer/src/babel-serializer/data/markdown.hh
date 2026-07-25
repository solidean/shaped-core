#pragma once

#include <babel-serializer/fwd.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/streams/stream.hh> // cc::read_stream
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>

// Markdown reader (data/), block level only.
//
// A parsed document has the same FLAT shape as babel::json — one preorder cc::vector<node> (root at
// index 0), one cc::vector<i32> of child indices so a block's children are a contiguous range, and one
// cc::string arena holding every block's text. Traverse through the non-owning `ref` handle.
//
// This reader parses the BLOCK structure only: headings, fenced code, paragraphs, lists, block quotes,
// and thematic breaks. Inline spans (emphasis, links, code spans) are NOT parsed — a paragraph's or
// heading's text() is the raw source text, so `**bold**` comes back with its asterisks. See
// ../../../docs/structure.md for what else is deliberately out of v1.
//
// Every input is valid markdown — there is no such thing as a parse error here. The cc::result exists
// for I/O failure on the stream and for API consistency with the other readers.
//
//   auto doc = babel::markdown::read("# title\n\n```cpp x\ncode\n```\n").value();
//   auto h = doc.node_at(1);         // preorder: the heading
//   h.level();                       // 1
//   h.text();                        // "title"
//   doc.node_at(2).info();           // "cpp x"   (the fence info string, verbatim)

namespace babel::impl
{
struct markdown_parser; // defined in markdown.cc; builds a document
}

namespace babel::markdown
{
/// The block kinds this reader produces. Leaves carry text; containers carry children.
enum class node_kind : cc::u8
{
    document,       // the root; children are the top-level blocks
    heading,        // level 1..6; text is the heading content, `#` markers stripped
    paragraph,      // text is the raw lines joined by '\n'; inline spans are not parsed
    code_block,     // fenced; info is the fence info string, text is the code verbatim
    list,           // children are list_items
    list_item,      // children are blocks
    block_quote,    // children are blocks
    thematic_break, // `---` / `***` / `___`
};

/// One parsed block. Not used directly — traverse via `ref`.
/// The payload fields are read according to `kind`; the unrelated ones are left at 0.
struct node
{
    node_kind kind = node_kind::document;

    u8 level = 0;         // kind == heading: 1..6
    bool ordered = false; // kind == list: `1.` / `1)` rather than `-` / `*` / `+`

    i32 line = 1; // 1-based source line the block starts on

    // heading / paragraph / code_block: [text_offset, text_offset + text_length) into document._text
    i32 text_offset = 0;
    i32 text_length = 0;

    // kind == code_block: the fence info string, same arena
    i32 info_offset = 0;
    i32 info_length = 0;

    // children are document._child_indices[first_child, first_child + child_count)
    i32 first_child = 0;
    i32 child_count = 0;
};

/// A parsed markdown document: owns the flat node array, the child-index array and the text arena.
/// Move-only-cheap value type (three vectors). Obtain one from babel::markdown::read; traverse via root().
class document
{
    // access
public:
    /// The root document node (invalid ref on a default-constructed document; read always produces one).
    [[nodiscard]] ref root() const;

    /// The node at a preorder index — 0 is the root. Invalid ref when out of range.
    /// Iterating 0 .. node_count() visits every block in document order.
    [[nodiscard]] ref node_at(i32 index) const;

    /// Number of parsed blocks, including the root.
    [[nodiscard]] isize node_count() const { return _nodes.size(); }

    document() = default;

    // internals shared with ref + the parser
private:
    friend struct ref;
    friend struct babel::impl::markdown_parser; // the parser, defined in markdown.cc

    document(cc::vector<node> nodes, cc::vector<i32> child_indices, cc::string text)
      : _nodes(cc::move(nodes)), _child_indices(cc::move(child_indices)), _text(cc::move(text))
    {
    }

    [[nodiscard]] cc::string_view impl_slice(i32 offset, i32 length) const
    {
        return cc::string_view(_text.data() + offset, isize(length));
    }

    cc::vector<node> _nodes;
    cc::vector<i32> _child_indices;
    cc::string _text;
};

/// A non-owning handle to one block in a document: {document*, node index}. Copyable and cheap.
/// All accessors are kind-tolerant — a mismatched kind returns the fallback / an invalid ref rather than asserting.
struct ref
{
    // construction
public:
    ref() = default;
    ref(document const* doc, i32 index) : _doc(doc), _index(index) {}

    // validity + kind
public:
    [[nodiscard]] bool is_valid() const { return _doc != nullptr && _index >= 0; }

    [[nodiscard]] node_kind kind() const { return _node().kind; }

    [[nodiscard]] bool is_document() const { return is_valid() && kind() == node_kind::document; }
    [[nodiscard]] bool is_heading() const { return is_valid() && kind() == node_kind::heading; }
    [[nodiscard]] bool is_paragraph() const { return is_valid() && kind() == node_kind::paragraph; }
    [[nodiscard]] bool is_code_block() const { return is_valid() && kind() == node_kind::code_block; }
    [[nodiscard]] bool is_list() const { return is_valid() && kind() == node_kind::list; }
    [[nodiscard]] bool is_list_item() const { return is_valid() && kind() == node_kind::list_item; }
    [[nodiscard]] bool is_block_quote() const { return is_valid() && kind() == node_kind::block_quote; }
    [[nodiscard]] bool is_thematic_break() const { return is_valid() && kind() == node_kind::thematic_break; }

    // payload (kind-tolerant: returns the fallback when the kind does not carry it)
public:
    /// Heading level 1..6; 0 for every other kind.
    [[nodiscard]] i32 level() const { return is_heading() ? i32(_node().level) : 0; }

    /// Whether a list is ordered (`1.`); false for every other kind.
    [[nodiscard]] bool is_ordered() const { return is_list() && _node().ordered; }

    /// The 1-based source line the block starts on; 0 on an invalid ref.
    [[nodiscard]] i32 line() const { return is_valid() ? _node().line : 0; }

    /// Heading / paragraph / code_block text, as a view into the document arena. Empty for containers.
    /// Inline spans are not parsed: this is the raw source text.
    [[nodiscard]] cc::string_view text() const
    {
        return is_valid() ? _doc->impl_slice(_node().text_offset, _node().text_length) : cc::string_view();
    }

    /// A code block's fence info string (`cpp`, or anything the author wrote after the fence), verbatim
    /// and trimmed. Empty for a bare fence and for every other kind.
    [[nodiscard]] cc::string_view info() const
    {
        return is_code_block() ? _doc->impl_slice(_node().info_offset, _node().info_length) : cc::string_view();
    }

    // container access
public:
    /// Child block count; 0 for leaves.
    [[nodiscard]] isize size() const { return is_valid() ? isize(_node().child_count) : isize(0); }

    /// The i-th child block; invalid ref when out of range.
    [[nodiscard]] ref operator[](isize i) const
    {
        if (!is_valid())
            return ref();
        auto const& n = _node();
        if (i < 0 || i >= n.child_count)
            return ref();
        return ref(_doc, _doc->_child_indices[isize(n.first_child) + i]);
    }

    // implementation
private:
    [[nodiscard]] node const& _node() const { return _doc->_nodes[isize(_index)]; }

    document const* _doc = nullptr;
    i32 _index = -1;
};

inline ref document::root() const
{
    return _nodes.empty() ? ref() : ref(this, 0);
}

inline ref document::node_at(i32 index) const
{
    return index >= 0 && index < _nodes.size() ? ref(this, index) : ref();
}

// reading
// -------------------------------------------------------------------------------------------------

/// Parse markdown from a stream, one line at a time. Never fails on content — only on stream I/O.
/// CRLF and LF are both accepted; the resulting document owns everything it needs.
[[nodiscard]] cc::result<document> read(cc::read_stream& in);

/// Convenience: parse from an in-memory UTF-8 buffer (wraps a span_read_stream_adapter).
[[nodiscard]] cc::result<document> read(cc::string_view text);
[[nodiscard]] cc::result<document> read(cc::span<cc::byte const> bytes);
} // namespace babel::markdown
