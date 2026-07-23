#pragma once

#include <babel-serializer/fwd.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/streams/stream.hh> // cc::read_stream
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>

// JSON reader (data/).
//
// A parsed document is a FLAT, read-once structure, not a tree of allocating nodes:
//   * all values live in one contiguous cc::vector<node> (root at index 0, preorder),
//   * child links live in one contiguous cc::vector<i32> (so random child access is O(1)),
//   * all string + key bytes live in one cc::string arena, already unescaped.
// It is cheap to traverse and query, deliberately awkward to mutate — mutation is not offered.
// Traverse through the lightweight non-owning `ref` handle; a `document` owns nothing per node.
//
// Parsing runs against a cc::read_stream's buffered window (ready_bytes / consume / flush) and copies
// whatever it keeps into the arena — it never buffers the whole input up front.
//
//   auto doc = babel::json::read(R"({"name": "shaped", "tags": [1, 2, 3]})").value();
//   auto root = doc.root();
//   auto name = root["name"].as_string();     // "shaped"
//   auto first = root["tags"][0].as_double();  // 1

namespace babel::impl
{
struct json_parser; // defined in json.cc; builds a document
}

namespace babel::json
{
/// The JSON value categories.
enum class node_kind : cc::u8
{
    null,
    boolean,
    number,
    string,
    array,
    object,
};

/// One parsed value in the document's flat node array. Not used directly — traverse via `ref`.
/// The payload fields are read according to `kind`; the unrelated ones are left at 0.
struct node
{
    node_kind kind = node_kind::null;
    bool boolean = false; // kind == boolean

    double number = 0; // kind == number

    // kind == string: [str_offset, str_offset + str_length) into document._text (already unescaped)
    i32 str_offset = 0;
    i32 str_length = 0;

    // kind == array / object: children are document._child_indices[first_child, first_child + child_count)
    i32 first_child = 0;
    i32 child_count = 0;

    // this node's key within its parent object; [key_offset, key_offset + key_length) into document._text.
    // empty (key_length == 0) for array elements and the root.
    i32 key_offset = 0;
    i32 key_length = 0;
};

/// A parsed JSON document: owns the flat node array, the child-index array and the string arena.
/// Move-only-cheap value type (three vectors). Obtain one from babel::json::read; traverse via root().
class document
{
    // access
public:
    /// The root value (invalid ref on an empty document, which read never produces on success).
    [[nodiscard]] ref root() const;

    /// Number of parsed nodes (values) in the document.
    [[nodiscard]] isize node_count() const { return _nodes.size(); }

    document() = default;

    // internals shared with ref + the parser
private:
    friend struct ref;
    friend struct babel::impl::json_parser; // the parser, defined in json.cc

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

/// A non-owning handle to one node in a document: {document*, node index}. Copyable and cheap.
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

    [[nodiscard]] bool is_null() const { return is_valid() && kind() == node_kind::null; }
    [[nodiscard]] bool is_bool() const { return is_valid() && kind() == node_kind::boolean; }
    [[nodiscard]] bool is_number() const { return is_valid() && kind() == node_kind::number; }
    [[nodiscard]] bool is_string() const { return is_valid() && kind() == node_kind::string; }
    [[nodiscard]] bool is_array() const { return is_valid() && kind() == node_kind::array; }
    [[nodiscard]] bool is_object() const { return is_valid() && kind() == node_kind::object; }

    // scalar access (kind-tolerant: returns the fallback when the kind does not match)
public:
    [[nodiscard]] double as_double(double fallback = 0) const { return is_number() ? _node().number : fallback; }
    [[nodiscard]] bool as_bool(bool fallback = false) const { return is_bool() ? _node().boolean : fallback; }
    [[nodiscard]] cc::string_view as_string(cc::string_view fallback = {}) const
    {
        return is_string() ? _doc->impl_slice(_node().str_offset, _node().str_length) : fallback;
    }

    /// This node's key within its parent object (empty for array elements / root).
    [[nodiscard]] cc::string_view key() const
    {
        return is_valid() ? _doc->impl_slice(_node().key_offset, _node().key_length) : cc::string_view();
    }

    // container access
public:
    /// Child count for arrays and objects; 0 for everything else.
    [[nodiscard]] isize size() const { return is_array() || is_object() ? isize(_node().child_count) : isize(0); }

    /// The i-th child by position (works for arrays and objects); invalid ref when out of range.
    /// For an object member, the returned ref's key() gives its key.
    [[nodiscard]] ref operator[](isize i) const
    {
        if (!(is_array() || is_object()))
            return ref();
        auto const& n = _node();
        if (i < 0 || i >= n.child_count)
            return ref();
        return ref(_doc, _doc->_child_indices[isize(n.first_child) + i]);
    }

    /// Object member value by key (first match wins); invalid ref when not an object / key absent.
    [[nodiscard]] ref operator[](cc::string_view key) const
    {
        if (!is_object())
            return ref();
        auto const& n = _node();
        for (auto i = isize(0); i < n.child_count; ++i)
        {
            auto const child_index = _doc->_child_indices[isize(n.first_child) + i];
            auto const& c = _doc->_nodes[isize(child_index)];
            if (_doc->impl_slice(c.key_offset, c.key_length) == key)
                return ref(_doc, child_index);
        }
        return ref();
    }

    /// True if this is an object with a member named `key`.
    [[nodiscard]] bool has(cc::string_view key) const { return (*this)[key].is_valid(); }

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

// reading
// -------------------------------------------------------------------------------------------------

/// Parse a complete JSON document from a stream. Trailing whitespace is allowed; trailing junk is an error.
/// Parses against the stream's buffered window; the resulting document owns everything it needs.
[[nodiscard]] cc::result<document> read(cc::read_stream& in);

/// Convenience: parse from an in-memory UTF-8 buffer (wraps a span_read_stream_adapter).
[[nodiscard]] cc::result<document> read(cc::string_view text);
[[nodiscard]] cc::result<document> read(cc::span<cc::byte const> bytes);
} // namespace babel::json
