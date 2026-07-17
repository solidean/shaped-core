// File stream throughput: cc::file_*_adapter vs std::ifstream / std::ofstream.
//
// The interesting axis is GRANULARITY. cc streams expose the buffer window directly, so byte-at-a-time I/O
// (writable_bytes()/produce(), ready_bytes()/consume()) inlines to a pointer bump + store/load with no per-byte
// virtual call; std streams route every put()/get() through the streambuf sentry + virtual overflow/underflow.
// For bulk transfers both are a memcpy into a buffer plus the occasional syscall, so they converge and the
// number is really the OS write-back / page cache. We time end-to-end (open -> transfer 4 MiB -> close); at
// 4 MiB the open/close is well under a percent, and repeated passes stay in the OS cache so this measures the
// stream layer's CPU cost, not the disk.
//
// Guide benchmark: prints the full table and records the byte-at-a-time points (where the abstraction cost
// lives) via nx::guide.

#include "bench_util.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/print.hh> // cc::print / cc::format
#include <clean-core/string/string.hh>
#include <nexus/guide.hh>
#include <nexus/test.hh>

#include <filesystem> // OS temp dir + remove (no cc filesystem yet)
#include <fstream>    // the std baseline under test
#include <string>

using namespace cc::primitive_defines;

namespace
{
constexpr isize total_bytes = 4 * 1024 * 1024; // payload transferred per timed pass
constexpr isize max_chunk = 64 * 1024;         // largest bulk chunk

std::string temp_path(char const* name)
{
    return (std::filesystem::temp_directory_path() / name).string();
}

// --- cc single-byte fast path: straight through the exposed window, refill/drain only when it fills --------

inline void cc_put(cc::seekable_write_stream& s, cc::byte b)
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

inline cc::byte cc_get(cc::seekable_read_stream& s)
{
    auto r = s.ready_bytes();
    if (r.empty())
    {
        (void)s.flush();
        r = s.ready_bytes();
    }
    cc::byte const b = r[0];
    s.consume(1);
    return b;
}

// --- one pass = open, transfer `total_bytes`, close. chunk == 1 is byte-at-a-time, else bulk of `chunk`. ----

u64 cc_write(cc::string_view path, cc::span<cc::byte const> chunk, isize chunk_n)
{
    auto a = cc::file_write_stream_adapter::create(path);
    CC_ASSERT(a.has_value(), "open for write failed");
    auto s = a.value().stream();

    u64 acc = 0;
    if (chunk_n == 1)
        for (isize i = 0; i < total_bytes; ++i)
        {
            cc_put(s, cc::byte(i));
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

u64 cc_read(cc::string_view path, cc::span<cc::byte> chunk, isize chunk_n)
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
        std::error_code ec;
        std::filesystem::remove(cc_file, ec);
        std::filesystem::remove(std_file, ec);
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

void run()
{
    paths const p;
    auto cc_buf = cc::vector<cc::byte>::create_filled(max_chunk, cc::byte(0xA5));
    // std path takes a char*; char aliases anything, so the std side reuses cc_buf rather than a second buffer.
    char* const std_buf = reinterpret_cast<char*>(cc_buf.data());

    // No explicit warmup: bench::measure_units_per_sec already discards one full pass per measurement, which
    // warms that path's code + file cache. Each chunk's write rows run before its read rows, so the files
    // exist when the reads are measured. (A fresh post-build run can still read low on the whole table — that
    // is machine state, not per-metric cache; run on an idle machine and discard the first run.)
    isize const chunks[] = {1, 2, 4, 8, 16, 64, 256, max_chunk};

    cc::println("\n=== file stream throughput (MB/s, {} MiB per pass, open->transfer->close) ===",
                total_bytes / (1024 * 1024));
    cc::println("{:<16} {:>10} {:>10} {:>10} {:>10}", "granularity", "cc write", "std write", "cc read", "std read");
    cc::println("{:<16} {:>10} {:>10} {:>10} {:>10}", "-----------", "--------", "---------", "-------", "--------");

    for (isize const chunk_n : chunks)
    {
        auto const cc_chunk = cc::span<cc::byte const>(cc_buf).first_n(chunk_n);
        auto const cc_chunk_mut = cc::span<cc::byte>(cc_buf).first_n(chunk_n);

        double const cc_w = mbps(bench::measure_units_per_sec(
            double(total_bytes), [&] { return cc_write(p.cc_view(), cc_chunk, chunk_n); }));
        double const std_w = mbps(
            bench::measure_units_per_sec(double(total_bytes), [&] { return std_write(p.std_file, std_buf, chunk_n); }));

        // both files now exist (last write pass left them populated) -> reads hit the page cache
        double const cc_r = mbps(bench::measure_units_per_sec(
            double(total_bytes), [&] { return cc_read(p.cc_view(), cc_chunk_mut, chunk_n); }));
        double const std_r = mbps(
            bench::measure_units_per_sec(double(total_bytes), [&] { return std_read(p.std_file, std_buf, chunk_n); }));

        cc::println("{:<16} {:>10.0f} {:>10.0f} {:>10.0f} {:>10.0f}", granularity_label(chunk_n), cc_w, std_w, cc_r,
                    std_r);

        if (chunk_n == 1)
        {
            nx::guide::report_raw("cc write@1B", cc_w, "MB/s", true);
            nx::guide::report_raw("std write@1B", std_w, "MB/s", true);
            nx::guide::report_raw("cc read@1B", cc_r, "MB/s", true);
            nx::guide::report_raw("std read@1B", std_r, "MB/s", true);
        }
    }

    p.remove();
}
} // namespace

GUIDE_BENCHMARK("bench-file-stream (cc vs std)")
{
    run();
}
