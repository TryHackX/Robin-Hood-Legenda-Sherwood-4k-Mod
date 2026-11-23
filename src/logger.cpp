#include "logger.h"
#include "mod_keys.h"       // SHERWOODFIX_VERSION
#include <windows.h>
#include <chrono>
#include <cstdio>
#include <cstdarg>
#include <mutex>
#include <share.h>

namespace Logger {
    namespace {
        FILE* g_logFile = nullptr;
        bool g_enabled = false;

        // Timed: the game kills its input thread with TerminateThread on exit and other code may
        // suspend threads. A thread that dies or stays suspended inside a log call must never be
        // able to block everybody else for good - a line is dropped instead.
        std::timed_mutex g_logMutex;
        constexpr auto kLockTimeout = std::chrono::milliseconds(250);

        void WriteLog(const char* level, const char* fmt, va_list args) {
            if (!g_enabled || !g_logFile) return;

            char msgBuf[2048]{};
            vsnprintf(msgBuf, sizeof(msgBuf), fmt, args);
            SYSTEMTIME st{};
            GetLocalTime(&st);

            std::unique_lock<std::timed_mutex> lock(g_logMutex, kLockTimeout);
            if (!lock.owns_lock() || !g_logFile) return;
            fprintf(g_logFile, "[%02d:%02d:%02d.%03d] [%s] %s\n",
                    st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                    level, msgBuf);
            // every line reaches the OS right away, so nothing is lost if the process is killed
            fflush(g_logFile);
        }
    }

    void Init(const std::string& filename, bool enabled) {
        std::lock_guard<std::timed_mutex> lock(g_logMutex);
        g_enabled = enabled;
        if (!g_enabled) return;

        // _SH_DENYNO: fopen_s would lock the file, and the log must be readable while the game runs
        g_logFile = _fsopen(filename.c_str(), "w", _SH_DENYNO);
        if (g_logFile) {
            fprintf(g_logFile, "============================================================\n");
            fprintf(g_logFile, " SherwoodEngineFix " SHERWOODFIX_VERSION " (x86 C++20) - Log Initialized\n");
            fprintf(g_logFile, "============================================================\n");
            fflush(g_logFile);
        }
    }

    void Shutdown() {
        std::unique_lock<std::timed_mutex> lock(g_logMutex, kLockTimeout);
        if (!lock.owns_lock()) return; // the lines written so far are already on disk
        if (g_logFile) {
            fprintf(g_logFile, "============================================================\n");
            fprintf(g_logFile, " SherwoodEngineFix - Log Closed\n");
            fprintf(g_logFile, "============================================================\n");
            fclose(g_logFile);
            g_logFile = nullptr;
        }
    }

    void Info(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        WriteLog("INFO", fmt, args);
        va_end(args);
    }

    void Warn(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        WriteLog("WARN", fmt, args);
        va_end(args);
    }

    void Error(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        WriteLog("ERROR", fmt, args);
        va_end(args);
    }
}
