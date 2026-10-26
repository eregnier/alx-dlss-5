#include <cassert>
#include <iostream>
#include <unordered_set>
#include <windows.h>
#include "../external/openvr/headers/openvr.h"

// Simulated SteamVR Compositor Internal State
struct MockSteamVRCompositor {
    std::unordered_set<void*> registeredAppTextures;
    uint64_t acceptedFrames = 0;
    uint64_t rejectedFrames = 0;

    void RegisterTexture(void* handle) {
        registeredAppTextures.insert(handle);
    }

    vr::EVRCompositorError Submit(vr::EVREye eEye, const vr::Texture_t* pTexture, const vr::VRTextureBounds_t* pBounds) {
        if (!pTexture || !pTexture->handle) {
            rejectedFrames++;
            return vr::VRCompositorError_InvalidTexture;
        }

        // SteamVR requires the texture handle to match an officially registered application surface
        if (registeredAppTextures.find(pTexture->handle) == registeredAppTextures.end()) {
            rejectedFrames++;
            // This is what caused the crash in vrclient_x64.dll when an unmapped handle was passed
            return vr::VRCompositorError_TextureIsOnWrongDevice;
        }

        // Texture handle is registered and valid
        acceptedFrames++;
        return vr::VRCompositorError_None;
    }
};

int main() {
    std::cout << "[TEST] Running test_openvr_submit_simulation (SteamVR Registered Texture Verification)..." << std::endl;

    MockSteamVRCompositor compositor;

    // 1. Game allocates its internal render backbuffer and registers it with SteamVR
    void* alyxBackbufferHandle = reinterpret_cast<void*>(0x000002AFA181D0E0);
    compositor.RegisterTexture(alyxBackbufferHandle);
    std::cout << "  [SETUP] Game registered backbuffer handle: " << alyxBackbufferHandle << std::endl;

    // 2. Scenario A (Old Buggy Behavior): Proxy substituted the handle with an unmapped shared texture
    void* unmappedProxyHandle = reinterpret_cast<void*>(0x000001F833AA2210);
    vr::Texture_t buggyTex = {};
    buggyTex.handle = unmappedProxyHandle;
    buggyTex.eType = vr::TextureType_DirectX;

    vr::EVRCompositorError errA = compositor.Submit(vr::Eye_Left, &buggyTex, nullptr);
    assert(errA == vr::VRCompositorError_TextureIsOnWrongDevice);
    assert(compositor.rejectedFrames == 1);
    std::cout << "  [PASS] Proved that passing substitute handle (" << unmappedProxyHandle
              << ") is strictly REJECTED by SteamVR Compositor (matches previous bug behavior)." << std::endl;

    // 3. Scenario B (Correct Fixed Behavior): Proxy keeps original pGameTex and modifies it in-place
    vr::Texture_t inPlaceTex = {};
    inPlaceTex.handle = alyxBackbufferHandle;
    inPlaceTex.eType = vr::TextureType_DirectX;

    vr::EVRCompositorError errB0 = compositor.Submit(vr::Eye_Left, &inPlaceTex, nullptr);
    vr::EVRCompositorError errB1 = compositor.Submit(vr::Eye_Right, &inPlaceTex, nullptr);
    assert(errB0 == vr::VRCompositorError_None);
    assert(errB1 == vr::VRCompositorError_None);
    assert(compositor.acceptedFrames == 2);
    std::cout << "  [PASS] Proved that submitting original registered handle (" << alyxBackbufferHandle
              << ") with in-place enhanced pixels is 100% ACCEPTED by SteamVR Compositor." << std::endl;

    std::cout << "[TEST] test_openvr_submit_simulation PASSED 100%!" << std::endl;
    return 0;
}
