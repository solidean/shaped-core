#include "print.hh"

#include <cstdio>

// This is the ONLY translation unit that touches <cstdio>: the raw byte writers behind cc::print / eprint.

void cc::impl::write_stdout(string_view s)
{
    if (!s.empty())
        std::fwrite(s.data(), 1, static_cast<size_t>(s.size()), stdout);
}

void cc::impl::write_stderr(string_view s)
{
    if (!s.empty())
        std::fwrite(s.data(), 1, static_cast<size_t>(s.size()), stderr);
}
