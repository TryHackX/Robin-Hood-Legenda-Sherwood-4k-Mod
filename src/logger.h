#pragma once
#include <string>

namespace Logger {
    void Init(const std::string& filename, bool enabled);
    void Shutdown();
    void Info(const char* fmt, ...);
    void Warn(const char* fmt, ...);
    void Error(const char* fmt, ...);
}
