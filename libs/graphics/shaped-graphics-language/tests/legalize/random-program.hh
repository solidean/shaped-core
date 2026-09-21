#pragma once

#include "flat-test-support.hh"

namespace sgl_test
{
struct program_shape;
} // namespace sgl_test

struct sgl_test::program_shape
{
    /// How deep statements nest; every loop, block and block expression is one level.
    int max_depth = 3;
    /// How many statements a list holds at most.
    int max_statements = 4;
    /// A budget over the whole program, which is what keeps a deep shape from growing without bound.
    int max_nodes = 60;
};

namespace sgl_test
{

/// A well-typed entry point `fun main(p: frag) -> float` in the STRUCTURED form, the same one for the same seed.
///
/// It is built to disagree with a wrong legalizer.
/// Blocks with values stand as operands of larger expressions, with prints inside them and to their left.
/// A `var` is read to the left of a block that assigns it.
/// A leave names any enclosing block, the root included, and a continue any enclosing loop, across blocks and loops.
/// The right side of an `and` / `or` prints, and a `while` condition is a block.
///
/// Every loop runs a bounded number of times: a counter nothing else assigns, advanced before anything can `continue`.
[[nodiscard]] sgl::check::flat_entry_point random_program(sgl::check::checked_module const& m,
                                                          u64 seed,
                                                          program_shape const& shape = {});

/// Generates the program of `seed`, runs it, legalizes it, runs that, and compares.
/// Empty when the two runs agree and the legalized tree is core; else the seed, both dumps and both outcomes.
[[nodiscard]] cc::string differential_failure(sgl::check::checked_module const& m,
                                              u64 seed,
                                              program_shape const& shape = {},
                                              sgl::check::legalize_options const& options = {});
} // namespace sgl_test
