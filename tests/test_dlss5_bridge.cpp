#include <cassert>
#include <iostream>
#include "../include/d3d12_interop.hpp"
#include "../include/dlss5_bridge.hpp"

int main() {
    std::cout << "[TEST] Running test_dlss5_bridge..." << std::endl;

    hlx::Config config;
    config.enabled = true;
    config.working_scale = 1.0f;

    auto& interop = hlx::D3D12Interop::Instance();
    bool d3d12Ok = interop.Init();
    assert(d3d12Ok);

    auto& bridge = hlx::DLSS5Bridge::Instance();
    bool bridgeOk = bridge.Init(interop.GetDevice(), config);
    assert(bridgeOk);
    assert(bridge.IsInitialized());
    std::cout << "  [PASS] DLSS5Bridge initialized with production forwarder and driver core" << std::endl;

    hlx::Logger::Instance().Init();
    bool feat0 = bridge.CreateEyeFeature(0, 512, 512);
    bool feat1 = bridge.CreateEyeFeature(1, 512, 512);
    std::cout << "CreateEyeFeature: Left=" << feat0 << ", Right=" << feat1 << std::endl;
    assert(feat0 && feat1);
    std::cout << "  [PASS] Stereo Feature 18 instances configured for Left and Right eyes" << std::endl;

    bridge.Shutdown();
    interop.Shutdown();
    std::cout << "[TEST] test_dlss5_bridge completed successfully!" << std::endl;
    return 0;
}
