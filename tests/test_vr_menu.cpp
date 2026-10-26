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

    // Navigate to row 13 ([ FERMER LE MENU ])
    for (int i = 0; i < 12; ++i) {
        menu.NavigateDown();
    }
    // Now on row 13, Adjust should close the menu
    menu.Adjust(+1, config, nullptr);
    assert(!menu.IsVisible());

    std::cout << "[TEST] VRMenu unit test passed successfully!" << std::endl;
    return 0;
}
