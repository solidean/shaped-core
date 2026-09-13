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
} // namespace nx
