#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-linter/fwd.hh>
#include <shaped-linter/lex/source_buffer.hh>
#include <shaped-linter/lex/source_span.hh>

namespace scl
{
/// Owns every source_buffer, assigns file_ids (the buffer's index), and resolves spans for reporting.
/// Buffers are boxed (cc::unique_ptr) so a returned `source_buffer const&` stays valid as more files
/// are added — the vector may grow, but the boxed buffers do not move.
struct source_manager
{
    /// Register in-memory text (tests, snippets). No file IO.
    source_buffer const& add_from_text(cc::string text, cc::string_view path);

    /// Read a file from disk and register it. Fails if the file cannot be opened/read.
    cc::result<source_buffer const*> add_from_file(cc::string_view path);

    /// The buffer for a file_id (which is its registration index).
    source_buffer const& buffer(u32 file_id) const;

    cc::string_view span_text(source_span s) const;

    /// Resolve a span's start to a path + 1-based line/column, for a finding.
    resolved_location resolve(source_span s) const;

private:
    cc::vector<cc::unique_ptr<source_buffer>> _buffers; // file_id == index
};
} // namespace scl
