#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/streams/impl/native_file.hh>
#include <clean-core/string/format.hh>

#ifdef CC_OS_WINDOWS

#include <clean-core/platform/win32_sanitized.hh>
#include <clean-core/string/conversion.hh> // utf8_to_utf16

#else

#include <clean-core/string/string.hh> // c_str_materialize for the path
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring> // std::strerror

#endif

namespace cc::impl
{
native_file::~native_file()
{
    this->impl_close();
}

native_file::native_file(native_file&& other) noexcept
{
#ifdef CC_OS_WINDOWS
    _handle = other._handle;
    other._handle = nullptr;
#else
    _fd = other._fd;
    other._fd = -1;
#endif
}

native_file& native_file::operator=(native_file&& other) noexcept
{
    if (this != &other)
    {
        this->impl_close();
#ifdef CC_OS_WINDOWS
        _handle = other._handle;
        other._handle = nullptr;
#else
        _fd = other._fd;
        other._fd = -1;
#endif
    }
    return *this;
}

#ifdef CC_OS_WINDOWS

// =========================================================================================================
// Windows
// =========================================================================================================

bool native_file::is_open() const
{
    return _handle != nullptr && _handle != INVALID_HANDLE_VALUE;
}

void native_file::impl_close()
{
    if (this->is_open())
        ::CloseHandle(_handle);
    _handle = nullptr;
}

cc::result<native_file> native_file::open(cc::string_view path, file_mode mode)
{
    auto wpath = cc::utf8_to_utf16(path);
    wpath.push_back(u'\0');

    DWORD access = 0;
    DWORD share = 0;
    DWORD disposition = 0;
    switch (mode)
    {
    case file_mode::read:
        access = GENERIC_READ;
        share = FILE_SHARE_READ;
        disposition = OPEN_EXISTING;
        break;
    case file_mode::write_truncate:
        access = GENERIC_WRITE;
        share = 0;
        disposition = CREATE_ALWAYS;
        break;
    case file_mode::write_keep:
        access = GENERIC_WRITE;
        share = 0;
        disposition = OPEN_ALWAYS; // create if missing, keep existing contents
        break;
    case file_mode::read_write:
        access = GENERIC_READ | GENERIC_WRITE;
        share = 0;
        disposition = OPEN_EXISTING; // must exist
        break;
    }

    HANDLE const h = ::CreateFileW(reinterpret_cast<wchar_t const*>(wpath.data()), access, share, nullptr, disposition,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return cc::error(cc::format("failed to open '{}' (win32 error {})", path, cc::u32(::GetLastError())));

    native_file f;
    f._handle = h;
    return f;
}

cc::result<cc::isize> native_file::read(cc::span<cc::byte> dst)
{
    CC_ASSERT(this->is_open(), "read on a closed file");
    if (dst.empty())
        return cc::isize(0);

    DWORD const want = dst.size() > cc::isize(0xFFFF'FFFF) ? 0xFFFF'FFFFu : DWORD(dst.size());
    DWORD got = 0;
    if (!::ReadFile(_handle, dst.data(), want, &got, nullptr))
        return cc::error(cc::format("ReadFile failed (win32 error {})", cc::u32(::GetLastError())));
    return cc::isize(got); // 0 => end of file
}

cc::result<cc::isize> native_file::write(cc::span<cc::byte const> src)
{
    CC_ASSERT(this->is_open(), "write on a closed file");
    if (src.empty())
        return cc::isize(0);

    DWORD const want = src.size() > cc::isize(0xFFFF'FFFF) ? 0xFFFF'FFFFu : DWORD(src.size());
    DWORD put = 0;
    if (!::WriteFile(_handle, src.data(), want, &put, nullptr))
        return cc::error(cc::format("WriteFile failed (win32 error {})", cc::u32(::GetLastError())));
    return cc::isize(put);
}

cc::result<cc::i64> native_file::seek(cc::i64 absolute_offset)
{
    CC_ASSERT(this->is_open(), "seek on a closed file");
    LARGE_INTEGER dist;
    dist.QuadPart = absolute_offset;
    LARGE_INTEGER out;
    if (!::SetFilePointerEx(_handle, dist, &out, FILE_BEGIN))
        return cc::error(cc::format("SetFilePointerEx failed (win32 error {})", cc::u32(::GetLastError())));
    return cc::i64(out.QuadPart);
}

cc::result<cc::i64> native_file::size()
{
    CC_ASSERT(this->is_open(), "size on a closed file");
    LARGE_INTEGER sz;
    if (!::GetFileSizeEx(_handle, &sz))
        return cc::error(cc::format("GetFileSizeEx failed (win32 error {})", cc::u32(::GetLastError())));
    return cc::i64(sz.QuadPart);
}

#else

// =========================================================================================================
// POSIX
// =========================================================================================================

bool native_file::is_open() const
{
    return _fd >= 0;
}

void native_file::impl_close()
{
    if (_fd >= 0)
        ::close(_fd);
    _fd = -1;
}

cc::result<native_file> native_file::open(cc::string_view path, file_mode mode)
{
    cc::string path_z(path); // owning copy so we can NUL-terminate (c_str_materialize mutates)

    int flags = 0;
    switch (mode)
    {
    case file_mode::read:
        flags = O_RDONLY;
        break;
    case file_mode::write_truncate:
        flags = O_WRONLY | O_CREAT | O_TRUNC;
        break;
    case file_mode::write_keep:
        flags = O_WRONLY | O_CREAT; // create if missing, keep existing contents
        break;
    case file_mode::read_write:
        flags = O_RDWR; // must exist
        break;
    }

    int const fd = ::open(path_z.c_str_materialize(), flags, 0644);
    if (fd < 0)
        return cc::error(cc::format("failed to open '{}' ({})", path, std::strerror(errno)));

    native_file f;
    f._fd = fd;
    return f;
}

cc::result<cc::isize> native_file::read(cc::span<cc::byte> dst)
{
    CC_ASSERT(this->is_open(), "read on a closed file");
    if (dst.empty())
        return cc::isize(0);

    auto const n = ::read(_fd, dst.data(), size_t(dst.size()));
    if (n < 0)
        return cc::error(cc::format("read failed ({})", std::strerror(errno)));
    return cc::isize(n); // 0 => end of file
}

cc::result<cc::isize> native_file::write(cc::span<cc::byte const> src)
{
    CC_ASSERT(this->is_open(), "write on a closed file");
    if (src.empty())
        return cc::isize(0);

    auto const n = ::write(_fd, src.data(), size_t(src.size()));
    if (n < 0)
        return cc::error(cc::format("write failed ({})", std::strerror(errno)));
    return cc::isize(n);
}

cc::result<cc::i64> native_file::seek(cc::i64 absolute_offset)
{
    CC_ASSERT(this->is_open(), "seek on a closed file");
    auto const p = ::lseek(_fd, off_t(absolute_offset), SEEK_SET);
    if (p < 0)
        return cc::error(cc::format("lseek failed ({})", std::strerror(errno)));
    return cc::i64(p);
}

cc::result<cc::i64> native_file::size()
{
    CC_ASSERT(this->is_open(), "size on a closed file");
    struct stat st;
    if (::fstat(_fd, &st) != 0)
        return cc::error(cc::format("fstat failed ({})", std::strerror(errno)));
    return cc::i64(st.st_size);
}

#endif
} // namespace cc::impl
