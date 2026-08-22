#include "suggest.hh"

#include <clean-core/container/vector.hh>

using namespace cc::primitive_defines;

namespace
{
using nx::isize;

/// A prefix or substring match is a much better guess than the edit distance alone suggests, so it is
/// scored as if it were one edit away regardless of how many characters are missing.
constexpr isize substring_score = 1;

isize score_of(cc::string_view token, cc::string_view candidate, isize budget)
{
    if (candidate.starts_with(token) || token.starts_with(candidate))
        return substring_score;

    if (candidate.contains(token))
        return substring_score + 1;

    return nx::impl::edit_distance(token, candidate, budget);
}
} // namespace

isize nx::impl::edit_distance(cc::string_view a, cc::string_view b, isize max)
{
    auto const over = max + 1;

    // Length alone can rule a candidate out before any work happens.
    if (a.size() - b.size() > max || b.size() - a.size() > max)
        return over;

    if (a.empty())
        return b.size() > max ? over : b.size();
    if (b.empty())
        return a.size() > max ? over : a.size();

    // Two rows rather than the full matrix: only the previous one is ever read.
    auto previous = cc::vector<isize>();
    auto current = cc::vector<isize>();
    previous.reserve(b.size() + 1);
    current.resize_to_defaulted(b.size() + 1);

    for (auto j = isize(0); j <= b.size(); ++j)
        previous.push_back(j);

    for (auto i = isize(1); i <= a.size(); ++i)
    {
        current[0] = i;
        auto row_best = current[0];

        for (auto j = isize(1); j <= b.size(); ++j)
        {
            auto const substitution = previous[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
            auto const deletion = previous[j] + 1;
            auto const insertion = current[j - 1] + 1;

            auto best = substitution;
            if (deletion < best)
                best = deletion;
            if (insertion < best)
                best = insertion;

            current[j] = best;
            if (best < row_best)
                row_best = best;
        }

        // Every later row is at least this large, so nothing below `max` can still be reached.
        if (row_best > max)
            return over;

        cc::swap(previous, current);
    }

    return previous[b.size()] > max ? over : previous[b.size()];
}

cc::string nx::impl::best_suggestion(cc::string_view token, cc::span<cc::string const> candidates)
{
    if (token.empty())
        return {};

    // One edit for something short, up to three for something long enough that a typo is easy to make.
    auto const budget = token.size() <= 2 ? isize(1) : (token.size() <= 6 ? isize(2) : isize(3));

    auto best = cc::string();
    auto best_score = budget + 1;

    for (auto const& candidate : candidates)
    {
        if (candidate.empty())
            continue;

        auto const score = score_of(token, candidate, budget);
        if (score < best_score)
        {
            best_score = score;
            best = candidate;
        }
    }

    return best;
}
