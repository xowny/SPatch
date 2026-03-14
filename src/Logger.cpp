#include "Logger.h"

#include <Windows.h>

#include <cstdarg>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace spatch::log {
#if defined(SPATCH_FINAL_RELEASE)

bool Initialize(const std::filesystem::path& path) {
    (void)path;
    return true;
}

void Shutdown() {}

void Info(std::string_view message) {
    (void)message;
}

void Warn(std::string_view message) {
    (void)message;
}

void Error(std::string_view message) {
    (void)message;
}

void InfoF(const char* format, ...) {
    (void)format;
}

void WarnF(const char* format, ...) {
    (void)format;
}

void ErrorF(const char* format, ...) {
    (void)format;
}

std::string ToUtf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }

    const int size = WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }

    std::string utf8(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8,
                        0,
                        text.data(),
                        static_cast<int>(text.size()),
                        utf8.data(),
                        size,
                        nullptr,
                        nullptr);
    return utf8;
}

#else
namespace {

constexpr std::size_t kMaxQueuedLines = 8192;

std::mutex g_mutex;
std::condition_variable g_cv;
std::ofstream g_stream;
std::vector<std::string> g_queue;
std::thread g_writer_thread;
bool g_shutdown_requested = false;

std::string FormatMessage(const char* format, va_list args) {
    va_list args_copy;
    va_copy(args_copy, args);
    const int length = std::vsnprintf(nullptr, 0, format, args_copy);
    va_end(args_copy);

    if (length <= 0) {
        return {};
    }

    std::string buffer(static_cast<std::size_t>(length), '\0');
    std::vsnprintf(buffer.data(), buffer.size() + 1, format, args);
    return buffer;
}

std::string TimestampPrefix() {
    SYSTEMTIME system_time{};
    GetLocalTime(&system_time);

    char buffer[64]{};
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%04u-%02u-%02u %02u:%02u:%02u.%03u",
                  system_time.wYear,
                  system_time.wMonth,
                  system_time.wDay,
                  system_time.wHour,
                  system_time.wMinute,
                  system_time.wSecond,
                  system_time.wMilliseconds);
    return buffer;
}

std::string ComposeLine(const char* level, std::string_view message) {
    std::string line;
    const std::string timestamp = TimestampPrefix();
    line.reserve(timestamp.size() + message.size() + 16);
    line.push_back('[');
    line.append(timestamp);
    line.append("] [");
    line.append(level);
    line.append("] ");
    line.append(message.data(), message.size());
    line.push_back('\n');
    return line;
}

void WriterThreadProc() {
    std::vector<std::string> local;
    local.reserve(64);

    for (;;) {
        {
            std::unique_lock lock(g_mutex);
            g_cv.wait(lock, [] { return g_shutdown_requested || !g_queue.empty(); });
            g_queue.swap(local);
            if (g_shutdown_requested && local.empty()) {
                break;
            }
        }

        for (const std::string& line : local) {
            g_stream << line;
        }
        g_stream.flush();
        local.clear();
    }
}

void WriteLine(const char* level, std::string_view message) {
    const std::string line = ComposeLine(level, message);

    {
        std::scoped_lock lock(g_mutex);
        if (!g_stream.is_open() || g_shutdown_requested) {
            return;
        }

        if (g_queue.size() >= kMaxQueuedLines) {
            return;
        }

        g_queue.push_back(line);
    }

    g_cv.notify_one();
}

void WriteFormatted(const char* level, const char* format, va_list args) {
    WriteLine(level, FormatMessage(format, args));
}

}  // namespace

bool Initialize(const std::filesystem::path& path) {
    std::scoped_lock lock(g_mutex);
    if (g_stream.is_open()) {
        return true;
    }

    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    g_stream.open(path, std::ios::out | std::ios::app);
    if (!g_stream.is_open()) {
        return false;
    }

    g_shutdown_requested = false;
    g_queue.clear();
    g_writer_thread = std::thread(&WriterThreadProc);
    return true;
}

void Shutdown() {
    {
        std::scoped_lock lock(g_mutex);
        if (!g_stream.is_open()) {
            return;
        }
        g_shutdown_requested = true;
    }
    g_cv.notify_one();

    if (g_writer_thread.joinable()) {
        g_writer_thread.join();
    }

    std::scoped_lock lock(g_mutex);
    if (g_stream.is_open()) {
        g_stream.flush();
        g_stream.close();
    }
    g_queue.clear();
    g_shutdown_requested = false;
}

void Info(std::string_view message) {
    WriteLine("INFO", message);
}

void Warn(std::string_view message) {
    WriteLine("WARN", message);
}

void Error(std::string_view message) {
    WriteLine("ERROR", message);
}

void InfoF(const char* format, ...) {
    va_list args;
    va_start(args, format);
    WriteFormatted("INFO", format, args);
    va_end(args);
}

void WarnF(const char* format, ...) {
    va_list args;
    va_start(args, format);
    WriteFormatted("WARN", format, args);
    va_end(args);
}

void ErrorF(const char* format, ...) {
    va_list args;
    va_start(args, format);
    WriteFormatted("ERROR", format, args);
    va_end(args);
}

std::string ToUtf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }

    const int size = WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }

    std::string utf8(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8,
                        0,
                        text.data(),
                        static_cast<int>(text.size()),
                        utf8.data(),
                        size,
                        nullptr,
                        nullptr);
    return utf8;
}

#endif
}  // namespace spatch::log
