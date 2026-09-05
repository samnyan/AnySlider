#pragma once

#include <cstdarg>
#include <cstdio>

namespace anyslider
{
inline void Log(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    std::fputs("[AnySlider] ", stdout);
    std::vfprintf(stdout, format, args);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    va_end(args);
}
}
