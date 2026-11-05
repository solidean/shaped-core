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

// caps this process's memory as dev.py caps what it starts (cc::cap_process_memory), so a binary run by hand or from a script cannot swap the machine
// - 30 GB, or SC_DEV_MEMORY_PROCESS_GB in GB, dev.py's own variable; 0 there turns the cap off
// - nx::run calls it first thing; a tool with its own main calls it there
void install_memory_cap();
} // namespace nx
