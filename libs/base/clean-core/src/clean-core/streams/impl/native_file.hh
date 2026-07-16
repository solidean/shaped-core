#pragma once

#include <clean-core/common/macros.hh> // CC_OS_WINDOWS
#include <clean-core/container/span.hh>
#include <clean-core/error/result.hh>
#include <clean-core/fwd.hh>
#include <clean-core/string/string_view.hh>

// =========================================================================================================
// cc::impl::native_file — minimal cross-platform file handle
// =========================================================================================================
//
// A thin, move-only wrapper over an OS file handle (Windows HANDLE / POSIX fd). It confines ALL platform I/O
// (and <Windows.h>) to native_file.cc, so the file stream adapters stay pure buffer logic. Not a public API
// — it lives under streams/impl/ and may graduate to clean-core/platform/ if it finds other uses.

namespace cc::impl
{
enum class file_mode : cc::u8
{
    read,           // open an existing file for reading
    write_truncate, // create or truncate a file for writing
    write_keep,     // open (creating if missing) for writing, WITHOUT truncating
    read_write,     // open an existing file for reading and writing
};

class native_file
{
public:
    native_file() = default;
    ~native_file();

    native_file(native_file&&) noexcept;
    native_file& operator=(native_file&&) noexcept;
    native_file(native_file const&) = delete;
    native_file& operator=(native_file const&) = delete;

    /// Open `path` (UTF-8) in the given mode.
    [[nodiscard]] static cc::result<native_file> open(cc::string_view path, file_mode mode);

    /// Read up to dst.size() bytes at the current position; returns the count read (0 == end-of-file). Short
    /// reads are normal.
    [[nodiscard]] cc::result<cc::isize> read(cc::span<cc::byte> dst);

    /// Write up to src.size() bytes at the current position; returns the count written. Short writes are
    /// possible — callers loop.
    [[nodiscard]] cc::result<cc::isize> write(cc::span<cc::byte const> src);

    /// Move the file pointer to an absolute offset from the start; returns the new position.
    [[nodiscard]] cc::result<cc::i64> seek(cc::i64 absolute_offset);

    /// Current total size of the file in bytes (independent of the file pointer).
    [[nodiscard]] cc::result<cc::i64> size();

    [[nodiscard]] bool is_open() const;

private:
    void impl_close();

#ifdef CC_OS_WINDOWS
    void* _handle = nullptr; // HANDLE; nullptr means closed
#else
    int _fd = -1; // -1 means closed
#endif
};
} // namespace cc::impl
