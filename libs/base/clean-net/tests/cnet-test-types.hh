#pragma once

#include <nexus/test.hh>

// Shared scaffolding for the clean-net tests.

/// A test that stands up an io_system.
///
/// **`cc::thread_pump_all()` is process-wide, and that is what makes this necessary.**
/// An unthreaded io_system registers with clean-core's pump registry, so every blocking wait anywhere in the process
/// sweeps it -- by design, since a cnet-only pump would be exactly the deadlock the registry exists to prevent
/// (io_system.hh argues it).
/// Nexus runs tests in parallel, so without this one test's pump drives another test's reactor, and that test's
/// completion callbacks run on a thread it never expected -- reaching its fixtures, its sinks and its counters from
/// two threads at once.
/// ThreadSanitizer catches it as a spray of races across the server, the session and whatever the body callback
/// touches; before TSan it was simply an unexplained flake.
///
/// The tag rather than a bare `exclusive()`: these tests only conflict with each other, so they serialize among
/// themselves and still run beside every parsing test in the binary.
#define CNET_IO_TEST(name_, ...) TEST(name_, nx::config::exclusive("cnet-io") __VA_OPT__(, ) __VA_ARGS__)
