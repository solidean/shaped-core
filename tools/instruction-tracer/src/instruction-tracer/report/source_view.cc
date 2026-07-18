#include "source_view.hh"

#include <clean-core/common/utility.hh> // cc::min, cc::max
#include <clean-core/container/map.hh>
#include <clean-core/container/set.hh>
#include <instruction-tracer/report/source_cache.hh>

#include <algorithm> // std::sort

namespace itrace
{
namespace
{
/// The touched lines of one file, in first-appearance file order.
struct touched_file
{
    cc::string path;
    cc::vector<u32> lines; // 1-based, may contain duplicates until compacted
};
} // namespace

source_view_model collect_source_view(trace const& t, source_cache& sources, u32 context)
{
    // Gather touched (file, line) pairs, keeping files in the order they first appear.
    cc::vector<touched_file> files;
    cc::map<cc::string, isize> index;
    for (auto const& insn : t.instructions)
    {
        if (insn.line == 0 || insn.file.empty())
            continue;

        auto entry = index.entry(insn.file);
        if (!entry.exists())
        {
            entry.emplace(files.size());
            files.push_back(touched_file{.path = insn.file});
        }
        files[entry.value()].lines.push_back(insn.line);
    }

    source_view_model model;
    for (auto& tf : files)
    {
        auto const count = sources.line_count(tf.path);
        if (count == 0) // unreadable source: nothing to show
            continue;

        // Sort + dedupe the touched lines (cc::vector has no resize/unique seam of its own).
        std::sort(tf.lines.begin(), tf.lines.end());
        cc::vector<u32> touched_lines;
        for (auto const l : tf.lines)
            if (touched_lines.empty() || touched_lines.back() != l)
                touched_lines.push_back(l);

        source_file_view view{.path = tf.path};
        for (auto const touched : touched_lines)
        {
            if (touched > count)
                continue;

            u32 const lo = touched > context ? touched - context : 1u;
            u32 const hi = cc::min(count, touched + context);

            // Merge into the last range when its window overlaps or directly abuts this one.
            if (!view.ranges.empty() && lo <= view.ranges.back().end + 1)
                view.ranges.back().end = cc::max(view.ranges.back().end, hi);
            else
                view.ranges.push_back(source_range{.start = lo, .end = hi});
        }

        // Fill each range's lines and mark which were executed.
        cc::set<u32> touched_set;
        for (auto const l : touched_lines)
            touched_set.insert(l);

        for (auto& range : view.ranges)
        {
            range.lines.reserve(range.end - range.start + 1);
            for (u32 n = range.start; n <= range.end; ++n)
                range.lines.push_back(source_view_line{
                    .number = n,
                    .text = cc::string(sources.raw_line(tf.path, n)),
                    .executed = touched_set.contains(n),
                });
        }

        if (!view.ranges.empty())
            model.files.push_back(cc::move(view));
    }

    return model;
}
} // namespace itrace
