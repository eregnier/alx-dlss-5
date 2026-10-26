#include <cassert>
#include <iostream>
#include "../include/d3d12_interop.hpp"

int main() {
    std::cout << "[TEST] Running test_d3d12_device..." << std::endl;

    auto& interop = hlx::D3D12Interop::Instance();
    bool init = interop.Init();
    if (!init) {
        std::cerr << "  [FAIL] D3D12Interop::Init() failed (no compatible D3D12 GPU found?)" << std::endl;
        return 1;
    }
    assert(interop.IsInitialized());
    assert(interop.GetDevice() != nullptr);
    assert(interop.GetCommandQueue() != nullptr);
    std::cout << "  [PASS] Private D3D12 Device and CommandQueue created" << std::endl;

    // Test creating a 2D texture (standard Alyx render target resolution ~3840x3840)
    hlx::ComPtr<ID3D12Resource> texture;
    bool texCreated = interop.CreateTexture(
        3840,
        3840,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_COMMON,
        texture,
        L"Test_Stereo_Eye_Texture"
    );
    assert(texCreated);
    assert(texture != nullptr);
    std::cout << "  [PASS] Committed 2D texture resource (3840x3840 RGBA8) successfully allocated" << std::endl;

    // Test GPU sync barrier / fence
    interop.WaitForGpu();
    std::cout << "  [PASS] GPU Fence signaled and synchronized without deadlock" << std::endl;

    interop.Shutdown();
    assert(!interop.IsInitialized());
    std::cout << "  [PASS] Clean D3D12 shutdown completed" << std::endl;

    std::cout << "[TEST] test_d3d12_device completed successfully!" << std::endl;
    return 0;
}
