#pragma once

namespace nx
{
// default cmd line arg handling and running of nexus
// - runs all tests by default
// - passing a test name only runs that test (multiple names allowed, wildcards allowed)
// - returns 0 if all tests passed
// - disabled tests and apps can be run by a non-wildcard match
int run(int argc, char** argv);
} // namespace nx