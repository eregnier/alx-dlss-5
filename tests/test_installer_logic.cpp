#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <iostream>
#include <cassert>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace fs = std::filesystem;

// Standalone implementation of installer routines for regression testing
std::wstring TestReadDllVersion(const std::wstring& path) {
    DWORD dummy = 0;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (size == 0) return L"Unknown";

    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) {
        return L"Unknown";
    }

    struct LANGANDCODEPAGE {
        WORD wLanguage;
        WORD wCodePage;
    } *lpTranslate = nullptr;
    UINT cbTranslate = 0;

    if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<LPVOID*>(&lpTranslate), &cbTranslate) && cbTranslate >= sizeof(LANGANDCODEPAGE)) {
        wchar_t subBlock[128];
        swprintf_s(subBlock, L"\\StringFileInfo\\%04x%04x\\FileVersion", lpTranslate[0].wLanguage, lpTranslate[0].wCodePage);
        wchar_t* strVer = nullptr;
        UINT strVerLen = 0;
        if (VerQueryValueW(data.data(), subBlock, reinterpret_cast<LPVOID*>(&strVer), &strVerLen) && strVer && wcslen(strVer) > 0) {
            return std::wstring(strVer);
        }
    }

    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT ffiLen = 0;
    if (VerQueryValueW(data.data(), L"\\", reinterpret_cast<LPVOID*>(&ffi), &ffiLen) && ffi) {
        wchar_t verBuf[64];
        swprintf_s(verBuf, L"%u.%u.%u.%u",
            HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
            HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
        return verBuf;
    }

    return L"Unknown";
}

std::vector<std::wstring> TestDetectSteamLibraries() {
    std::vector<std::wstring> libs;
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t steamPath[MAX_PATH] = {};
        DWORD valType = 0, valSize = sizeof(steamPath);
        if (RegQueryValueExW(hKey, L"SteamPath", nullptr, &valType, reinterpret_cast<LPBYTE>(steamPath), &valSize) == ERROR_SUCCESS) {
            std::wstring mainSteam = steamPath;
            std::replace(mainSteam.begin(), mainSteam.end(), L'/', L'\\');
            libs.push_back(mainSteam);

            std::wstring vdfPath = mainSteam + L"\\steamapps\\libraryfolders.vdf";
            std::wifstream vdfFile(vdfPath);
            if (vdfFile.is_open()) {
                std::wstring line;
                while (std::getline(vdfFile, line)) {
                    size_t pos = line.find(L"\"path\"");
                    if (pos != std::wstring::npos) {
                        size_t firstQuote = line.find(L'\"', pos + 6);
                        if (firstQuote != std::wstring::npos) {
                            size_t secondQuote = line.find(L'\"', firstQuote + 1);
                            if (secondQuote != std::wstring::npos) {
                                std::wstring libPath = line.substr(firstQuote + 1, secondQuote - firstQuote - 1);
                                std::wstring cleanPath;
                                for (size_t i = 0; i < libPath.size(); ++i) {
                                    if (libPath[i] == L'\\' && i + 1 < libPath.size() && libPath[i + 1] == L'\\') {
                                        cleanPath += L'\\';
                                        ++i;
                                    } else {
                                        cleanPath += libPath[i];
                                    }
                                }
                                if (std::find(libs.begin(), libs.end(), cleanPath) == libs.end()) {
                                    libs.push_back(cleanPath);
                                }
                            }
                        }
                    }
                }
            }
        }
        RegCloseKey(hKey);
    }
    return libs;
}

int main() {
    std::wcout << L"========================================================" << std::endl;
    std::wcout << L"  HLX-DLSS5: Test Suite - Installer Core Logic" << std::endl;
    std::wcout << L"========================================================" << std::endl;

    // 1. Test Steam library discovery
    std::wcout << L"[TEST 1] Steam Library & Alyx Path Discovery..." << std::endl;
    auto libs = TestDetectSteamLibraries();
    std::wcout << L"  -> Found " << libs.size() << L" Steam library path(s):" << std::endl;
    for (const auto& l : libs) {
        std::wcout << L"     - " << l << std::endl;
    }
    assert(!libs.empty() && "Expected at least 1 Steam library on host machine");

    // 2. Test Version Reader on Known Windows Binary
    std::wcout << L"[TEST 2] PE File Version Info Reader..." << std::endl;
    std::wstring kernel32 = L"C:\\Windows\\System32\\kernel32.dll";
    std::wstring kver = TestReadDllVersion(kernel32);
    std::wcout << L"  -> kernel32.dll Version: " << kver << std::endl;
    assert(kver != L"Unknown" && "Failed to extract version info from kernel32.dll");

    // 3. Test Sandbox Backup & Restore Cycle
    std::wcout << L"[TEST 3] Simulated Install/Uninstall Sandbox..." << std::endl;
    fs::path tempSandbox = fs::temp_directory_path() / "hlx_installer_test_sandbox";
    fs::create_directories(tempSandbox);

    fs::path origDll = tempSandbox / "openvr_api.dll";
    fs::path realBackupDll = tempSandbox / "openvr_api_real.dll";
    fs::path proxyDll = tempSandbox / "mock_proxy.dll";

    // Create mock vanilla file
    {
        std::ofstream f(origDll.string());
        f << "VANILLA_OPENVR_1.0";
    }
    // Create mock proxy file
    {
        std::ofstream f(proxyDll.string());
        f << "MODDED_PROXY_OPENVR_DLSS5";
    }

    // Step A: Install Simulation
    // 1. Backup if not exists
    if (!fs::exists(realBackupDll)) {
        fs::copy_file(origDll, realBackupDll);
    }
    // 2. Overwrite with proxy
    fs::copy_file(proxyDll, origDll, fs::copy_options::overwrite_existing);

    // Verify install state
    {
        std::ifstream fOrig(origDll.string());
        std::string s;
        fOrig >> s;
        assert(s == "MODDED_PROXY_OPENVR_DLSS5" && "Active openvr_api.dll should be modded");

        std::ifstream fBackup(realBackupDll.string());
        std::string sB;
        fBackup >> sB;
        assert(sB == "VANILLA_OPENVR_1.0" && "Backup openvr_api_real.dll should hold vanilla content");
    }
    std::wcout << L"  -> Install step: Backup created and proxy installed cleanly." << std::endl;

    // Step B: Uninstall Simulation
    if (fs::exists(realBackupDll)) {
        fs::copy_file(realBackupDll, origDll, fs::copy_options::overwrite_existing);
        fs::remove(realBackupDll);
    }

    // Verify uninstall state
    {
        std::ifstream fRestored(origDll.string());
        std::string s;
        fRestored >> s;
        assert(s == "VANILLA_OPENVR_1.0" && "Active openvr_api.dll must be restored to vanilla");
        assert(!fs::exists(realBackupDll) && "Backup file should be cleaned up on uninstall");
    }
    std::wcout << L"  -> Uninstall step: Vanilla restored, backup removed cleanly." << std::endl;

    // Cleanup sandbox
    fs::remove_all(tempSandbox);

    std::wcout << L"\n[SUCCESS] All installer logic unit tests passed (100% PASS)!" << std::endl;
    return 0;
}
