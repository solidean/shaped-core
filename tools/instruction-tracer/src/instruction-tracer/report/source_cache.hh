#pragma once

#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <instruction-tracer/debug/trace_record.hh>

namespace itrace
{
/// Reads source files off disk to annotate traces, caching each file's lines.
///
/// A file that cannot be read is cached as empty and never retried — a build whose sources have
/// moved would otherwise re-stat once per instruction.
class source_cache
{
public:
    /// One 1-based line, trimmed of leading indentation and trailing whitespace. Empty when the
    /// file is unreadable or the line is out of range.
    cc::string_view line(cc::string_view path, u32 line_number);

private:
    cc::vector<cc::string> const& lines_of(cc::string_view path);

    cc::map<cc::string, cc::vector<cc::string>> _files;
};
} // namespace itrace
