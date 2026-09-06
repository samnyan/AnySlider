#pragma once

#include <atomic>
#include <cstdarg>
#include <cstdio>

namespace anyslider
{
inline std::atomic_bool debugLoggingEnabled = false;

inline void SetDebugLoggingEnabled(bool enabled)
{
    debugLoggingEnabled.store(enabled, std::memory_order_relaxed);
}

inline bool IsDebugLoggingEnabled()
{
    return debugLoggingEnabled.load(std::memory_order_relaxed);
}

inline void LogV(const char* prefix, const char* format, va_list args)
{
    std::fputs(prefix, stdout);
    std::vfprintf(stdout, format, args);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

inline void Log(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    LogV("[AnySlider] ", format, args);
    va_end(args);
}

inline void DebugLog(const char* format, ...)
{
    if (!IsDebugLoggingEnabled())
    {
        return;
    }
    va_list args;
    va_start(args, format);
    LogV("[AnySlider Debug] ", format, args);
    va_end(args);
}
}
