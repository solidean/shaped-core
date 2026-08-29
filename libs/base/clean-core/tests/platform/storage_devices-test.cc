#include <clean-core/platform/storage_devices.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// Which disks a machine has is not something a test can know, so nothing here asserts a device or a size.
// What it pins is that a mount describes one coherent filesystem, that free and available are kept apart, and that a
// sampler keyed on a device that is not there fails cleanly instead of differencing two unrelated counters.

TEST("cc storage_devices - a mount describes one coherent filesystem")
{
    auto mounts = cc::query_mounts();
    if (mounts.has_error())
    {
        // A platform with no filesystems to report says which kind of cannot, in words a log line can carry.
        CHECK(!mounts.error().detail.empty());
        return;
    }

    for (auto const& m : mounts.value())
    {
        CHECK(!m.path.empty());

        // Zero-sized mounts are filtered out, because a pseudo filesystem is nothing a dashboard can draw.
        CHECK(m.total_bytes > 0);

        CHECK(m.free_bytes >= 0);
        CHECK(m.free_bytes <= m.total_bytes);

        // available is what THIS user may take, so it never exceeds what is actually free.
        CHECK(m.available_bytes >= 0);
        CHECK(m.available_bytes <= m.free_bytes);
    }
}

TEST("cc storage_devices - every enumerated disk has an id to key a sampler on")
{
    // Counted rather than checked per device, so the test asserts the property itself and still says something on a
    // platform that enumerates nothing.
    auto without_id = 0;
    for (auto const& d : cc::enumerate_disks())
    {
        if (d.id.empty())
            ++without_id;
        if (d.capacity_bytes.has_value())
            CHECK(d.capacity_bytes.value() > 0);
    }

    CHECK(without_id == 0);
}

TEST("cc storage_devices - disk ids are unique")
{
    auto const disks = cc::enumerate_disks();

    auto repeats = 0;
    for (isize i = 0; i < disks.size(); ++i)
        for (isize j = i + 1; j < disks.size(); ++j)
            if (disks[i].id == disks[j].id)
                ++repeats;

    // An id that repeats would make two samplers silently share one device's counters.
    CHECK(repeats == 0);
}

TEST("cc storage_devices - io counters are monotone for a real device")
{
    auto const disks = cc::enumerate_disks();
    if (disks.empty() || !cc::disk_io_sampler::is_supported())
    {
        // Nothing to difference, and nothing invented in its place: a lookup here fails rather than returning zeroes.
        CHECK(cc::read_disk_io_counters("cc-no-such-device-7f3a").has_error());
        return;
    }

    auto first = cc::read_disk_io_counters(disks[0].id);
    if (first.has_error())
    {
        // A device that will not report is normal, and must say so rather than return zeroes.
        CHECK(!first.error().detail.empty());
        return;
    }

    auto second = cc::read_disk_io_counters(disks[0].id);
    REQUIRE(second.has_value());

    CHECK(second.value().bytes_read >= first.value().bytes_read);
    CHECK(second.value().bytes_written >= first.value().bytes_written);
    CHECK(second.value().read_ops >= first.value().read_ops);
    CHECK(second.value().write_ops >= first.value().write_ops);

    CHECK(first.value().busy_secs.has_value() == second.value().busy_secs.has_value());
    if (first.value().busy_secs.has_value())
        CHECK(second.value().busy_secs.value() >= first.value().busy_secs.value());
}

TEST("cc storage_devices - a sampled rate is non-negative and bounded")
{
    auto const disks = cc::enumerate_disks();
    if (disks.empty() || !cc::disk_io_sampler::is_supported())
    {
        CHECK(cc::read_disk_io_counters("cc-no-such-device-7f3a").has_error());
        return;
    }

    auto sampler = cc::disk_io_sampler(disks[0].id);
    auto rate = sampler.sample();
    if (rate.has_error())
    {
        CHECK(!rate.error().detail.empty());
        return;
    }

    CHECK(rate.value().read_bytes_per_sec >= 0);
    CHECK(rate.value().write_bytes_per_sec >= 0);
    CHECK(rate.value().read_ops_per_sec >= 0);
    CHECK(rate.value().write_ops_per_sec >= 0);

    if (rate.value().busy_fraction.has_value())
    {
        CHECK(rate.value().busy_fraction.value() >= 0.0f);
        CHECK(rate.value().busy_fraction.value() <= 1.0f);
    }
}

TEST("cc storage_devices - a device that is not there reports absence, not zeroes")
{
    // The vanished-device case, which is normal operation rather than an error: a sampler must never report a stale
    // reading or a wild delta for a drive that was unplugged.
    auto const missing = cc::string_view("cc-no-such-device-7f3a");

    auto const counters = cc::read_disk_io_counters(missing);
    REQUIRE(counters.has_error());
    CHECK(counters.error().status == cc::query_status::unsupported);

    auto sampler = cc::disk_io_sampler(missing);
    auto const rate = sampler.sample();
    CHECK(rate.has_error());
}
