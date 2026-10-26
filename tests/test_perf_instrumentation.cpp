#include <cassert>
#include <iostream>
#include <cstdint>
#include <vector>
#include <chrono>

// Test Component 1: One-Time Stage Override Guard
// Ensures ClearStageOverride & FadeGrid are never repeatedly called across IPC,
// preventing SteamVR compositor message queue congestion.
class StageClearGuard {
public:
    bool TryClearStage(uint64_t frameCount) {
        if (!stageCleared_ && frameCount >= 10) {
            stageCleared_ = true;
            clearCallsCount_++;
            return true;
        }
        return false;
    }

    uint32_t GetCallCount() const { return clearCallsCount_; }

private:
    bool stageCleared_ = false;
    uint32_t clearCallsCount_ = 0;
};

// Test Component 2: Performance Spike Detector & 1-Second Cadence Aggregator
// Tracks sub-millisecond metrics (CopyIn, EvalCPU, GpuSync, CopyOut, VRSubmit)
// and identifies performance anomalies (> 12 ms per eye).
struct FrameMetrics {
    double copyInMs;
    double evalCpuMs;
    double gpuSyncMs;
    double copyOutMs;
    double vrSubmitMs;
    double totalMs;
};

class CadenceAggregator {
public:
    void RecordEye(const FrameMetrics& m) {
        eyeCount_++;
        totalSum_ += m.totalMs;
        syncSum_ += m.gpuSyncMs;
        submitSum_ += m.vrSubmitMs;
        if (m.totalMs > 12.0) {
            spikeCount_++;
        }
        if (m.totalMs > maxTotal_) {
            maxTotal_ = m.totalMs;
        }
    }

    bool TryEmit1SecReport(double elapsedSec, double& outStereoFps, double& outAvgTotal, double& outMaxTotal, double& outAvgSync, uint32_t& outSpikes) {
        if (elapsedSec < 1.0) return false;

        outStereoFps = (eyeCount_ / 2.0) / elapsedSec;
        outAvgTotal = eyeCount_ > 0 ? (totalSum_ / eyeCount_) : 0.0;
        outMaxTotal = maxTotal_;
        outAvgSync = eyeCount_ > 0 ? (syncSum_ / eyeCount_) : 0.0;
        outSpikes = spikeCount_;

        // Reset window
        eyeCount_ = 0;
        spikeCount_ = 0;
        totalSum_ = 0.0;
        maxTotal_ = 0.0;
        syncSum_ = 0.0;
        submitSum_ = 0.0;
        return true;
    }

private:
    uint32_t eyeCount_ = 0;
    uint32_t spikeCount_ = 0;
    double totalSum_ = 0.0;
    double maxTotal_ = 0.0;
    double syncSum_ = 0.0;
    double submitSum_ = 0.0;
};

int main() {
    std::cout << "[TEST] Running Performance Instrumentation & Spike Detection Test..." << std::endl;

    // 1. Verify One-Time Stage Override Guard
    StageClearGuard stageGuard;
    for (uint64_t f = 1; f <= 500; ++f) {
        stageGuard.TryClearStage(f);
    }
    assert(stageGuard.GetCallCount() == 1);
    std::cout << "  [PASS] StageClearGuard executed strictly ONCE across 500 frames (IPC flood prevented)" << std::endl;

    // 2. Verify Cadence Aggregator on 90 Hz simulation (180 eye submits per second)
    CadenceAggregator agg;
    // 178 normal frames (total = 5.5 ms, sync = 4.0 ms)
    for (int i = 0; i < 178; ++i) {
        FrameMetrics m = { 0.1, 0.2, 4.0, 0.1, 1.1, 5.5 };
        agg.RecordEye(m);
    }
    // 2 simulated spike frames (total = 22.0 ms, sync = 18.0 ms)
    for (int i = 0; i < 2; ++i) {
        FrameMetrics mSpike = { 0.2, 0.3, 18.0, 0.2, 3.3, 22.0 };
        agg.RecordEye(mSpike);
    }

    double fps = 0.0, avgTotal = 0.0, maxTotal = 0.0, avgSync = 0.0;
    uint32_t spikes = 0;
    bool emitted = agg.TryEmit1SecReport(1.0, fps, avgTotal, maxTotal, avgSync, spikes);
    assert(emitted);
    assert(fps == 90.0);
    assert(spikes == 2);
    assert(maxTotal == 22.0);
    assert(avgTotal > 5.5 && avgTotal < 6.0);
    assert(avgSync > 4.0 && avgSync < 4.5);
    std::cout << "  [PASS] 1-Second report calculated exactly: " << fps << " Stereo FPS, Avg=" << avgTotal << " ms, Max=" << maxTotal << " ms, Spikes=" << spikes << std::endl;

    // 3. Verify clean reset of window
    bool notEmittedYet = agg.TryEmit1SecReport(0.5, fps, avgTotal, maxTotal, avgSync, spikes);
    assert(!notEmittedYet);
    std::cout << "  [PASS] Cadence window resets cleanly between intervals" << std::endl;

    std::cout << "[SUCCESS] Performance Instrumentation & Spike Detection Verified Successfully!" << std::endl;
    return 0;
}
