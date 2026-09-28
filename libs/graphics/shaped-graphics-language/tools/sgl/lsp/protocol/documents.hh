#pragma once

#include "fwd.hh"
#include "text_index.hh"
#include "types.hh"

#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/memory/shared_ptr.hh>
#include <clean-core/string/string.hh>

/// One open document at one version; immutable once built, so a snapshot shares it rather than copying its text.
struct lsp::document
{
    cc::string uri;
    i32 version = 0;
    cc::string text;
    text_index index;
};

/// The open documents at one moment: what a request is answered against, however the documents change meanwhile.
/// Cheap to copy, since documents are shared; safe to read from any thread, since nothing in it changes.
class lsp::snapshot
{
public:
    /// The document named `uri` if it is open; null otherwise.
    [[nodiscard]] document const* find(cc::string_view uri) const;

    /// The same document, shared, so it can outlive this snapshot; empty if not open.
    [[nodiscard]] cc::shared_ptr<document> share(cc::string_view uri) const;

    /// Its version when open, -1 otherwise.
    [[nodiscard]] i32 version_of(cc::string_view uri) const;

    [[nodiscard]] cc::vector<cc::string> uris() const;

private:
    friend class workspace;
    cc::map<cc::string, cc::shared_ptr<document>> _documents;
};

/// The open documents as the client edits them.
/// Applies `didOpen`, `didChange` and `didClose` in the order they arrive, which LSP requires before any later request
/// is answered, and hands out snapshots.
/// Touched by the thread that drives the server only.
class lsp::workspace
{
public:
    void open(text_document_item const& item);

    /// Applies each change in order, a range edit against the text the previous one left; false when the document is
    /// not open, which a well-behaved client never sends.
    bool change(did_change_params const& params, position_encoding e);

    void close(cc::string_view uri);

    [[nodiscard]] lsp::snapshot snapshot() const { return _current; }

    /// The document as it is now, null when not open.
    [[nodiscard]] document const* find(cc::string_view uri) const { return _current.find(uri); }

private:
    lsp::snapshot _current;
};

namespace lsp
{
/// The file path a `file:` URI names, `%XX` decoded, its query and fragment ignored; nothing for any other scheme.
/// An authority makes it a UNC path: `file://server/share/x` is `\\server\share\x` on Windows and `//server/share/x` elsewhere.
/// On Windows `/c:/x` is the drive path `c:/x`, when the first segment is exactly a letter and a colon; elsewhere it stays.
[[nodiscard]] cc::optional<cc::string> path_of_uri(cc::string_view uri);

/// The `file:` URI of an absolute path, as VS Code writes it: `C:\a b` is `file:///c%3A/a%20b`.
[[nodiscard]] cc::string uri_of_path(cc::string_view path);
} // namespace lsp
