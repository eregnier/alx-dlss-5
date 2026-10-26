#include <cassert>
#include <iostream>
#include <cstdint>
#include <algorithm>

struct VRTextureBounds_t {
    float uMin, vMin, uMax, vMax;
};

struct SimulatedEyeContext {
    uint32_t width = 0;
    uint32_t height = 0;
    bool isInitialized = false;
    bool needsReset = true;
};

struct D3D11Box {
    uint32_t left, top, front;
    uint32_t right, bottom, back;
};

bool ProcessEyeFrame(
    uint32_t atlasW, uint32_t atlasH,
    uint32_t targetW, uint32_t targetH,
    const VRTextureBounds_t& bounds,
    SimulatedEyeContext& eye,
    uint32_t& startX, uint32_t& startY,
    uint32_t& eyeWidth, uint32_t& eyeHeight,
    D3D11Box& srcBox, D3D11Box& outBox,
    float& mvScaleX, float& mvScaleY
) {
    float uSpan = bounds.uMax - bounds.uMin;
    float vSpan = bounds.vMax - bounds.vMin;

    startX = static_cast<uint32_t>(bounds.uMin * atlasW + 0.5f);
    startY = static_cast<uint32_t>(bounds.vMin * atlasH + 0.5f);
    eyeWidth = static_cast<uint32_t>(uSpan * atlasW + 0.5f);
    eyeHeight = static_cast<uint32_t>(vSpan * atlasH + 0.5f);

    eyeWidth = (eyeWidth + 1) & ~1u;
    eyeHeight = (eyeHeight + 1) & ~1u;

    // Loading stage pass-through: do not initialize Feature 18 on low-res loading screens
    if (!eye.isInitialized) {
        if (eyeWidth >= static_cast<uint32_t>(targetW * 0.90f)) {
            eye.width = targetW;
            eye.height = targetH;
            eye.isInitialized = true;
            eye.needsReset = true;
        } else {
            return false; // Passthrough to SteamVR without running DLSS 5
        }
    }

    // Temporary DRS dip: forward cleanly without destroying/recreating Feature 18
    if (eye.isInitialized && (eyeWidth < eye.width || eyeHeight < eye.height)) {
        return false; // Passthrough without reallocation stall
    }

    // Full 100% active eye coverage
    srcBox.left = startX;
    srcBox.top = startY;
    srcBox.front = 0;
    srcBox.right = startX + eyeWidth;
    srcBox.bottom = startY + eyeHeight;
    srcBox.back = 1;

    outBox.left = 0;
    outBox.top = 0;
    outBox.front = 0;
    outBox.right = eyeWidth;
    outBox.bottom = eyeHeight;
    outBox.back = 1;

    mvScaleX = 0.0f;
    mvScaleY = 0.0f;
    return true; // Enhanced with DLSS 5
}

int main() {
    std::cout << "[TEST] Running Viewport Adaptation & Reallocation Prevention Test..." << std::endl;

    const uint32_t atlasWidth = 8120;
    const uint32_t atlasHeight = 2708;
    const uint32_t targetWidth = 2872;
    const uint32_t targetHeight = 1914;

    SimulatedEyeContext eyeLeft;
    SimulatedEyeContext eyeRight;

    // Stage 1: Loading screen diorama (scale ~0.70x of recommended: 2030x1354)
    VRTextureBounds_t boundsLoadingLeft = { 0.000f, 0.000f, 0.250f, 0.499f };
    uint32_t sx, sy, ew, eh;
    D3D11Box sBox, oBox;
    float mvX, mvY;

    bool ranLoading = ProcessEyeFrame(atlasWidth, atlasHeight, targetWidth, targetHeight, boundsLoadingLeft, eyeLeft, sx, sy, ew, eh, sBox, oBox, mvX, mvY);
    assert(!ranLoading); // Correctly bypassed on loading screen
    assert(!eyeLeft.isInitialized); // Feature 18 not prematurely allocated
    std::cout << "  [PASS] Loading screen Eye 0 passed through cleanly (no stall, no premature allocation)" << std::endl;

    // Stage 2: Transition to Gameplay Full Scale (1.0x recommended: 2872x1914)
    VRTextureBounds_t boundsGameLeft = { 0.0000f, 0.0000f, 0.3536f, 0.7067f };
    VRTextureBounds_t boundsGameRight = { 0.3536f, 0.0000f, 0.7072f, 0.7067f };

    bool ranGame = ProcessEyeFrame(atlasWidth, atlasHeight, targetWidth, targetHeight, boundsGameLeft, eyeLeft, sx, sy, ew, eh, sBox, oBox, mvX, mvY);
    assert(ranGame);
    assert(eyeLeft.isInitialized);
    assert(eyeLeft.width == 2872);
    assert(eyeLeft.height == 1914);
    assert(oBox.right == 2872);
    assert(oBox.bottom == 1914);
    assert(mvX == 0.0f && mvY == 0.0f);
    std::cout << "  [PASS] Gameplay Eye 0 initialized permanently at 2872x1914 with 100% writeback coverage" << std::endl;

    // Stage 3: Simulated DRS momentary dip to 2030x1354
    bool ranDip = ProcessEyeFrame(atlasWidth, atlasHeight, targetWidth, targetHeight, boundsLoadingLeft, eyeLeft, sx, sy, ew, eh, sBox, oBox, mvX, mvY);
    assert(!ranDip); // Bypassed without stall
    assert(eyeLeft.isInitialized); // Feature 18 NOT destroyed!
    assert(eyeLeft.width == 2872); // Resolution preserved!
    std::cout << "  [PASS] Temporary DRS dip handled with zero-stall passthrough (Feature 18 preserved)" << std::endl;

    // Stage 4: Next frame returns to 2872x1914
    bool ranResume = ProcessEyeFrame(atlasWidth, atlasHeight, targetWidth, targetHeight, boundsGameLeft, eyeLeft, sx, sy, ew, eh, sBox, oBox, mvX, mvY);
    assert(ranResume);
    assert(eyeLeft.width == 2872);
    std::cout << "  [PASS] Gameplay resumed instantly on next frame without reallocation spike" << std::endl;

    // Right Eye Gameplay
    bool ranRight = ProcessEyeFrame(atlasWidth, atlasHeight, targetWidth, targetHeight, boundsGameRight, eyeRight, sx, sy, ew, eh, sBox, oBox, mvX, mvY);
    assert(ranRight);
    assert(eyeRight.width == 2872);
    assert(sx == 2871 || sx == 2872);
    std::cout << "  [PASS] Gameplay Eye 1 matches Eye 0 with exact stereo offset" << std::endl;

    std::cout << "[SUCCESS] Viewport Adaptation & Reallocation Prevention Test Passed!" << std::endl;
    return 0;
}
