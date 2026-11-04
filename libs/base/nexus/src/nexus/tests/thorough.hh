#pragma once

namespace nx
{
/// Whether the running test was asked to test MORE, at the price of running longer — `--thorough` on the binary,
/// `uv run dev.py test --thorough` from the driver.
///
/// A flag, not a bucket: the test is the same test either way, and a default run keeps it.
/// The thorough run is the one a test is written for, and a default run narrows it — fewer seeds, lower caps, smaller
/// inputs — so `check` stays fast while the full-strength version stays one flag away.
/// False outside a running test.
[[nodiscard]] bool is_thorough();

/// Whether the running test is a smoke run, which asks LESS of it than a default run: `--smoke` on the binary.
///
/// `dev.py check` runs the whole suite on several presets, and only the default one needs a heavy test's full ground.
/// The others prove the code builds and runs where their flags differ, which a heavy test proves on its smallest case:
/// `check` passes `--smoke` to every preset but the default, and such a test narrows to that case under it.
/// Narrow ground, never checks, as under a default run; `--thorough` wins over it.
/// False outside a running test.
[[nodiscard]] bool is_smoke();
} // namespace nx
