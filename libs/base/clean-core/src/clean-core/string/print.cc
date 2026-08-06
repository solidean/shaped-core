#include "print.hh"

#include <cstdio>

// The raw byte writers behind cc::print / eprint live here so that print.hh stays free of <cstdio>.

void cc::print(string_view s)
{
    if (!s.empty())
        std::fwrite(s.data(), 1, static_cast<size_t>(s.size()), stdout);
}

void cc::flush()
{
    std::fflush(stdout);
}

void cc::println(string_view s)
{
    cc::print(s);
    cc::print("\n");
    cc::flush();
}

void cc::eprint(string_view s)
{
    if (!s.empty())
        std::fwrite(s.data(), 1, static_cast<size_t>(s.size()), stderr);
}

void cc::eflush()
{
    std::fflush(stderr);
}

void cc::eprintln(string_view s)
{
    cc::eprint(s);
    cc::eprint("\n");
    cc::eflush();
}
