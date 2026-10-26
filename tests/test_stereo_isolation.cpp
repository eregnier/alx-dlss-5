#include <cassert>
#include <iostream>
#include "../include/stereo_pipeline.hpp"

int main() {
    std::cout << "[TEST] Running test_stereo_isolation..." << std::endl;

    hlx::Config config;
    config.working_scale = 0.75f;
    config.sharpness = 0.5f;

    auto& pipeline = hlx::StereoPipeline::Instance();
    pipeline.Init(config);

    // Verify initial eye state
    auto& eye0 = pipeline.GetEye(0);
    auto& eye1 = pipeline.GetEye(1);

    assert(eye0.eyeIndex == 0);
    assert(eye1.eyeIndex == 1);
    assert(eye0.stats.totalFrames == 0);
    assert(eye1.stats.totalFrames == 0);
    std::cout << "  [PASS] Left (0) and Right (1) eyes distinctly addressed" << std::endl;

    // Simulate Left eye frame
    pipeline.RecordFrame(0, true, 1.45);
    assert(eye0.stats.totalFrames == 1);
    assert(eye0.stats.enhancedFrames == 1);
    assert(eye0.stats.fallbackFrames == 0);
    assert(eye1.stats.totalFrames == 0); // Eye 1 must remain untouched!
    std::cout << "  [PASS] Frame recorded on Eye 0 did not bleed into Eye 1" << std::endl;

    // Simulate Right eye frame (with fallback)
    pipeline.RecordFrame(1, false, 0.12);
    assert(eye1.stats.totalFrames == 1);
    assert(eye1.stats.enhancedFrames == 0);
    assert(eye1.stats.fallbackFrames == 1);
    assert(eye0.stats.totalFrames == 1); // Eye 0 remains at 1
    std::cout << "  [PASS] Frame recorded on Eye 1 did not bleed into Eye 0" << std::endl;

    // Test isolation of temporal reset
    eye0.isInitialized = true;
    eye1.isInitialized = true;
    pipeline.ResetEyeHistory(0);
    assert(!eye0.isInitialized);
    assert(eye1.isInitialized); // Eye 1 still initialized!
    std::cout << "  [PASS] History reset on Eye 0 kept Eye 1 intact" << std::endl;

    std::cout << "[TEST] test_stereo_isolation completed successfully!" << std::endl;
    return 0;
}
