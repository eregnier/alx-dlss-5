#include <cassert>
#include <iostream>
#include "../include/openvr_hook.hpp"

// Mock class with virtual methods mimicking IVRCompositor
class MockCompositor {
public:
    virtual void Method0() { callCount0++; }
    virtual void Method1() { callCount1++; }
    virtual void Method2() { callCount2++; }
    virtual void Method3() { callCount3++; }
    virtual void Method4() { callCount4++; }
    virtual void Method5() { callCount5++; }
    virtual int Submit(int eye, void* texture) { 
        submitCallCount++;
        lastEye = eye;
        lastTexture = texture;
        return 100; // Original return code
    }

    int callCount0 = 0;
    int callCount1 = 0;
    int callCount2 = 0;
    int callCount3 = 0;
    int callCount4 = 0;
    int callCount5 = 0;
    int submitCallCount = 0;
    int lastEye = -1;
    void* lastTexture = nullptr;
};

using PFN_MockSubmit = int(*)(void* thisPtr, int eye, void* texture);
static PFN_MockSubmit g_origSubmit = nullptr;
static int g_detourCallCount = 0;
static int g_detourLastEye = -1;

static int Detour_MockSubmit(void* thisPtr, int eye, void* texture) {
    g_detourCallCount++;
    g_detourLastEye = eye;
    // Call original
    if (g_origSubmit) {
        return g_origSubmit(thisPtr, eye, texture) + 5; // Return modified code to prove detour ran
    }
    return 0;
}

int main() {
    std::cout << "[TEST] Running test_vtable_hook..." << std::endl;

    MockCompositor compositor;
    void* dummyTex = reinterpret_cast<void*>(0x12345678);

    // Call without hook
    int res0 = compositor.Submit(0, dummyTex);
    assert(res0 == 100);
    assert(compositor.submitCallCount == 1);
    assert(g_detourCallCount == 0);
    std::cout << "  [PASS] Unhooked call returned expected value (100)" << std::endl;

    // Install hook on slot 6
    void** vtable = *reinterpret_cast<void***>(&compositor);
    void** submitSlot = &vtable[6];

    hlx::VTableHook hook;
    bool installed = hook.Install(submitSlot, reinterpret_cast<void*>(&Detour_MockSubmit), reinterpret_cast<void**>(&g_origSubmit));
    assert(installed);
    assert(hook.IsInstalled());
    std::cout << "  [PASS] VTableHook installed on slot 6" << std::endl;

    // Call with hook
    int res1 = compositor.Submit(1, dummyTex);
    assert(res1 == 105); // Detour ran and added 5
    assert(g_detourCallCount == 1);
    assert(g_detourLastEye == 1);
    assert(compositor.submitCallCount == 2);
    std::cout << "  [PASS] Hooked call intercepted successfully (returned 105, Eye=1)" << std::endl;

    // Restore hook
    hook.Restore();
    assert(!hook.IsInstalled());

    int res2 = compositor.Submit(0, dummyTex);
    assert(res2 == 100);
    assert(g_detourCallCount == 1); // Detour was NOT called
    assert(compositor.submitCallCount == 3);
    std::cout << "  [PASS] Hook restored, subsequent call bypassed detour (returned 100)" << std::endl;

    std::cout << "[TEST] test_vtable_hook completed successfully!" << std::endl;
    return 0;
}
