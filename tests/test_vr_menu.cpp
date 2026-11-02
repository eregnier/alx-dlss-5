#include <iostream>
#include <cassert>
#include "../include/hlx_vr_menu.hpp"

int main() {
    std::cout << "[TEST] Running VRMenu unit test..." << std::endl;

    auto& menu = hlx::VRMenu::Instance();
    assert(!menu.IsVisible());

    // Show menu
    menu.Show(nullptr);
    assert(menu.IsVisible());

    // Hide menu
    menu.Hide(nullptr);
    assert(!menu.IsVisible());

    // Toggle menu
    menu.Toggle(nullptr);
    assert(menu.IsVisible());

    hlx::Config config;
    config.sharpness = 0.35f;
    config.detail_boost = 1.0f;
    config.foveated_width = 0.55f;

    // Test Navigation: starts on row 0 (Sharpness)
    // Verify clamping at top (cannot wrap backwards onto row 13)
    menu.NavigateUp();
    menu.Adjust(+1, config, nullptr);
    assert(config.sharpness >= 0.39f && config.sharpness <= 0.41f);

    menu.Adjust(-1, config, nullptr);
    assert(config.sharpness >= 0.34f && config.sharpness <= 0.36f);

    // Navigate down to row 1 (Detail Boost)
    menu.NavigateDown();
    menu.Adjust(+1, config, nullptr);
    assert(config.detail_boost >= 1.04f && config.detail_boost <= 1.06f);

    // Navigate down to row 2 (Foveated Width 0% - 100%)
    menu.NavigateDown();
    for (int i = 0; i < 20; ++i) {
        menu.Adjust(-1, config, nullptr);
    }
    assert(config.foveated_width == 0.0f); // Can go all the way down to 0%
    menu.Adjust(+1, config, nullptr);
    assert(config.foveated_width >= 0.049f && config.foveated_width <= 0.051f); // 5%
    config.foveated_width = 0.55f;

    // Navigate to row 14 (Passes) and test cycling 1 -> 2 -> 3 -> 1
    for (int i = 0; i < 12; ++i) {
        menu.NavigateDown();
    }
    assert(config.passes == 1);
    menu.Adjust(+1, config, nullptr);
    assert(config.passes == 2);
    menu.Adjust(+1, config, nullptr);
    assert(config.passes == 3);
    menu.Adjust(+1, config, nullptr);
    assert(config.passes == 1);

    // Navigate to row 15 (Eye Tracking DFR) and test toggle
    menu.NavigateDown();
    assert(config.eye_tracking == true);
    menu.Adjust(+1, config, nullptr);
    assert(config.eye_tracking == false);
    menu.Adjust(+1, config, nullptr);
    assert(config.eye_tracking == true);

    // Navigate to row 16 ([ FERMER LE MENU ])
    menu.NavigateDown();
    // Now on row 16, Adjust should close the menu
    menu.Adjust(+1, config, nullptr);
    assert(!menu.IsVisible());

    // Test GazeTracker Dynamic Foveated Rendering (Cheeky DFR)
    auto& gaze = hlx::GazeTracker::Instance();
    gaze.Reset();
    float u = 0.0f, v = 0.0f;
    assert(hlx::GazeTracker::NdcToUv(0.0f, 0.0f, u, v));
    assert(std::abs(u - 0.5f) < 1e-4f && std::abs(v - 0.5f) < 1e-4f);

    // Simulate eye looking top-right (U=0.75, V=0.25) on a 4000x3000 eye with a 25% (1000x750) fovea
    gaze.InjectTestGaze(0.75f, 0.25f, 0.75f, 0.25f, true, 1.000);
    auto p0 = gaze.ResolveEyePlacement(0, 4000, 3000, 1000, 750, 0.5f, 0.5f, config);
    assert(p0.usingGaze);
    assert(p0.needsHistoryReset); // First acquisition triggers history reset
    assert((p0.foveaX % 8) == 0 && (p0.foveaY % 8) == 0);
    assert(p0.foveaX > 1500 && p0.foveaY < 800);

    // Micro-tremor on next frame should NOT move quantized foveaX/Y or trigger reset
    gaze.InjectTestGaze(0.7505f, 0.2495f, 0.7505f, 0.2495f, true, 1.011);
    auto p1 = gaze.ResolveEyePlacement(0, 4000, 3000, 1000, 750, 0.5f, 0.5f, config);
    assert(p1.usingGaze);
    assert(!p1.needsHistoryReset);
    assert(p1.foveaX == p0.foveaX && p1.foveaY == p0.foveaY);

    // Blink (< 100ms) holds the last gaze position
    gaze.InjectTestGaze(0.0f, 0.0f, 0.0f, 0.0f, false, 1.060);
    auto pBlink = gaze.ResolveEyePlacement(0, 4000, 3000, 1000, 750, 0.5f, 0.5f, config);
    assert(pBlink.usingGaze);
    assert(pBlink.foveaX == p0.foveaX && pBlink.foveaY == p0.foveaY);

    // Large saccade across the screen triggers jump reset
    config.gaze_smoothing_ms = 0.0f;
    gaze.InjectTestGaze(0.20f, 0.80f, 0.20f, 0.80f, true, 1.080);
    auto pSaccade = gaze.ResolveEyePlacement(0, 4000, 3000, 1000, 750, 0.5f, 0.5f, config);
    assert(pSaccade.usingGaze);
    assert(pSaccade.needsHistoryReset);

    std::cout << "[TEST] VRMenu unit test passed successfully!" << std::endl;
    return 0;
}
