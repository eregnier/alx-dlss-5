#include <cassert>
#include <iostream>
#include <fstream>
#include <vector>
#include <thread>
#include <chrono>
#include "../include/hlx_logger.hpp"

int main() {
    std::cout << "[TEST] Running Asynchronous Logger & Background I/O Worker Test..." << std::endl;

    const wchar_t* testLogPath = L"test_async_worker.log";
    const wchar_t* testDumpPath = L"test_async_dump.raw";

    // Delete any old test files
    DeleteFileW(testLogPath);
    DeleteFileW(testDumpPath);

    // 1. Initialize Logger
    hlx::Logger::Instance().Init(testLogPath);

    // 2. Measure non-blocking throughput across multiple threads
    const int numThreads = 4;
    const int logsPerThread = 2500;
    std::vector<std::thread> threads;

    auto t_start = std::chrono::high_resolution_clock::now();

    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([t, logsPerThread]() {
            for (int i = 0; i < logsPerThread; ++i) {
                HLX_INFO("Worker thread %d test message index %d (stress testing non-blocking queue)", t, i);
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    auto t_end = std::chrono::high_resolution_clock::now();
    double totalMs = std::chrono::duration<double, std::milli>(t_end - t_start).count();
    double avgMicrosec = (totalMs * 1000.0) / (numThreads * logsPerThread);

    std::cout << "  [PASS] 10,000 concurrent log lines enqueued in " << totalMs << " ms (Avg: " << avgMicrosec << " microseconds/call)" << std::endl;
    assert(avgMicrosec < 50.0); // Must be fast memory queueing, NOT synchronous disk I/O!

    // 3. Test background binary texture dump task
    const uint32_t width = 256;
    const uint32_t height = 256;
    const uint32_t dataSize = width * height * 4;

    hlx::DumpTask task;
    task.filename = testDumpPath;
    task.width = width;
    task.height = height;
    task.format = 28; // DXGI_FORMAT_R8G8B8A8_UNORM
    task.rowPitch = width * 4;
    task.data.resize(dataSize, 0x7F);

    auto t_dump0 = std::chrono::high_resolution_clock::now();
    hlx::Logger::Instance().QueueDump(std::move(task));
    auto t_dump1 = std::chrono::high_resolution_clock::now();
    double dumpEnqueueMs = std::chrono::duration<double, std::milli>(t_dump1 - t_dump0).count();

    std::cout << "  [PASS] Binary dump task enqueued in " << dumpEnqueueMs << " ms (Zero render thread stall)" << std::endl;
    assert(dumpEnqueueMs < 5.0);

    // 4. Shutdown and drain background queue
    hlx::Logger::Instance().Shutdown();

    // 5. Verify log file exists and is populated
    HANDLE hLog = CreateFileW(testLogPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    assert(hLog != INVALID_HANDLE_VALUE);
    DWORD logSize = GetFileSize(hLog, nullptr);
    CloseHandle(hLog);
    std::cout << "  [PASS] Log file successfully written by worker thread: " << logSize << " bytes" << std::endl;
    assert(logSize > 100000); // 10,000 lines should be ~1 MB

    // 6. Verify binary dump file exists and has exact header + payload
    HANDLE hDump = CreateFileW(testDumpPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    assert(hDump != INVALID_HANDLE_VALUE);
    DWORD dumpFileSize = GetFileSize(hDump, nullptr);
    CloseHandle(hDump);
    const DWORD expectedSize = 16 + dataSize; // 4 uint32 header + payload
    assert(dumpFileSize == expectedSize);
    std::cout << "  [PASS] Async binary dump verified on disk: " << dumpFileSize << " bytes (Header + Payload matched)" << std::endl;

    // Cleanup test artifacts
    DeleteFileW(testLogPath);
    DeleteFileW(testDumpPath);

    std::cout << "[SUCCESS] Asynchronous Logger & Background I/O Worker Test Passed!" << std::endl;
    return 0;
}
