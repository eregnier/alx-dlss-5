#pragma once

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <mutex>
#include <string>
#include <vector>
#include <thread>
#include <condition_variable>
#include <atomic>

namespace hlx {

enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error
};

struct DumpTask {
    std::wstring filename;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    uint32_t rowPitch = 0;
    std::vector<char> data;
};

class Logger {
public:
    static Logger& Instance() {
        static Logger instance;
        return instance;
    }

    void Init(const wchar_t* logFileName = L"hlx_dlss5.log") {
        std::unique_lock<std::mutex> lock(mutex_);
        if (fileHandle_ == INVALID_HANDLE_VALUE) {
            fileHandle_ = CreateFileW(
                logFileName,
                GENERIC_WRITE,
                FILE_SHARE_READ,
                nullptr,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr
            );

            running_.store(true, std::memory_order_release);
            workerThread_ = std::thread(&Logger::WorkerLoop, this);

            // Queue initial header banners
            EnqueueLogInternal(LogLevel::Info, "==================================================");
            EnqueueLogInternal(LogLevel::Info, " HLX-DLSS5: Neural Reconstruction for Half-Life: Alyx");
            EnqueueLogInternal(LogLevel::Info, " Dual-Instance Stereoscopic OpenVR Proxy (Async I/O)");
            EnqueueLogInternal(LogLevel::Info, "==================================================");
        }
    }

    void Shutdown() {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!running_.load(std::memory_order_acquire)) {
                return;
            }
            EnqueueLogInternal(LogLevel::Info, "HLX-DLSS5 Logger shutting down.");
            running_.store(false, std::memory_order_release);
            cv_.notify_all();
        }

        if (workerThread_.joinable()) {
            workerThread_.join();
        }

        std::unique_lock<std::mutex> lock(mutex_);
        if (fileHandle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(fileHandle_);
            fileHandle_ = INVALID_HANDLE_VALUE;
        }
    }

    void Log(LogLevel level, const char* format, ...) {
        char buffer[2048];
        va_list args;
        va_start(args, format);
        vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);

        // Format timestamp and severity into line
        const char* levelStr = "INFO";
        switch (level) {
            case LogLevel::Debug:   levelStr = "DEBUG"; break;
            case LogLevel::Info:    levelStr = "INFO "; break;
            case LogLevel::Warning: levelStr = "WARN "; break;
            case LogLevel::Error:   levelStr = "ERROR"; break;
        }

        SYSTEMTIME st;
        GetLocalTime(&st);

        char line[2560];
        int len = snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] [%s] %s\r\n",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            levelStr, buffer);

        if (len > 0) {
            // Mirror to debugger immediately (zero cost on release)
            OutputDebugStringA(line);

            // Enqueue line for asynchronous background disk write
            std::unique_lock<std::mutex> lock(mutex_);
            logQueue_.emplace_back(line, static_cast<size_t>(len));
            cv_.notify_one();
        }
    }

    // High-performance asynchronous binary texture dump:
    // Hands off mapped RAM buffers to background disk thread, guaranteeing 0 ms stall on render thread
    void QueueDump(DumpTask&& task) {
        std::unique_lock<std::mutex> lock(mutex_);
        dumpQueue_.push_back(std::move(task));
        cv_.notify_one();
    }

private:
    Logger() : fileHandle_(INVALID_HANDLE_VALUE) {}
    ~Logger() { Shutdown(); }

    void EnqueueLogInternal(LogLevel level, const char* message) {
        const char* levelStr = (level == LogLevel::Error) ? "ERROR" : "INFO ";
        SYSTEMTIME st;
        GetLocalTime(&st);

        char line[2560];
        int len = snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] [%s] %s\r\n",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            levelStr, message);

        if (len > 0) {
            logQueue_.emplace_back(line, static_cast<size_t>(len));
            cv_.notify_one();
        }
    }

    void WorkerLoop() {
        std::vector<std::string> localLogQueue;
        std::vector<DumpTask> localDumpQueue;

        while (true) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] {
                    return !running_.load(std::memory_order_acquire) || !logQueue_.empty() || !dumpQueue_.empty();
                });

                if (!running_.load(std::memory_order_acquire) && logQueue_.empty() && dumpQueue_.empty()) {
                    break;
                }

                localLogQueue.swap(logQueue_);
                localDumpQueue.swap(dumpQueue_);
            }

            // 1. Process pending log entries (batched write)
            if (!localLogQueue.empty() && fileHandle_ != INVALID_HANDLE_VALUE) {
                for (const auto& entry : localLogQueue) {
                    DWORD written = 0;
                    WriteFile(fileHandle_, entry.data(), static_cast<DWORD>(entry.size()), &written, nullptr);
                }
                localLogQueue.clear();
            }

            // 2. Process asynchronous texture dumps in background
            if (!localDumpQueue.empty()) {
                for (auto& task : localDumpQueue) {
                    HANDLE hFile = CreateFileW(
                        task.filename.c_str(),
                        GENERIC_WRITE,
                        0,
                        nullptr,
                        CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL,
                        nullptr
                    );
                    if (hFile != INVALID_HANDLE_VALUE) {
                        DWORD bytesWritten = 0;
                        uint32_t header[4] = { task.width, task.height, task.format, task.rowPitch };
                        WriteFile(hFile, header, sizeof(header), &bytesWritten, nullptr);
                        if (!task.data.empty()) {
                            WriteFile(hFile, task.data.data(), static_cast<DWORD>(task.data.size()), &bytesWritten, nullptr);
                        }
                        CloseHandle(hFile);

                        char msg[512];
                        snprintf(msg, sizeof(msg), "Async texture dump complete: %ls (%ux%u, %zu bytes)",
                            task.filename.c_str(), task.width, task.height, task.data.size());
                        if (fileHandle_ != INVALID_HANDLE_VALUE) {
                            DWORD w = 0;
                            WriteFile(fileHandle_, msg, static_cast<DWORD>(strlen(msg)), &w, nullptr);
                            WriteFile(fileHandle_, "\r\n", 2, &w, nullptr);
                        }
                    }
                }
                localDumpQueue.clear();
            }
        }
    }

    HANDLE fileHandle_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread workerThread_;
    std::atomic<bool> running_{ false };
    std::vector<std::string> logQueue_;
    std::vector<DumpTask> dumpQueue_;
};

#define HLX_DEBUG(...) hlx::Logger::Instance().Log(hlx::LogLevel::Debug, __VA_ARGS__)
#define HLX_INFO(...)  hlx::Logger::Instance().Log(hlx::LogLevel::Info, __VA_ARGS__)
#define HLX_WARN(...)  hlx::Logger::Instance().Log(hlx::LogLevel::Warning, __VA_ARGS__)
#define HLX_ERROR(...) hlx::Logger::Instance().Log(hlx::LogLevel::Error, __VA_ARGS__)

} // namespace hlx
