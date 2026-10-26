#include <cassert>
#include <iostream>
#include "../include/openvr_hook.hpp"
#include "../include/hlx_config.hpp"

// Mock class mimicking IVRSystem where GetRecommendedRenderTargetSize is slot 0
class MockVRSystem {
public:
    virtual void GetRecommendedRenderTargetSize(uint32_t* pnWidth, uint32_t* pnHeight) {
        callCount++;
        if (pnWidth) *pnWidth = originalWidth;
        if (pnHeight) *pnHeight = originalHeight;
    }
    virtual void OtherMethod() { otherCallCount++; }

    int callCount = 0;
    int otherCallCount = 0;
    uint32_t originalWidth = 5720;
    uint32_t originalHeight = 3817;
};

static hlx::VTableHook g_testHook;
static hlx::PFN_VRSystem_GetRecommendedRenderTargetSize g_testOrig = nullptr;
static float g_testWorkingScale = 0.50f;
static bool g_testEnabled = true;

static void TestDetour_GetRecommendedRenderTargetSize(void* thisPtr, uint32_t* pnWidth, uint32_t* pnHeight) {
    if (g_testOrig) {
        g_testOrig(thisPtr, pnWidth, pnHeight);
    }
    if (pnWidth && pnHeight) {
        uint32_t origW = *pnWidth;
        uint32_t origH = *pnHeight;

        if (g_testEnabled && g_testWorkingScale > 0.1f && g_testWorkingScale < 0.999f) {
            *pnWidth = static_cast<uint32_t>(origW * g_testWorkingScale);
            *pnHeight = static_cast<uint32_t>(origH * g_testWorkingScale);

            // Ensure even dimensions
            *pnWidth = (*pnWidth + 1) & ~1u;
            *pnHeight = (*pnHeight + 1) & ~1u;
        }
    }
}

int main() {
    std::cout << "[TEST] Running test_render_target_scale..." << std::endl;

    MockVRSystem vrSystem;
    uint32_t w = 0, h = 0;

    // 1. Initial unhooked call
    vrSystem.GetRecommendedRenderTargetSize(&w, &h);
    assert(vrSystem.callCount == 1);
    assert(w == 5720);
    assert(h == 3817);
    std::cout << "  [PASS] Unhooked call returned raw dimensions: " << w << "x" << h << std::endl;

    // 2. Install hook on slot 0
    void** vtable = *reinterpret_cast<void***>(&vrSystem);
    bool hookOk = g_testHook.Install(
        &vtable[0],
        reinterpret_cast<void*>(&TestDetour_GetRecommendedRenderTargetSize),
        reinterpret_cast<void**>(&g_testOrig)
    );
    assert(hookOk);
    assert(g_testHook.IsInstalled());
    std::cout << "  [PASS] Hook installed on IVRSystem slot 0" << std::endl;

    // 3. Test WorkingScale = 0.50 (Performance mode, e.g. Pimax Dream Air)
    g_testWorkingScale = 0.50f;
    g_testEnabled = true;
    w = 0; h = 0;
    vrSystem.GetRecommendedRenderTargetSize(&w, &h);
    assert(vrSystem.callCount == 2);
    // 5720 * 0.50 = 2860 (even)
    // 3817 * 0.50 = 1908.5 -> 1908 -> (+1) & ~1 = 1908
    assert(w == 2860);
    assert(h == 1908);
    std::cout << "  [PASS] WorkingScale 0.50 correctly scaled: " << w << "x" << h << " (even dimensions verified)" << std::endl;

    // 4. Test WorkingScale = 0.67 (Quality mode)
    g_testWorkingScale = 0.67f;
    w = 0; h = 0;
    vrSystem.GetRecommendedRenderTargetSize(&w, &h);
    assert(vrSystem.callCount == 3);
    // 5720 * 0.67 = 3832.4 -> 3832
    // 3817 * 0.67 = 2557.39 -> 2557 -> (+1) & ~1 = 2558 (even)
    assert(w % 2 == 0);
    assert(h % 2 == 0);
    std::cout << "  [PASS] WorkingScale 0.67 correctly scaled: " << w << "x" << h << " (even dimensions verified)" << std::endl;

    // 5. Test WorkingScale = 1.0 (Native resolution passthrough)
    g_testWorkingScale = 1.0f;
    w = 0; h = 0;
    vrSystem.GetRecommendedRenderTargetSize(&w, &h);
    assert(vrSystem.callCount == 4);
    assert(w == 5720);
    assert(h == 3817);
    std::cout << "  [PASS] WorkingScale 1.0 preserved native dimensions: " << w << "x" << h << std::endl;

    // 6. Test Disabled mode
    g_testEnabled = false;
    g_testWorkingScale = 0.50f;
    w = 0; h = 0;
    vrSystem.GetRecommendedRenderTargetSize(&w, &h);
    assert(vrSystem.callCount == 5);
    assert(w == 5720);
    assert(h == 3817);
    std::cout << "  [PASS] Enabled=false bypassed scaling: " << w << "x" << h << std::endl;

    // 7. Restore hook
    g_testHook.Restore();
    assert(!g_testHook.IsInstalled());
    w = 0; h = 0;
    vrSystem.GetRecommendedRenderTargetSize(&w, &h);
    assert(vrSystem.callCount == 6);
    assert(w == 5720);
    assert(h == 3817);
    std::cout << "  [PASS] Hook restored, returned raw dimensions: " << w << "x" << h << std::endl;

    std::cout << "[SUCCESS] test_render_target_scale passed completely!" << std::endl;
    return 0;
}
