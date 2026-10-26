#include <cassert>
#include <iostream>
#include <fstream>
#include <windows.h>
#include "../include/hlx_config.hpp"

int main() {
    std::cout << "[TEST] Running test_config_resolution..." << std::endl;

    // 1. Determine module directory
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    std::wstring dir = (lastSlash ? std::wstring(exePath, lastSlash + 1) : L".\\");
    std::wstring testIni = dir + L"test_hlx_config.ini";

    // 2. Write an explicit config with Enabled=0, WorkingScale=0.75, DetailBoost=1.5
    {
        std::ofstream iniFile(testIni);
        iniFile << "[HLX_DLSS5]\n";
        iniFile << "Enabled=0\n";
        iniFile << "WorkingScale=0.75\n";
        iniFile << "Sharpness=0.45\n";
        iniFile << "DetailBoost=1.5\n";
        iniFile << "FallbackOnError=1\n";
    }

    // 3. Test Load with relative filename (should resolve to module dir, NOT C:\Windows)
    hlx::Config config;
    config.Load(L"test_hlx_config.ini");

    std::cout << "  Loaded Config: Enabled=" << config.enabled
              << ", WorkingScale=" << config.working_scale
              << ", Sharpness=" << config.sharpness
              << ", DetailBoost=" << config.detail_boost << std::endl;

    assert(config.enabled == false);
    assert(std::abs(config.working_scale - 0.75f) < 0.001f);
    assert(std::abs(config.sharpness - 0.45f) < 0.001f);
    assert(std::abs(config.detail_boost - 1.50f) < 0.001f);
    std::cout << "  [PASS] Config successfully resolved and parsed from local module directory (Enabled=0 strictly honored)." << std::endl;

    // 4. Update file to Enabled=1, reload and verify
    {
        std::ofstream iniFile(testIni);
        iniFile << "[HLX_DLSS5]\n";
        iniFile << "Enabled=1\n";
        iniFile << "WorkingScale=0.50\n";
    }
    config.Load(L"test_hlx_config.ini");
    assert(config.enabled == true);
    assert(std::abs(config.working_scale - 0.50f) < 0.001f);
    std::cout << "  [PASS] Config reload successfully reflected Enabled=1 and WorkingScale=0.50." << std::endl;

    // Clean up
    DeleteFileW(testIni.c_str());
    std::cout << "[TEST] test_config_resolution PASSED 100%!" << std::endl;
    return 0;
}
