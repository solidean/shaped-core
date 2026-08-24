#pragma once

#include <babel-serializer/fwd.hh>
#include <clean-core/container/small_vector.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/streams/growing_stream.hh>
#include <clean-core/streams/stream.hh> // cc::read_stream / cc::write_stream
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/string/to_string.hh> // cc::float_notation

#include <type_traits>

// JSON reading and writing (data/).
//
// The two halves are separate APIs and share no type: the reader's `document` is read-once and offers no mutation,
// so a writer built on it would be the wrong shape for both.
// Reading parses into that flat document; writing is an imperative, stream-oriented writer with no document at all.
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

/// The JSON value categories.
enum class babel::json::node_kind : babel::u8
{
    null,
    boolean,
    number,
    string,
    array,
    object,
};

/// One parsed value in the document's flat node array.
/// Not used directly — traverse via `ref`.
/// The payload fields are read according to `kind`; the unrelated ones are left at 0.
struct babel::json::node
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
class babel::json::document
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
struct babel::json::ref
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

namespace babel::json
{

inline ref document::root() const
{
    return _nodes.empty() ? ref() : ref(this, 0);
}

// reading
// -------------------------------------------------------------------------------------------------

/// Parse a complete JSON document from a stream.
/// Trailing whitespace is allowed; trailing junk is an error.
/// `\uXXXX` escapes and surrogate pairs decode to UTF-8 into the arena, and an unpaired surrogate is an error.
/// Parses against the stream's buffered window; the resulting document owns everything it needs.
[[nodiscard]] cc::result<document> read(cc::read_stream& in);

/// Convenience: parse from an in-memory UTF-8 buffer (wraps a span_read_stream_adapter).
[[nodiscard]] cc::result<document> read(cc::string_view text);
[[nodiscard]] cc::result<document> read(cc::span<byte const> bytes);
} // namespace babel::json

// writing
// -------------------------------------------------------------------------------------------------
//
// The writer is imperative and streaming: there is no document to build up, values go straight into a
// cc::write_stream as they are written, and nothing but one small scope stack is held in between.
//
//   auto w = babel::json::writer(out, {.indent = 2});
//   {
//       auto obj = w.object();
//       obj.write("name", "shaped");
//       auto tags = obj.write_array("tags");
//       tags.write(1);
//       tags.write(2);
//   }
//   CC_ASSERT(w.finish().has_value());
//
// Scopes are RAII handles onto the writer, so an object closes when its handle dies, and the nesting in
// the source IS the nesting in the output.
// An object scope takes a key with every value and an array scope takes none, so the two cannot be confused.
//
// ERRORS ARE STICKY, and there is exactly one place to check them: finish().
// A failing stream write is recorded, every later write becomes a no-op, and finish() reports it.
//
// STRUCTURAL MISUSE GOES THERE TOO — writing through a scope whose child is still open, a key into an array, a scope
// closed out of order.
// It also asserts where assertions are on, so a debug run stops at the site with a stack rather than at finish().
// Both, because an assert alone would leave a release build silently emitting a document that is not valid JSON, and
// that is a worse outcome than a slower write.
//
// A writer that is never finished LOGS its error rather than losing it (CC_LOG_ERROR from the destructor).
//
// WHAT THE WRITER CHANGED ON THE WAY OUT is counted in report(), which finish() does not replace.
// A JSON document can be valid and still not be what the caller handed over — a NaN became null, an id past 2^53
// will round in any reader that parses into a double — and those are not errors, so they need somewhere else to go.
// The counters are flat on purpose: naming WHERE it happened would cost a key per open scope on every write.
//
// TWO THINGS THE WRITER DOES NOT DO, both on purpose:
//   * DUPLICATE KEYS ARE NOT DETECTED.
//     Tracking them would cost a set per object, on every write, forever.
//   * UTF-8 IS NEVER VALIDATED.
//     Well-formed input passes through byte-for-byte, and malformed input passes through unchanged rather than
//     being replaced or rejected.

/// What a writer does with a NaN or an infinity, neither of which JSON can represent.
enum class babel::json::non_finite_policy : babel::u8
{
    error,  // fail the write; emitting invalid JSON silently is worse than failing loudly
    null,   // emit null
    string, // emit "NaN" / "Infinity" / "-Infinity", which some parsers accept
};

/// What a writer does with an integer a JSON number cannot hold exactly, i.e. one past 2^53.
/// Every reader that parses numbers into a double — every JavaScript one, among others — rounds those.
enum class babel::json::large_integer_policy : babel::u8
{
    number, // emit the digits anyway: exact in the file, rounded by such a reader
    string, // emit "9007199254740993", which is what an opaque id wants, since precision is all it has
    error,  // fail the write
};

/// What the writer changed on the way out, none of which is an error.
/// Counts rather than a list of sites: a streaming writer knows the value it is writing, not the path to it, and
/// keeping that path would cost a key per open scope on every single write.
struct babel::json::write_report
{
    /// NaN / infinity values that hit the non_finite policy.
    i64 non_finite = 0;

    /// Integers past 2^53. What happened to them is large_integers policy's business; under the default `number`
    /// these are exactly the values a double-based reader will round.
    i64 large_integers = 0;

    /// Bytes that did not decode as UTF-8 while escape_non_ascii was on, and were passed through untouched.
    /// Always 0 otherwise, because nothing decodes them then.
    i64 undecodable_bytes = 0;

    /// True when the document says exactly what the caller wrote.
    [[nodiscard]] bool is_clean() const { return non_finite == 0 && large_integers == 0 && undecodable_bytes == 0; }
};

/// Writer tuning, passed by value.
/// The defaults produce compact, strictly-valid, UTF-8-transparent JSON.
struct babel::json::write_options
{
    /// Spaces per nesting level; 0 puts the whole document on one line.
    /// Indenting also puts a space after every ':'.
    i32 indent = 0;

    /// How a float or double is rendered.
    /// `shortest` is the shortest text that reads back as the same value.
    cc::float_notation floats = cc::float_notation::shortest;

    /// Digits for the fixed / scientific / general notations; a negative value means their own default of 6.
    i32 float_precision = -1;

    non_finite_policy non_finite = non_finite_policy::error;

    large_integer_policy large_integers = large_integer_policy::number;

    /// Escape every non-ASCII byte as \uXXXX instead of passing the UTF-8 through.
    bool escape_non_ascii = false;

    /// Escape '<' as \u003c, so the output can be embedded in an HTML <script> tag.
    /// Without it a string containing "</script>" or "<!--" ends the tag early; the parsed value is the same either way.
    bool escape_html = false;

    /// Newline-delimited JSON: several root values, one per line.
    /// This is what allows more than one root value at all, and it forces compact output.
    bool newline_delimited = false;
};

/// Whether a nested scope keeps the writer's indentation or puts itself on one line.
/// `compact` covers everything inside it too, which is what gives one-record-per-line output from an indented document.
/// It means nothing when the writer is not indenting at all.
enum class babel::json::layout : babel::u8
{
    inherit,
    compact,
};

namespace babel::json
{
/// The scalar types a writer accepts.
/// `char` is deliberately excluded: whether it is a number or a one-character string is not the writer's call.
template <class T>
concept writable_scalar
    = std::is_same_v<T, decltype(nullptr)> || std::is_same_v<T, bool> || std::is_floating_point_v<T>
   || (std::is_integral_v<T> && !std::is_same_v<std::remove_cv_t<T>, char>
       && !std::is_same_v<std::remove_cv_t<T>, char8_t> && !std::is_same_v<std::remove_cv_t<T>, char16_t>
       && !std::is_same_v<std::remove_cv_t<T>, char32_t> && !std::is_same_v<std::remove_cv_t<T>, wchar_t>)
   || std::is_convertible_v<T const&, cc::string_view>;
} // namespace babel::json

/// The one stateful object behind a JSON write: the output stream, the options, the scope stack, the sticky error.
/// Neither movable nor copyable, because every live scope points at it.
class babel::json::writer
{
    // construction
public:
    /// `out` must outlive the writer, which writes into it as values arrive.
    explicit writer(cc::write_stream& out, write_options opts = {}) : _out(&out), _opts(opts) {}

    /// Best-effort finish, dropping the result — so an early return cannot assert.
    /// A writer whose finish() was never checked therefore never reports an I/O failure.
    ~writer();

    writer(writer&&) = delete;
    writer& operator=(writer&&) = delete;
    writer(writer const&) = delete;
    writer& operator=(writer const&) = delete;

    // the root value
public:
    /// Opens the root object / array.
    /// The handle must be destroyed before finish().
    [[nodiscard]] object_writer object(layout l = layout::inherit);
    [[nodiscard]] array_writer array(layout l = layout::inherit);

    /// A bare scalar as the whole document, which is valid JSON on its own.
    template <class T>
        requires writable_scalar<T>
    void write(T const& value)
    {
        this->impl_begin_root();
        this->impl_value(value);
    }

    // finishing
public:
    /// Closes any scope still open, drains the stream, and reports the first error the writer hit.
    /// Idempotent, so calling it and then letting the writer die is fine.
    [[nodiscard]] cc::result<cc::unit> finish();

    /// True once a write has failed; every write after that is a no-op.
    [[nodiscard]] bool has_error() const { return !_error.is_empty(); }

    /// What the writer changed on the way out, so far.
    /// Readable at any point, and finish() does not clear it.
    [[nodiscard]] write_report report() const { return _report; }

    // internals shared with the scope handles
private:
    friend struct object_writer;
    friend struct array_writer;

    /// Emits the separator, newline and indentation before a value, plus the key when inside an object.
    /// `depth` is the calling scope's, which is what catches a write through a scope whose child is still open.
    /// False means the write must not proceed.
    [[nodiscard]] bool impl_begin_member(cc::string_view key, i32 depth);
    [[nodiscard]] bool impl_begin_element(i32 depth);
    void impl_begin_root();

    /// Structural misuse: records it and reports whether the caller may proceed.
    /// The check itself is a compare the caller already has the operands for; the failure path is out of line.
    bool impl_expect(bool ok, char const* what)
    {
        if (ok) [[likely]]
            return true;
        this->impl_misuse(what);
        return false;
    }
    void impl_misuse(char const* what);

    void impl_open(bool is_object, layout l);
    void impl_close(i32 depth);

    void impl_null();
    void impl_bool(bool v);
    void impl_i64(i64 v);
    void impl_u64(u64 v);
    void impl_integer(cc::string_view digits, u64 magnitude);
    void impl_double(double v, cc::float_notation notation, i32 precision);
    void impl_float(float v, cc::float_notation notation, i32 precision);
    void impl_string(cc::string_view v, bool escape_non_ascii);
    void impl_raw(cc::string_view fragment);

    template <class T>
    void impl_value(T const& v)
    {
        if constexpr (std::is_same_v<T, decltype(nullptr)>)
            this->impl_null();
        else if constexpr (std::is_same_v<T, bool>)
            this->impl_bool(v);
        else if constexpr (std::is_same_v<T, float>)
            this->impl_float(v, _opts.floats, _opts.float_precision);
        else if constexpr (std::is_floating_point_v<T>)
            this->impl_double(double(v), _opts.floats, _opts.float_precision);
        else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>)
            this->impl_i64(i64(v));
        else if constexpr (std::is_integral_v<T>)
            this->impl_u64(u64(v));
        else
            this->impl_string(cc::string_view(v), _opts.escape_non_ascii);
    }

    void impl_emit(cc::string_view text);
    void impl_indent();
    void impl_fail(cc::string message);

    [[nodiscard]] i32 impl_depth() const { return i32(_stack.size()); }

    struct level
    {
        bool is_object = false;
        bool has_any = false;
    };

    cc::write_stream* _out = nullptr;
    write_options _opts;
    cc::small_vector<level, 16> _stack;
    cc::any_error _error;
    write_report _report;
    i32 _root_count = 0;

    /// Depth of the outermost open compact scope, or 0 when none is open — everything below it is compact too.
    i32 _compact_depth = 0;
    bool _finished = false;
};

/// A live object scope: every value takes a key.
/// Move-only, and the parent scope must not be written to while this one is alive.
struct babel::json::object_writer
{
    object_writer(object_writer&& rhs) noexcept : _w(rhs._w), _depth(rhs._depth) { rhs._w = nullptr; }
    object_writer& operator=(object_writer&&) = delete;
    object_writer(object_writer const&) = delete;
    object_writer& operator=(object_writer const&) = delete;
    ~object_writer();

    template <class T>
        requires writable_scalar<T>
    void write(cc::string_view key, T const& value)
    {
        if (_w->impl_begin_member(key, _depth))
            _w->impl_value(value);
    }

    /// Renders this one value with a notation of its own, leaving the writer's default alone.
    void write(cc::string_view key, double value, cc::float_notation notation, i32 precision = -1)
    {
        if (_w->impl_begin_member(key, _depth))
            _w->impl_double(value, notation, precision);
    }
    void write(cc::string_view key, float value, cc::float_notation notation, i32 precision = -1)
    {
        if (_w->impl_begin_member(key, _depth))
            _w->impl_float(value, notation, precision);
    }

    /// Like write, but escapes every non-ASCII byte as \uXXXX.
    void write_ascii(cc::string_view key, cc::string_view value)
    {
        if (_w->impl_begin_member(key, _depth))
            _w->impl_string(value, true);
    }

    /// Emits `fragment` verbatim as the value — already-serialized JSON, or a number rendered elsewhere.
    /// Trusted: it is never parsed, escaped or checked, not even in debug.
    void write_raw(cc::string_view key, cc::string_view fragment)
    {
        if (_w->impl_begin_member(key, _depth))
            _w->impl_raw(fragment);
    }

    [[nodiscard]] object_writer write_object(cc::string_view key, layout l = layout::inherit);
    [[nodiscard]] array_writer write_array(cc::string_view key, layout l = layout::inherit);

private:
    friend class writer;
    friend struct array_writer;
    object_writer(writer* w, i32 depth) : _w(w), _depth(depth) {}

    writer* _w = nullptr;
    i32 _depth = 0;
};

/// A live array scope: the same set of writes, without the keys.
struct babel::json::array_writer
{
    array_writer(array_writer&& rhs) noexcept : _w(rhs._w), _depth(rhs._depth) { rhs._w = nullptr; }
    array_writer& operator=(array_writer&&) = delete;
    array_writer(array_writer const&) = delete;
    array_writer& operator=(array_writer const&) = delete;
    ~array_writer();

    template <class T>
        requires writable_scalar<T>
    void write(T const& value)
    {
        if (_w->impl_begin_element(_depth))
            _w->impl_value(value);
    }

    /// Renders this one value with a notation of its own, leaving the writer's default alone.
    void write(double value, cc::float_notation notation, i32 precision = -1)
    {
        if (_w->impl_begin_element(_depth))
            _w->impl_double(value, notation, precision);
    }
    void write(float value, cc::float_notation notation, i32 precision = -1)
    {
        if (_w->impl_begin_element(_depth))
            _w->impl_float(value, notation, precision);
    }

    /// Like write, but escapes every non-ASCII byte as \uXXXX.
    void write_ascii(cc::string_view value)
    {
        if (_w->impl_begin_element(_depth))
            _w->impl_string(value, true);
    }

    /// Emits `fragment` verbatim as the element; see object_writer::write_raw.
    void write_raw(cc::string_view fragment)
    {
        if (_w->impl_begin_element(_depth))
            _w->impl_raw(fragment);
    }

    [[nodiscard]] object_writer write_object(layout l = layout::inherit);
    [[nodiscard]] array_writer write_array(layout l = layout::inherit);

private:
    friend class writer;
    friend struct object_writer;
    array_writer(writer* w, i32 depth) : _w(w), _depth(depth) {}

    writer* _w = nullptr;
    i32 _depth = 0;
};

/// A writer over a cc::string it owns — the "give me the JSON as text" shape.
/// finish() hands the string over with no copy, because the writer wrote into its storage all along.
class babel::json::string_writer
{
public:
    explicit string_writer(write_options opts = {}) : _stream(_buffer.stream()), _writer(_stream, opts) {}

    string_writer(string_writer&&) = delete;
    string_writer& operator=(string_writer&&) = delete;

    [[nodiscard]] object_writer object(layout l = layout::inherit) { return _writer.object(l); }
    [[nodiscard]] array_writer array(layout l = layout::inherit) { return _writer.array(l); }

    template <class T>
        requires writable_scalar<T>
    void write(T const& value)
    {
        _writer.write(value);
    }

    /// Finishes the write and moves the text out.
    /// Every open scope must be destroyed first.
    [[nodiscard]] cc::result<cc::string> finish();

    /// What the writer changed on the way out; see write_report.
    [[nodiscard]] write_report report() const { return _writer.report(); }

private:
    cc::string_write_stream_adapter _buffer;
    cc::write_stream _stream;
    writer _writer;
};
