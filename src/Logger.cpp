#include "Logger.h"

#include <Windows.h>

#include <atomic>
#include <cstdarg>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <mutex>
#include <process.h>
#include <utility>
#include <vector>

namespace spatch::log {
namespace {

constexpr std::size_t kMaxQueuedLines = 8192;
constexpr std::uintmax_t kMaxLogBytes = 8ull * 1024ull * 1024ull;

std::mutex g_mutex;
std::mutex g_lifecycle_mutex;
std::condition_variable g_cv;
std::ofstream g_stream;
std::filesystem::path g_active_path;
std::vector<std::string> g_queue;
HANDLE g_writer_thread = nullptr;
bool g_shutdown_requested = false;
bool g_writer_failed = false;
std::atomic<bool> g_enabled = false;
std::atomic<std::uint64_t> g_dropped_lines = 0;

void MarkWriterFailed() noexcept {
    g_enabled.store(false, std::memory_order_release);
    {
        std::lock_guard lock(g_mutex);
        g_writer_failed = true;
        g_shutdown_requested = true;
        g_queue.clear();
    }
    g_cv.notify_all();
}

// g_lifecycle_mutex must be held by the caller. Keeping the wait outside
// g_mutex lets a writer that is draining or reporting a failure finish.
void StopWriterUnderLifecycleLock() noexcept {
    g_enabled.store(false, std::memory_order_release);

    HANDLE writer = nullptr;
    {
        std::lock_guard lock(g_mutex);
        if (!g_stream.is_open() && g_writer_thread == nullptr) {
            g_active_path.clear();
            g_queue.clear();
            g_shutdown_requested = false;
            g_writer_failed = false;
            return;
        }
        g_shutdown_requested = true;
        writer = g_writer_thread;
    }
    g_cv.notify_all();

    if (writer != nullptr) {
        WaitForSingleObject(writer, INFINITE);
        CloseHandle(writer);
    }

    std::lock_guard lock(g_mutex);
    g_writer_thread = nullptr;
    if (g_stream.is_open()) {
        g_stream.flush();
        g_stream.close();
    }
    g_active_path.clear();
    g_queue.clear();
    g_shutdown_requested = false;
    g_writer_failed = false;
}

std::string FormatMessage(const char* format, va_list args) {
    va_list args_copy;
    va_copy(args_copy, args);
    const int length = std::vsnprintf(nullptr, 0, format, args_copy);
    va_end(args_copy);

    if (length <= 0) {
        return {};
    }

    std::string buffer(static_cast<std::size_t>(length) + 1, '\0');
    const int written = std::vsnprintf(buffer.data(), buffer.size(), format, args);
    if (written < 0 || written > length) {
        return {};
    }
    buffer.resize(static_cast<std::size_t>(written));
    return buffer;
}

std::filesystem::path NormalizeLogPath(
    const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path absolute_path =
        std::filesystem::absolute(path, error);
    return (error ? path : absolute_path).lexically_normal();
}

bool SameLogPath(const std::filesystem::path& left,
                 const std::filesystem::path& right) noexcept {
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
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

unsigned __stdcall WriterThreadProc(void*) noexcept {
    try {
        std::vector<std::string> local;
        local.reserve(64);

        for (;;) {
            {
                std::unique_lock lock(g_mutex);
                g_cv.wait(lock, [] { return g_shutdown_requested || !g_queue.empty(); });
                g_queue.swap(local);
                if (g_shutdown_requested && local.empty()) {
                    return 0;
                }
            }

            const std::uint64_t dropped =
                g_dropped_lines.exchange(0, std::memory_order_acq_rel);
            if (dropped != 0) {
                char message[128]{};
                std::snprintf(message,
                              sizeof(message),
                              "logger queue overflow dropped=%llu",
                              static_cast<unsigned long long>(dropped));
                g_stream << ComposeLine("WARN", message);
            }
            for (const std::string& line : local) {
                g_stream << line;
            }
            g_stream.flush();
            if (!g_stream.good()) {
                MarkWriterFailed();
                return ERROR_WRITE_FAULT;
            }
            local.clear();
        }
    } catch (...) {
        // Logging must never become a process-termination path.  In
        // particular, a bad_alloc in this worker must not escape the CRT
        // thread trampoline (which would call terminate).  Wake Shutdown so
        // it can observe a signalled writer handle and close the stream.
        MarkWriterFailed();
        return ERROR_UNHANDLED_EXCEPTION;
    }
}

void WriteLine(const char* level, std::string_view message) noexcept {
    if (!g_enabled.load(std::memory_order_acquire)) {
        return;
    }
    try {
        std::string line = ComposeLine(level, message);

        {
            std::scoped_lock lock(g_mutex);
            if (!g_stream.is_open() || g_shutdown_requested) {
                return;
            }

            if (g_queue.size() >= kMaxQueuedLines) {
                g_dropped_lines.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            g_queue.push_back(std::move(line));
        }

        g_cv.notify_one();
    } catch (...) {
        // Diagnostics are best-effort.  Never let allocation/formatting
        // failure cross an engine detour boundary.
    }
}

void WriteFormatted(const char* level, const char* format, va_list args) noexcept {
    if (!g_enabled.load(std::memory_order_acquire)) {
        return;
    }
    try {
        WriteLine(level, FormatMessage(format, args));
    } catch (...) {
        // FormatMessage may allocate before WriteLine gets control.
    }
}

}  // namespace

bool Initialize(const std::filesystem::path& path, bool enabled) {
    if (!enabled) {
        Shutdown();
        return true;
    }

    if (path.empty()) {
        return false;
    }
    const std::filesystem::path normalized_path = NormalizeLogPath(path);

    std::scoped_lock lifecycle_lock(g_lifecycle_mutex);
    {
        std::scoped_lock lock(g_mutex);
        const bool writer_running =
            g_writer_thread != nullptr &&
            WaitForSingleObject(g_writer_thread, 0) == WAIT_TIMEOUT;
        if (g_stream.is_open() && writer_running && !g_shutdown_requested &&
            !g_writer_failed && SameLogPath(g_active_path, normalized_path)) {
            g_enabled.store(true, std::memory_order_release);
            return true;
        }
    }
    StopWriterUnderLifecycleLock();

    std::error_code error;
    std::filesystem::create_directories(normalized_path.parent_path(), error);
    const bool log_exists = std::filesystem::exists(normalized_path, error);
    const std::uintmax_t log_size =
        log_exists && !error ? std::filesystem::file_size(normalized_path, error) : 0;
    bool truncate_log = false;
    if (log_exists && !error && log_size > kMaxLogBytes) {
        // Keep the game directory tidy: cap the one diagnostic log in place
        // instead of creating a persistent SPatch.log.old work file.
        truncate_log = true;
    }

    std::scoped_lock lock(g_mutex);
    g_stream.open(normalized_path,
                  std::ios::out |
                      (truncate_log ? std::ios::trunc : std::ios::app));
    if (!g_stream.is_open()) {
        g_enabled.store(false, std::memory_order_release);
        return false;
    }

    g_shutdown_requested = false;
    g_writer_failed = false;
    g_dropped_lines.store(0, std::memory_order_relaxed);
    g_queue.clear();
    const uintptr_t writer = _beginthreadex(nullptr, 0, &WriterThreadProc, nullptr, 0, nullptr);
    if (writer == 0) {
        g_stream.close();
        g_enabled.store(false, std::memory_order_release);
        return false;
    }
    g_writer_thread = reinterpret_cast<HANDLE>(writer);
    g_active_path = normalized_path;
    g_enabled.store(true, std::memory_order_release);
    return true;
}

void Shutdown() {
    std::scoped_lock lifecycle_lock(g_lifecycle_mutex);
    StopWriterUnderLifecycleLock();
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
    if (text.empty() || text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return {};
    }

    const int input_size = static_cast<int>(text.size());
    const int output_size =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), input_size, nullptr, 0, nullptr, nullptr);
    if (output_size <= 0) {
        return {};
    }

    std::string utf8(static_cast<std::size_t>(output_size), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8, 0, text.data(), input_size, utf8.data(), output_size, nullptr, nullptr);
    if (written != output_size) {
        return {};
    }
    return utf8;
}

}  // namespace spatch::log
