#pragma once

namespace nx
{
// default cmd line arg handling and running of nexus
// - runs all tests by default
// - passing a test name only runs that test (multiple names allowed, wildcards allowed)
// - returns 0 if all tests passed
// - disabled and manual tests can be run by a non-wildcard match
// - --manual restricts the run to manual tests (benchmarks etc.), allowing wildcard selection among them
int run(int argc, char** argv);
} // namespace nx
