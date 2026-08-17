#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <versioned-document/op_graph.hh>

/// The shared DAG fixtures every materialization test measures itself against.
///
/// The brute-force oracle lives here rather than in one test file because two suites now check against it: the plain
/// pass in op_graph-test.cc, and the snapshot cache in snapshot_cache-test.cc.
/// A second copy is how the two would drift.

namespace vdoc_test
{
using namespace cc::primitive_defines;

/// One property write, as a test spells it: a path and an integer.
/// NOT `write`: that collides with POSIX ::write(2) wherever a test says `using namespace vdoc_test`.
struct property_write;

/// One generated DAG, with the head sets and paths worth asking it about.
struct corpus_case;
} // namespace vdoc_test

struct vdoc_test::property_write
{
    vdoc::property_path path;
    i64 value;

    /// Withdraws the path instead of writing it; `value` is then unused.
    bool abstain = false;
};

/// `name` is carried so a failure over ~40 cases says which shape broke rather than which index.
struct vdoc_test::corpus_case
{
    cc::string name;
    vdoc::op_graph graph;

    /// Every op, in creation order — so a test can install a snapshot at each in turn.
    cc::vector<vdoc::op_id> ops;

    /// The head sets this case is worth materializing at, including multi-head ones.
    cc::vector<cc::vector<vdoc::op_id>> head_sets;

    /// Every path any op in this case wrote.
    cc::vector<vdoc::property_path> paths;
};

namespace vdoc_test
{
[[nodiscard]] vdoc::property_path path_of(cc::string_view e, cc::string_view c, cc::string_view p);

/// Builds and adds one op, standing in for op_builder.
///
/// The values are local because their views only have to survive as far as encode_assignments, which copies the bytes
/// into the blob — after that the op owns everything it points at.
[[nodiscard]] vdoc::op_id add_op(vdoc::op_graph& graph,
                                 cc::span<vdoc::op_id const> parents,
                                 cc::span<property_write const> writes);

/// The writers of one path, as sorted id bytes, for comparing two materializations.
[[nodiscard]] cc::vector<vdoc::op_id> writers_of(vdoc::raw_document const& doc, vdoc::property_path const& path);

[[nodiscard]] bool same_ids(cc::span<vdoc::op_id const> a, cc::span<vdoc::op_id const> b);

/// Two documents compared exactly: the same paths in the same order, the same writers in the same order, the same
/// value bytes.
///
/// Stronger than comparing writers path by path, because it also pins the iteration order the whole format commits to.
[[nodiscard]] bool same_document(vdoc::raw_document const& a, vdoc::raw_document const& b);

/// The deliberately stupid reference: a writer survives unless some OTHER writer of the same path descends from it.
///
/// This is what the real pass is checked against.
/// It is exponentially worse and obviously correct, which is the point: the snapshot cache is checked against the real
/// pass, so the real pass needs an oracle of its own.
///
/// **An abstention is a writer for dominance and not for the result.** It supersedes what it descends from exactly as a
/// write does, and is then dropped — so the two rules are separate steps here, in the order the sweep applies them.
[[nodiscard]] cc::vector<vdoc::op_id> oracle_writers(vdoc::op_graph const& graph,
                                                     cc::span<vdoc::op_id const> heads,
                                                     vdoc::property_path const& path);

/// The corpus: named shapes first, then a deterministic sweep of generated ones.
///
/// **Generation is a fixed LCG rather than a shared rng**, so the corpus is byte-stable across the repo's lifetime and
/// a failure reproduces from its name alone.
///
/// The `{ancestor, descendant}` head sets are load-bearing: that shape is the counterexample the snapshot cache's
/// validity gate exists to reject, and a corpus without it would accept an unsound gate.
[[nodiscard]] cc::vector<corpus_case> generate_corpus();
} // namespace vdoc_test
