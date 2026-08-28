// File stream throughput: cc::file_*_adapter vs std::ifstream / std::ofstream.
//
// The interesting axis is GRANULARITY, and the results plus their analysis are in libs/base/clean-core/docs/benchmarks/file-stream-benchmark.md.
// cc streams expose the buffer window directly, so byte-at-a-time I/O inlines to a pointer bump plus a store or load, with no per-byte virtual call.
// std streams route every put()/get() through the streambuf sentry and a virtual overflow/underflow.
// Each timed pass is end-to-end — open, transfer 4 MiB, close — and repeated passes stay in the OS cache, so this measures the stream layer's CPU cost rather than the disk.
//
// PGO benchmark: prints the full table and records the byte-at-a-time points (where the abstraction cost
// lives) via nx::pgo.


#include <clean-core/container/vector.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/record/stat.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/print.hh> // cc::print / cc::format
#include <clean-core/string/string.hh>
#include <nexus/bench/run.hh>
#include <nexus/bench/units.hh>
#include <nexus/pgo.hh>
#include <nexus/test.hh>

#include <fstream> // the std baseline under test
#include <string>

using namespace cc::primitive_defines;

namespace
{
constexpr isize total_bytes = 4 * 1024 * 1024; // payload transferred per timed pass
constexpr isize max_chunk = 64 * 1024;         // largest bulk chunk

std::string temp_path(char const* name)
{
    auto const path = cc::temp_file_path(name);
    return std::string(path.data(), size_t(path.size()));
}

// --- cc single-byte fast path: straight through the exposed window, refill/drain only when it fills --------

inline void cc_put(cc::seekable_write_stream& s, byte b)
{
    auto w = s.writable_bytes();
    if (w.empty())
    {
        (void)s.flush();
        w = s.writable_bytes();
    }
    w[0] = b;
    s.produce(1);
}

inline byte cc_get(cc::seekable_read_stream& s)
{
    auto r = s.ready_bytes();
    if (r.empty())
    {
        (void)s.flush();
        r = s.ready_bytes();
    }
    byte const b = r[0];
    s.consume(1);
    return b;
}

// --- one pass = open, transfer `total_bytes`, close ------------------------------------------------------
// chunk == 1 is byte-at-a-time; anything larger is a bulk transfer of `chunk` bytes.

u64 cc_write(cc::string_view path, cc::span<byte const> chunk, isize chunk_n)
{
    auto a = cc::file_write_stream_adapter::create(path);
    CC_ASSERT(a.has_value(), "open for write failed");
    auto s = a.value().stream();

    u64 acc = 0;
    if (chunk_n == 1)
        for (isize i = 0; i < total_bytes; ++i)
        {
            cc_put(s, byte(i));
            acc ^= u64(i);
        }
    else
        for (isize off = 0; off < total_bytes; off += chunk_n)
        {
            (void)s.write(chunk);
            acc ^= u64(off);
        }
    (void)s.flush();
    return acc;
}

u64 std_write(std::string const& path, char const* chunk, isize chunk_n)
{
    std::ofstream os(path, std::ios::binary | std::ios::trunc);

    u64 acc = 0;
    if (chunk_n == 1)
        for (isize i = 0; i < total_bytes; ++i)
        {
            os.put(char(i));
            acc ^= u64(i);
        }
    else
        for (isize off = 0; off < total_bytes; off += chunk_n)
        {
            os.write(chunk, chunk_n);
            acc ^= u64(off);
        }
    os.flush();
    return acc;
}

u64 cc_read(cc::string_view path, cc::span<byte> chunk, isize chunk_n)
{
    auto a = cc::file_read_stream_adapter::open(path);
    CC_ASSERT(a.has_value(), "open for read failed");
    auto s = a.value().stream();

    u64 acc = 0;
    if (chunk_n == 1)
        for (isize i = 0; i < total_bytes; ++i)
            acc ^= u64(cc_get(s));
    else
        for (isize off = 0; off < total_bytes; off += chunk_n)
            acc ^= u64(s.read(chunk).value_or(0));
    return acc;
}

u64 std_read(std::string const& path, char* chunk, isize chunk_n)
{
    std::ifstream is(path, std::ios::binary);

    u64 acc = 0;
    if (chunk_n == 1)
        for (isize i = 0; i < total_bytes; ++i)
            acc ^= u64(static_cast<unsigned char>(is.get()));
    else
        for (isize off = 0; off < total_bytes; off += chunk_n)
        {
            is.read(chunk, chunk_n);
            acc ^= u64(off);
        }
    return acc;
}

double mbps(double bytes_per_sec)
{
    return bytes_per_sec / 1e6;
}

struct paths
{
    std::string cc_file = temp_path("cc-file-bench.tmp");
    std::string std_file = temp_path("std-file-bench.tmp");
    [[nodiscard]] cc::string_view cc_view() const { return cc::string_view(cc_file.c_str()); }
    void remove() const
    {
        (void)cc::remove_file(cc::string_view(cc_file.data(), isize(cc_file.size())));
        (void)cc::remove_file(cc::string_view(std_file.data(), isize(std_file.size())));
    }
};

// A chunk size as a label (e.g. "1 B (put/get)", "16 B", "64 KiB").
cc::string granularity_label(isize chunk_n)
{
    if (chunk_n == 1)
        return cc::string("1 B (put/get)");
    if (chunk_n < 1024)
        return cc::format("{} B", chunk_n);
    return cc::format("{} KiB", chunk_n / 1024);
}

/// One measured transfer, recording the bytes moved so the harness derives B/s itself.
template <class Body>
nx::bench::result measure(cc::string_view name, nx::bench::run_config const& cfg, Body&& body)
{
    return nx::bench::run(name, cfg,
                          [&](nx::bench::iteration& it)
                          {
                              nx::bench::sink(body());
                              it.record("bytes", cc::rec::unit_bytes, f64(total_bytes));
                          });
}

double bytes_per_second_of(nx::bench::result const& r)
{
    auto const* const bytes = r.find_quantity("bytes");
    return bytes != nullptr ? bytes->per_second : 0.0;
}

/// The four paths at every granularity.
///
/// No explicit warmup is needed: the harness discards a warmup pass per loop, which warms that path's code and the
/// file cache.
/// Each chunk's write loops run before its read loops, so the files exist when the reads are measured.
/// A fresh post-build run can still read low across the whole table — that is machine state rather than per-metric
/// cache, so run on an idle machine and discard the first run.
void run(nx::bench::run_config const& cfg, bool record_representative)
{
    paths const p;
    auto cc_buf = cc::vector<byte>::create_filled(max_chunk, byte(0xA5));
    // std path takes a char*; char aliases anything, so the std side reuses cc_buf rather than a second buffer.
    char* const std_buf = reinterpret_cast<char*>(cc_buf.data());

    isize const chunks[] = {1, 2, 4, 8, 16, 64, 256, max_chunk};
    auto const* const bps = &nx::bench::unit_bytes_per_second;

    for (isize const chunk_n : chunks)
    {
        auto const cc_chunk = cc::span<byte const>(cc_buf).first_n(chunk_n);
        auto const cc_chunk_mut = cc::span<byte>(cc_buf).first_n(chunk_n);
        auto const label = granularity_label(chunk_n);

        auto const cc_w
            = measure(cc::format("cc write @{}", label), cfg, [&] { return cc_write(p.cc_view(), cc_chunk, chunk_n); });
        auto const std_w
            = measure(cc::format("std write @{}", label), cfg, [&] { return std_write(p.std_file, std_buf, chunk_n); });

        // Both files now exist (the last write pass left them populated), so the reads hit the page cache.
        auto const cc_r = measure(cc::format("cc read @{}", label), cfg,
                                  [&] { return cc_read(p.cc_view(), cc_chunk_mut, chunk_n); });
        auto const std_r
            = measure(cc::format("std read @{}", label), cfg, [&] { return std_read(p.std_file, std_buf, chunk_n); });

        // The 1 B granularity is the one the PGO report tracks: it is where the per-call cost dominates, so it is
        // where a codegen change actually shows up.
        if (record_representative && chunk_n == 1)
        {
            nx::pgo::report("cc write@1B", bytes_per_second_of(cc_w), *bps);
            nx::pgo::report("std write@1B", bytes_per_second_of(std_w), *bps);
            nx::pgo::report("cc read@1B", bytes_per_second_of(cc_r), *bps);
            nx::pgo::report("std read@1B", bytes_per_second_of(std_r), *bps);
        }
    }

    p.remove();
}
} // namespace

PGO_BENCHMARK("bench-file-stream (cc vs std)")
{
    // File I/O does not converge to the default 2%: the page cache, the writeback thread and the OS scheduler all
    // move between samples, and no sample count fixes that.
    // Asking for 10% is what this measurement can actually deliver, and it is still far tighter than the differences
    // the report is read for.
    run({.min_time_secs = 0.1, .max_samples = 512, .target_relative_error = 0.10}, /*record_representative*/ true);
}

// The full granularity table, as a comparison per chunk size.
//
// A sweep across granularities: comparing a 1 B transfer against a 64 KiB one is a statement about the chunk size
// rather than about cc against std, so the rows carry no baseline and the byte rate is what stays comparable.
BENCHMARK("bench-file-stream - granularity sweep")
{
    run({.min_time_secs = 0.05, .max_samples = 128, .target_relative_error = 0.10, .no_baseline = true},
        /*record_representative*/ false);
}
