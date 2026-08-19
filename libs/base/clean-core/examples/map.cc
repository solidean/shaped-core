#include <clean-core/container/map.hh>
#include <clean-core/string/print.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>

EXAMPLE("clean-core/map")
{
    cc::map<cc::string, int> word_counts;

    // Heterogeneous lookup: a literal or a string_view probes a map keyed by string without materializing one.
    // A literal stays a char array here, and hashes as the bytes string_view would view — that is what makes the bare `word_counts["axis"]` work.
    cc::string_view const words[] = {"axis", "vector", "axis", "matrix", "axis", "vector"};
    for (auto const word : words)
        word_counts[word] += 1;

    cc::println("size: {}", word_counts.size());
    cc::println("axis:     {}", word_counts.get_or("axis", 0));
    cc::println("bivector: {}", word_counts.get_or("bivector", 0));
    cc::println("contains bivector: {}", word_counts.contains("bivector"));

    // get_ptr is the branchless-at-the-call-site read: one lookup, and absence is a null rather than an assert.
    if (auto const* const count = word_counts.get_ptr("matrix"))
        cc::println("matrix:   {}", *count);

    // An entry does the lookup once and reuses the probe for the insert.
    // The vacant path builds the key from that probe, so nothing is hashed or compared twice.
    auto entry = word_counts.entry("quaternion");
    if (!entry.exists())
        entry.emplace(0);
    entry.value() += 5;
    cc::println("quaternion: {}", word_counts.get("quaternion"));

    // Iteration order is arbitrary — deliberately, so nobody builds on the bucket layout.
    // The proxy hands out a mutable value reference, which is how you edit in place.
    for (auto [key, value] : word_counts)
        value *= 100;

    cc::println("axis after scaling: {}", word_counts.get("axis"));

    // erase reports whether anything went, so "remove it if it is there" needs no prior contains().
    cc::println("erased vector: {}", word_counts.erase("vector"));
    cc::println("erased vector again: {}", word_counts.erase("vector"));
}
