#pragma once
#include <functional>
#include <string>
#include <cstdarg>
#include <cstdio>
#include <cstring>

class Logger
{
public:
    using Callback = std::function<void(const std::string&)>;

    static void setCallback(Callback cb)
    {
        s_callback = std::move(cb);
    }

    static void log(const char* fmt, ...)
    {
        char buf[2048];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);

        // Strip trailing newlines in-place before any allocation
        int len = static_cast<int>(std::strlen(buf));
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
            buf[--len] = '\0';
        if (len == 0) return;

        if (s_callback)
            s_callback(std::string(buf, len)); // construct once, only when needed
        else
            std::printf("%s\n", buf);
    }

private:
    static inline Callback s_callback;
};