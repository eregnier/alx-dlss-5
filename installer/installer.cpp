#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

// Enable Modern Common Controls (v6)
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace fs = std::filesystem;

// Custom Window Messages
#define WM_USER_LOG             (WM_USER + 101)
#define WM_USER_SEARCH_DONE     (WM_USER + 102)
#define WM_USER_SEARCH_PROGRESS (WM_USER + 103)

// Control IDs
#define IDC_EDIT_TARGET_PATH    2001
#define IDC_BTN_BROWSE_TARGET   2002
#define IDC_BTN_SEARCH_PC       2003
#define IDC_BTN_BROWSE_DLL      2004
#define IDC_BTN_HELP_DLL        2005
#define IDC_LIST_MATCHES        2006
#define IDC_BTN_INSTALL         2007
#define IDC_BTN_UNINSTALL       2008
#define IDC_BTN_LAUNCH_STEAM    2009
#define IDC_BTN_EXIT            2010
#define IDC_EDIT_LOG            2011

struct DlssMatch {
    std::wstring path;
    std::wstring version;
    uint64_t sizeBytes = 0;
};

// Global App State
static HWND g_hWnd = nullptr;
static HWND g_hEditTarget = nullptr;
static HWND g_hLblTargetStatus = nullptr;
static HWND g_hLblDllStatus = nullptr;
static HWND g_hListMatches = nullptr;
static HWND g_hEditLog = nullptr;
static HWND g_hBtnInstall = nullptr;
static HWND g_hBtnUninstall = nullptr;
static HFONT g_hFontNormal = nullptr;
static HFONT g_hFontBold = nullptr;
static HFONT g_hFontTitle = nullptr;
static HBRUSH g_hBrushBg = nullptr;

static std::wstring g_targetDir = L"";
static std::wstring g_selectedDllPath = L"";
static std::vector<DlssMatch> g_matches;
static std::mutex g_matchesMutex;
static std::atomic<bool> g_isSearching(false);

// Forward declarations
void AppendLog(const std::wstring& text);
void UpdateTargetValidation();
void UpdateDllStatus();

// Helper: Get installer executable directory
std::wstring GetInstallerDir() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(buf, L'\\');
    if (lastSlash) *(lastSlash + 1) = L'\0';
    return std::wstring(buf);
}

// Helper: Format bytes to MB
std::wstring FormatSizeMB(uint64_t bytes) {
    double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    wchar_t buf[32];
    swprintf_s(buf, L"%.1f MB", mb);
    return buf;
}

// Helper: Read a string from PE Version StringTable
std::wstring QueryStringTableValue(BYTE* data, WORD lang, WORD cp, const wchar_t* key) {
    wchar_t subBlock[128];
    swprintf_s(subBlock, L"\\StringFileInfo\\%04x%04x\\%s", lang, cp, key);
    wchar_t* strVal = nullptr;
    UINT strLen = 0;
    if (VerQueryValueW(data, subBlock, reinterpret_cast<LPVOID*>(&strVal), &strLen) && strVal && strLen > 0) {
        return std::wstring(strVal);
    }
    return L"";
}

// Helper: Read PE File Version Info
std::wstring ReadDllVersion(const std::wstring& path) {
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
        std::wstring strVer = QueryStringTableValue(data.data(), lpTranslate[0].wLanguage, lpTranslate[0].wCodePage, L"FileVersion");
        if (!strVer.empty()) return strVer;
    }

    // Fallback to fixed file info
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

// Helper: Rigorous DLSS-NR file (nvngx_dlssnr.dll) validation
// Guarantees:
// 1. Valid file size between 100 MB and 250 MB (U-Net model weights are ~165 MB, distinct from DLSS SR ~58 MB and RR ~48 MB)
// 2. CompanyName is NVIDIA
// 3. ProductName / InternalName / Description explicitly contains "DLSSNR" (Neural Reconstruction)
// 4. FileVersion belongs to DLSS 5 branch (310.8.x / 310.x)
bool ValidateDlssNrFile(const std::wstring& path, DlssMatch& outMatch) {
    std::error_code ec;
    uint64_t size = fs::file_size(path, ec);
    if (ec || size < 100ULL * 1024 * 1024 || size > 250ULL * 1024 * 1024) {
        return false;
    }

    DWORD dummy = 0;
    DWORD verSize = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (verSize == 0) return false;

    std::vector<BYTE> data(verSize);
    if (!GetFileVersionInfoW(path.c_str(), 0, verSize, data.data())) return false;

    struct LANGANDCODEPAGE {
        WORD wLanguage;
        WORD wCodePage;
    } *lpTranslate = nullptr;
    UINT cbTranslate = 0;

    std::wstring company = L"";
    std::wstring product = L"";
    std::wstring internalName = L"";
    std::wstring desc = L"";
    std::wstring fileVer = L"";

    if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<LPVOID*>(&lpTranslate), &cbTranslate) && cbTranslate >= sizeof(LANGANDCODEPAGE)) {
        WORD lang = lpTranslate[0].wLanguage;
        WORD cp   = lpTranslate[0].wCodePage;
        company      = QueryStringTableValue(data.data(), lang, cp, L"CompanyName");
        product      = QueryStringTableValue(data.data(), lang, cp, L"ProductName");
        internalName = QueryStringTableValue(data.data(), lang, cp, L"InternalName");
        desc         = QueryStringTableValue(data.data(), lang, cp, L"FileDescription");
        fileVer      = QueryStringTableValue(data.data(), lang, cp, L"FileVersion");
    }

    // Fallback to fixed file info for version if string not found
    if (fileVer.empty()) {
        VS_FIXEDFILEINFO* ffi = nullptr;
        UINT ffiLen = 0;
        if (VerQueryValueW(data.data(), L"\\", reinterpret_cast<LPVOID*>(&ffi), &ffiLen) && ffi) {
            wchar_t verBuf[64];
            swprintf_s(verBuf, L"%u.%u.%u.%u",
                HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
                HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
            fileVer = verBuf;
        }
    }

    // 1. Verify Company is NVIDIA
    if (company.find(L"NVIDIA") == std::wstring::npos) {
        return false;
    }

    // 2. Verify it is DLSSNR (Neural Reconstruction), NOT DLSS SR or DLSSD RR
    bool isDlssNr = (product.find(L"DLSSNR") != std::wstring::npos) ||
                    (internalName.find(L"DLSSNR") != std::wstring::npos) ||
                    (desc.find(L"DLSSNR") != std::wstring::npos);
    if (!isDlssNr) {
        return false;
    }

    // 3. Verify Version is 310.x (DLSS 5 generation)
    if (fileVer.find(L"310.") != 0) {
        return false;
    }

    outMatch.path = path;
    outMatch.version = fileVer;
    outMatch.sizeBytes = size;
    return true;
}

// Steam Library & Alyx Auto-Detector
std::vector<std::wstring> DetectSteamLibraries() {
    std::vector<std::wstring> libs;
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t steamPath[MAX_PATH] = {};
        DWORD valType = 0, valSize = sizeof(steamPath);
        if (RegQueryValueExW(hKey, L"SteamPath", nullptr, &valType, reinterpret_cast<LPBYTE>(steamPath), &valSize) == ERROR_SUCCESS) {
            std::wstring mainSteam = steamPath;
            std::replace(mainSteam.begin(), mainSteam.end(), L'/', L'\\');
            libs.push_back(mainSteam);

            // Parse libraryfolders.vdf
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
                                // Unescape double backslashes
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

// Auto-detect Alyx installation directory
std::wstring AutoDetectAlyxDir() {
    auto libs = DetectSteamLibraries();
    for (const auto& lib : libs) {
        std::wstring candidate = lib + L"\\steamapps\\common\\Half-Life Alyx\\game\\bin\\win64";
        std::wstring exeCheck = candidate + L"\\hlvr.exe";
        if (GetFileAttributesW(exeCheck.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return candidate;
        }
    }
    return L"";
}

// Quick Steam scan for nvngx_dlssnr.dll
void QuickSteamDlssScan(std::vector<DlssMatch>& results) {
    auto libs = DetectSteamLibraries();
    for (const auto& lib : libs) {
        std::wstring common = lib + L"\\steamapps\\common";
        if (GetFileAttributesW(common.c_str()) == INVALID_FILE_ATTRIBUTES) continue;

        try {
            for (const auto& entry : fs::directory_iterator(common, fs::directory_options::skip_permission_denied)) {
                if (entry.is_directory()) {
                    // Check top-level and common bin subdirs (e.g. Cyberpunk 2077\bin\x64, etc.)
                    std::vector<std::wstring> checks = {
                        entry.path().wstring() + L"\\nvngx_dlssnr.dll",
                        entry.path().wstring() + L"\\bin\\x64\\nvngx_dlssnr.dll",
                        entry.path().wstring() + L"\\bin\\win64\\nvngx_dlssnr.dll",
                        entry.path().wstring() + L"\\game\\bin\\win64\\nvngx_dlssnr.dll"
                    };
                    for (const auto& p : checks) {
                        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) {
                            DlssMatch match;
                            if (ValidateDlssNrFile(p, match)) {
                                bool already = false;
                                for (const auto& r : results) {
                                    if (_wcsicmp(r.path.c_str(), match.path.c_str()) == 0) { already = true; break; }
                                }
                                if (!already) results.push_back(match);
                            }
                        }
                    }
                }
            }
        } catch (...) {}
    }
}

// Deep disk search in background thread
void DeepDiskSearchThread() {
    g_isSearching.store(true);
    std::vector<DlssMatch> results;

    // 1. Quick Steam pass
    QuickSteamDlssScan(results);

    // Also check alongside installer
    std::wstring localDll = GetInstallerDir() + L"nvngx_dlssnr.dll";
    if (GetFileAttributesW(localDll.c_str()) != INVALID_FILE_ATTRIBUTES) {
        DlssMatch match;
        if (ValidateDlssNrFile(localDll, match)) {
            results.push_back(match);
        }
    }

    // 2. Scan fixed drives
    wchar_t driveStrings[512] = {};
    if (GetLogicalDriveStringsW(512, driveStrings)) {
        const wchar_t* drive = driveStrings;
        while (*drive && g_isSearching.load()) {
            UINT type = GetDriveTypeW(drive);
            if (type == DRIVE_FIXED) {
                std::wstring driveRoot = drive;
                try {
                    for (const auto& it : fs::recursive_directory_iterator(
                             driveRoot,
                             fs::directory_options::skip_permission_denied)) {
                        if (!g_isSearching.load()) break;

                        // Skip Windows system and recycle directories for speed and safety
                        auto pathStr = it.path().wstring();
                        if (pathStr.find(L"\\Windows") != std::wstring::npos ||
                            pathStr.find(L"\\$Recycle.Bin") != std::wstring::npos ||
                            pathStr.find(L"\\System Volume Information") != std::wstring::npos ||
                            pathStr.find(L"\\AppData\\Local\\Temp") != std::wstring::npos) {
                            continue;
                        }

                        if (it.is_regular_file()) {
                            if (_wcsicmp(it.path().filename().c_str(), L"nvngx_dlssnr.dll") == 0) {
                                DlssMatch match;
                                if (ValidateDlssNrFile(pathStr, match)) {
                                    bool exists = false;
                                    for (const auto& r : results) {
                                        if (_wcsicmp(r.path.c_str(), match.path.c_str()) == 0) { exists = true; break; }
                                    }
                                    if (!exists) {
                                        results.push_back(match);
                                        std::wstring foundMsg = L"Found: " + match.path + L" [" + match.version + L"]";
                                        AppendLog(foundMsg);
                                    }
                                }
                            }
                        }
                    }
                } catch (...) {}
            }
            drive += wcslen(drive) + 1;
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_matchesMutex);
        g_matches = results;
    }

    g_isSearching.store(false);
    PostMessageW(g_hWnd, WM_USER_SEARCH_DONE, 0, 0);
}

// Append text to read-only multiline log edit control
void AppendLog(const std::wstring& text) {
    if (!g_hEditLog) return;
    std::wstring line = text + L"\r\n";
    int len = GetWindowTextLengthW(g_hEditLog);
    SendMessageW(g_hEditLog, EM_SETSEL, len, len);
    SendMessageW(g_hEditLog, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
    SendMessageW(g_hEditLog, WM_VSCROLL, SB_BOTTOM, 0);
}

// Update Target Validation Status
void UpdateTargetValidation() {
    wchar_t buf[MAX_PATH] = {};
    GetWindowTextW(g_hEditTarget, buf, MAX_PATH);
    g_targetDir = buf;

    if (g_targetDir.empty()) {
        SetWindowTextW(g_hLblTargetStatus, L"Status: Please specify Half-Life: Alyx game/bin/win64 directory.");
        EnableWindow(g_hBtnInstall, FALSE);
        return;
    }

    std::wstring exeCheck = g_targetDir + L"\\hlvr.exe";
    bool isValid = (GetFileAttributesW(exeCheck.c_str()) != INVALID_FILE_ATTRIBUTES);

    if (isValid) {
        SetWindowTextW(g_hLblTargetStatus, L"Status: Valid Half-Life: Alyx directory detected (hlvr.exe found).");
        EnableWindow(g_hBtnInstall, TRUE);
    } else {
        SetWindowTextW(g_hLblTargetStatus, L"Status: Warning: hlvr.exe not found in this folder. Make sure it points to game/bin/win64.");
        EnableWindow(g_hBtnInstall, FALSE);
    }

    UpdateDllStatus();
}

// Update DLSS 5 DLL Status Label
void UpdateDllStatus() {
    std::wstring inGameDll = g_targetDir + L"\\nvngx_dlssnr.dll";
    bool inGame = (GetFileAttributesW(inGameDll.c_str()) != INVALID_FILE_ATTRIBUTES);

    if (inGame) {
        std::wstring ver = ReadDllVersion(inGameDll);
        std::error_code ec;
        uint64_t sz = fs::file_size(inGameDll, ec);
        std::wstring status = L"Current in Game: Installed | Version " + ver + L" (" + FormatSizeMB(sz) + L")";
        SetWindowTextW(g_hLblDllStatus, status.c_str());
    } else if (!g_selectedDllPath.empty()) {
        std::wstring ver = ReadDllVersion(g_selectedDllPath);
        std::error_code ec;
        uint64_t sz = fs::file_size(g_selectedDllPath, ec);
        std::wstring status = L"Ready to Copy: " + ver + L" (" + FormatSizeMB(sz) + L") from " + g_selectedDllPath;
        SetWindowTextW(g_hLblDllStatus, status.c_str());
    } else {
        SetWindowTextW(g_hLblDllStatus, L"Status: Missing nvngx_dlssnr.dll. Click 'Search on my PC' or 'Select File...'.");
    }
}

// Browse for folder using Windows Shell
std::wstring BrowseFolder(HWND hParent, const std::wstring& title) {
    std::wstring result = L"";
    IFileDialog* pfd = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd)))) {
        DWORD dwOptions = 0;
        pfd->GetOptions(&dwOptions);
        pfd->SetOptions(dwOptions | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        pfd->SetTitle(title.c_str());
        if (SUCCEEDED(pfd->Show(hParent))) {
            IShellItem* psi = nullptr;
            if (SUCCEEDED(pfd->GetResult(&psi))) {
                PWSTR pszPath = nullptr;
                if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath))) {
                    result = pszPath;
                    CoTaskMemFree(pszPath);
                }
                psi->Release();
            }
        }
        pfd->Release();
    }
    return result;
}

// Browse for nvngx_dlssnr.dll file
std::wstring BrowseDlssDll(HWND hParent) {
    wchar_t filename[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hParent;
    ofn.lpstrFilter = L"NVIDIA DLSS 5 Neural Model (nvngx_dlssnr.dll)\0nvngx_dlssnr.dll\0All DLL Files (*.dll)\0*.dll\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Select nvngx_dlssnr.dll";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_ENABLESIZING;

    if (GetOpenFileNameW(&ofn)) {
        return filename;
    }
    return L"";
}

// Perform Mod Installation
void PerformInstall() {
    if (g_targetDir.empty()) {
        MessageBoxW(g_hWnd, L"Please select a target directory first.", L"Error", MB_ICONERROR);
        return;
    }

    std::wstring instDir = GetInstallerDir();
    AppendLog(L"--- Starting Installation ---");
    AppendLog(L"Target: " + g_targetDir);

    // 1. Check required installer files
    std::wstring proxySrc = instDir + L"openvr_api.dll";
    std::wstring forwarderSrc = instDir + L"nvngx.dll_dlssnr.dll";
    std::wstring iniSrc = instDir + L"hlx_dlss5.ini";

    // If running in development tree (build/dist fallback)
    if (GetFileAttributesW(proxySrc.c_str()) == INVALID_FILE_ATTRIBUTES) {
        proxySrc = instDir + L"..\\build\\openvr_api.dll";
    }
    if (GetFileAttributesW(forwarderSrc.c_str()) == INVALID_FILE_ATTRIBUTES) {
        forwarderSrc = instDir + L"..\\external\\forwarder\\nvngx.dll_dlssnr.dll";
        if (GetFileAttributesW(forwarderSrc.c_str()) == INVALID_FILE_ATTRIBUTES) {
            forwarderSrc = instDir + L"..\\bin\\nvngx.dll_dlssnr.dll";
        }
    }
    if (GetFileAttributesW(iniSrc.c_str()) == INVALID_FILE_ATTRIBUTES) {
        iniSrc = instDir + L"..\\hlx_dlss5.ini";
    }

    if (GetFileAttributesW(proxySrc.c_str()) == INVALID_FILE_ATTRIBUTES) {
        AppendLog(L"[ERROR] Proxy openvr_api.dll not found in installer package!");
        MessageBoxW(g_hWnd, L"openvr_api.dll is missing from the installer package.", L"Error", MB_ICONERROR);
        return;
    }

    // 2. Backup vanilla openvr_api.dll
    std::wstring realDllDst = g_targetDir + L"\\openvr_api_real.dll";
    std::wstring activeDll = g_targetDir + L"\\openvr_api.dll";

    if (GetFileAttributesW(realDllDst.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (GetFileAttributesW(activeDll.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (CopyFileW(activeDll.c_str(), realDllDst.c_str(), TRUE)) {
                AppendLog(L"[OK] Backed up vanilla openvr_api.dll -> openvr_api_real.dll");
            } else {
                AppendLog(L"[ERROR] Failed to backup vanilla openvr_api.dll!");
                MessageBoxW(g_hWnd, L"Failed to backup original openvr_api.dll. Check permissions.", L"Error", MB_ICONERROR);
                return;
            }
        }
    } else {
        AppendLog(L"[INFO] Existing openvr_api_real.dll preserved.");
    }

    // 3. Copy proxy openvr_api.dll
    if (CopyFileW(proxySrc.c_str(), activeDll.c_str(), FALSE)) {
        AppendLog(L"[OK] Deployed DLSS 5 OpenVR Proxy: openvr_api.dll");
    } else {
        AppendLog(L"[ERROR] Failed to write openvr_api.dll into game folder.");
        MessageBoxW(g_hWnd, L"Failed to copy openvr_api.dll. Ensure the game is closed.", L"Error", MB_ICONERROR);
        return;
    }

    // 4. Copy forwarder nvngx.dll_dlssnr.dll
    if (GetFileAttributesW(forwarderSrc.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::wstring forwarderDst = g_targetDir + L"\\nvngx.dll_dlssnr.dll";
        if (CopyFileW(forwarderSrc.c_str(), forwarderDst.c_str(), FALSE)) {
            AppendLog(L"[OK] Deployed NVIDIA signature forwarder: nvngx.dll_dlssnr.dll");
        }
    }

    // 5. Copy configuration hlx_dlss5.ini (do not overwrite if exists to protect user custom settings)
    std::wstring iniDst = g_targetDir + L"\\hlx_dlss5.ini";
    if (GetFileAttributesW(iniDst.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (GetFileAttributesW(iniSrc.c_str()) != INVALID_FILE_ATTRIBUTES) {
            CopyFileW(iniSrc.c_str(), iniDst.c_str(), FALSE);
            AppendLog(L"[OK] Deployed default settings: hlx_dlss5.ini");
        }
    } else {
        AppendLog(L"[INFO] Existing hlx_dlss5.ini kept to preserve your VR preferences.");
    }

    // 6. Copy selected DLSS 5 Neural Model nvngx_dlssnr.dll if available
    std::wstring nrDst = g_targetDir + L"\\nvngx_dlssnr.dll";
    if (!g_selectedDllPath.empty() && _wcsicmp(g_selectedDllPath.c_str(), nrDst.c_str()) != 0) {
        AppendLog(L"Copying DLSS 5 model (~165 MB) from: " + g_selectedDllPath);
        if (CopyFileW(g_selectedDllPath.c_str(), nrDst.c_str(), FALSE)) {
            AppendLog(L"[OK] Successfully installed DLSS 5 model: nvngx_dlssnr.dll");
        } else {
            AppendLog(L"[WARNING] Failed to copy nvngx_dlssnr.dll. You can place it manually.");
        }
    }

    UpdateDllStatus();
    AppendLog(L"[SUCCESS] HLX-DLSS5 is installed and ready to play!");
    MessageBoxW(g_hWnd,
        L"HLX-DLSS5 has been successfully installed!\n\n"
        L"Launch Half-Life: Alyx via Steam.\n\n"
        L"In VR:\n"
        L"• Left D-Pad + Right Trigger (or Right Stick Up) to open the VR Menu.\n"
        L"• Left D-Pad + Right Grip for quick DLSS 5 toggle ON/OFF.",
        L"Installation Complete", MB_ICONINFORMATION);
}

// Perform Mod Uninstallation (Vanilla Restore)
void PerformUninstall() {
    if (g_targetDir.empty()) return;

    std::wstring realDll = g_targetDir + L"\\openvr_api_real.dll";
    std::wstring activeDll = g_targetDir + L"\\openvr_api.dll";
    std::wstring forwarderDll = g_targetDir + L"\\nvngx.dll_dlssnr.dll";
    std::wstring logFile = g_targetDir + L"\\hlx_dlss5.log";

    AppendLog(L"--- Restoring Vanilla Configuration ---");

    if (GetFileAttributesW(realDll.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (CopyFileW(realDll.c_str(), activeDll.c_str(), FALSE)) {
            DeleteFileW(realDll.c_str());
            AppendLog(L"[OK] Restored vanilla openvr_api.dll");
        } else {
            AppendLog(L"[ERROR] Failed to restore openvr_api.dll. Close the game and retry.");
            return;
        }
    } else {
        AppendLog(L"[INFO] openvr_api_real.dll not present. Mod may already be uninstalled.");
    }

    DeleteFileW(forwarderDll.c_str());
    DeleteFileW(logFile.c_str());
    AppendLog(L"[OK] Cleaned up proxy and logs.");
    AppendLog(L"[SUCCESS] Half-Life: Alyx is restored to vanilla state.");

    UpdateDllStatus();
    MessageBoxW(g_hWnd, L"Half-Life: Alyx has been restored to vanilla OpenVR.", L"Uninstalled", MB_ICONINFORMATION);
}

// Launch Alyx via Steam URL protocol
void LaunchAlyxSteam() {
    ShellExecuteW(nullptr, L"open", L"steam://run/546560", nullptr, nullptr, SW_SHOWNORMAL);
    AppendLog(L"Sent Steam launch command for Half-Life: Alyx (AppID 546560).");
}

// Show where to find DLSS 5 model guidance dialog
void ShowDllHelpDialog(HWND hParent) {
    const wchar_t* msg =
        L"How to obtain NVIDIA DLSS 5 Neural Model (nvngx_dlssnr.dll):\n\n"
        L"1. Cyberpunk 2077 or DLSS 5 enabled games:\n"
        L"   Copy 'nvngx_dlssnr.dll' (~165 MB) from:\n"
        L"   Cyberpunk 2077\\bin\\x64\\nvngx_dlssnr.dll\n\n"
        L"2. Automated PC Search:\n"
        L"   Click 'Search on my PC'. The installer will scan your Steam libraries\n"
        L"   and local drives, then let you select which version to copy.\n\n"
        L"3. Manual Placement:\n"
        L"   If you downloaded the file separately, click 'Select File...' to point to it,\n"
        L"   or place it directly inside:\n"
        L"   Half-Life Alyx\\game\\bin\\win64\\nvngx_dlssnr.dll\n\n"
        L"Note: NVIDIA neural models are proprietary and cannot be bundled in open-source releases.";
    MessageBoxW(hParent, msg, L"DLSS 5 Model Information", MB_ICONINFORMATION);
}

// Window Procedure
LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_CREATE: {
        g_hWnd = hWnd;

        // Fonts
        g_hFontNormal = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        g_hFontBold = CreateFontW(16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        g_hFontTitle = CreateFontW(22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        g_hBrushBg = CreateSolidBrush(RGB(245, 246, 248));

        int x = 20, y = 15;

        // Title Header
        HWND hTitle = CreateWindowW(L"STATIC", L"HLX-DLSS5 — NVIDIA DLSS 5 for Half-Life: Alyx",
            WS_CHILD | WS_VISIBLE | SS_LEFT, x, y, 640, 26, hWnd, nullptr, nullptr, nullptr);
        SendMessageW(hTitle, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontTitle), TRUE);

        HWND hSub = CreateWindowW(L"STATIC", L"Non-intrusive OpenVR Proxy & Dynamic Foveated Neural Reconstruction (RTX Series)",
            WS_CHILD | WS_VISIBLE | SS_LEFT, x, y + 28, 640, 20, hWnd, nullptr, nullptr, nullptr);
        SendMessageW(hSub, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontNormal), TRUE);

        y += 58;

        // 1. Target Directory Section
        HWND hGrpTarget = CreateWindowW(L"BUTTON", L"1. Target Game Directory",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX, x, y, 645, 88, hWnd, nullptr, nullptr, nullptr);
        SendMessageW(hGrpTarget, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontBold), TRUE);

        g_hEditTarget = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, x + 15, y + 26, 505, 24, hWnd, reinterpret_cast<HMENU>(IDC_EDIT_TARGET_PATH), nullptr, nullptr);
        SendMessageW(g_hEditTarget, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontNormal), TRUE);

        HWND hBtnBrowse = CreateWindowW(L"BUTTON", L"Browse...",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, x + 530, y + 25, 100, 26, hWnd, reinterpret_cast<HMENU>(IDC_BTN_BROWSE_TARGET), nullptr, nullptr);
        SendMessageW(hBtnBrowse, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontNormal), TRUE);

        g_hLblTargetStatus = CreateWindowW(L"STATIC", L"Status: Detecting Half-Life: Alyx...",
            WS_CHILD | WS_VISIBLE | SS_LEFT, x + 15, y + 58, 615, 18, hWnd, nullptr, nullptr, nullptr);
        SendMessageW(g_hLblTargetStatus, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontNormal), TRUE);

        y += 98;

        // 2. DLSS 5 Model Acquisition Section
        HWND hGrpDll = CreateWindowW(L"BUTTON", L"2. DLSS 5 Neural Model (nvngx_dlssnr.dll ~165 MB)",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX, x, y, 645, 150, hWnd, nullptr, nullptr, nullptr);
        SendMessageW(hGrpDll, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontBold), TRUE);

        g_hLblDllStatus = CreateWindowW(L"STATIC", L"Status: Checking...",
            WS_CHILD | WS_VISIBLE | SS_LEFT, x + 15, y + 24, 615, 20, hWnd, nullptr, nullptr, nullptr);
        SendMessageW(g_hLblDllStatus, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontBold), TRUE);

        HWND hBtnSearch = CreateWindowW(L"BUTTON", L"Search on my PC",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, x + 15, y + 48, 140, 28, hWnd, reinterpret_cast<HMENU>(IDC_BTN_SEARCH_PC), nullptr, nullptr);
        SendMessageW(hBtnSearch, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontNormal), TRUE);

        HWND hBtnBrowseDll = CreateWindowW(L"BUTTON", L"Select File...",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, x + 165, y + 48, 120, 28, hWnd, reinterpret_cast<HMENU>(IDC_BTN_BROWSE_DLL), nullptr, nullptr);
        SendMessageW(hBtnBrowseDll, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontNormal), TRUE);

        HWND hBtnHelpDll = CreateWindowW(L"BUTTON", L"Where to get it?",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, x + 295, y + 48, 130, 28, hWnd, reinterpret_cast<HMENU>(IDC_BTN_HELP_DLL), nullptr, nullptr);
        SendMessageW(hBtnHelpDll, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontNormal), TRUE);

        // Listbox for matches
        g_hListMatches = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL | WS_BORDER,
            x + 15, y + 82, 615, 56, hWnd, reinterpret_cast<HMENU>(IDC_LIST_MATCHES), nullptr, nullptr);
        SendMessageW(g_hListMatches, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontNormal), TRUE);

        y += 160;

        // 3. Activity Log Box
        HWND hGrpLog = CreateWindowW(L"BUTTON", L"3. Installation Progress & Details",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX, x, y, 645, 125, hWnd, nullptr, nullptr, nullptr);
        SendMessageW(hGrpLog, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontBold), TRUE);

        g_hEditLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            x + 15, y + 22, 615, 92, hWnd, reinterpret_cast<HMENU>(IDC_EDIT_LOG), nullptr, nullptr);
        SendMessageW(g_hEditLog, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontNormal), TRUE);

        y += 135;

        // 4. Action Buttons
        g_hBtnInstall = CreateWindowW(L"BUTTON", L"Install / Update Mod",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, x, y, 160, 32, hWnd, reinterpret_cast<HMENU>(IDC_BTN_INSTALL), nullptr, nullptr);
        SendMessageW(g_hBtnInstall, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontBold), TRUE);

        g_hBtnUninstall = CreateWindowW(L"BUTTON", L"Uninstall (Vanilla Restore)",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, x + 170, y, 180, 32, hWnd, reinterpret_cast<HMENU>(IDC_BTN_UNINSTALL), nullptr, nullptr);
        SendMessageW(g_hBtnUninstall, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontNormal), TRUE);

        HWND hBtnSteam = CreateWindowW(L"BUTTON", L"Launch Alyx via Steam",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, x + 360, y, 160, 32, hWnd, reinterpret_cast<HMENU>(IDC_BTN_LAUNCH_STEAM), nullptr, nullptr);
        SendMessageW(hBtnSteam, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontNormal), TRUE);

        HWND hBtnExit = CreateWindowW(L"BUTTON", L"Close",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, x + 535, y, 110, 32, hWnd, reinterpret_cast<HMENU>(IDC_BTN_EXIT), nullptr, nullptr);
        SendMessageW(hBtnExit, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontNormal), TRUE);

        // Auto-detect Alyx directory
        std::wstring autoAlyx = AutoDetectAlyxDir();
        if (!autoAlyx.empty()) {
            SetWindowTextW(g_hEditTarget, autoAlyx.c_str());
            AppendLog(L"Auto-detected Half-Life: Alyx in Steam library: " + autoAlyx);
        } else {
            AppendLog(L"Could not auto-detect Half-Life: Alyx. Please click 'Browse...' to select game/bin/win64.");
        }

        UpdateTargetValidation();
        break;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);

        if (id == IDC_EDIT_TARGET_PATH && code == EN_CHANGE) {
            UpdateTargetValidation();
        } else if (id == IDC_BTN_BROWSE_TARGET) {
            std::wstring folder = BrowseFolder(hWnd, L"Select Half-Life Alyx/game/bin/win64 Directory");
            if (!folder.empty()) {
                SetWindowTextW(g_hEditTarget, folder.c_str());
                UpdateTargetValidation();
            }
        } else if (id == IDC_BTN_SEARCH_PC) {
            if (g_isSearching.load()) {
                MessageBoxW(hWnd, L"A disk search is already in progress.", L"Searching...", MB_ICONINFORMATION);
            } else {
                SendMessageW(g_hListMatches, LB_RESETCONTENT, 0, 0);
                AppendLog(L"Searching Steam libraries and drives for nvngx_dlssnr.dll...");
                AppendLog(L"(Full disk search may take 1 to 3 minutes, running in background)");
                SetWindowTextW(g_hLblDllStatus, L"Status: Searching PC drives for nvngx_dlssnr.dll...");
                std::thread(DeepDiskSearchThread).detach();
            }
        } else if (id == IDC_BTN_BROWSE_DLL) {
            std::wstring file = BrowseDlssDll(hWnd);
            if (!file.empty()) {
                DlssMatch match;
                if (ValidateDlssNrFile(file, match)) {
                    g_selectedDllPath = file;
                    AppendLog(L"Selected DLSS 5 model: " + file + L" [" + match.version + L"] (" + FormatSizeMB(match.sizeBytes) + L")");
                    UpdateDllStatus();
                } else {
                    MessageBoxW(hWnd, L"The selected file does not appear to be a valid NVIDIA DLSS 5 model (file size too small or missing version).", L"Invalid File", MB_ICONWARNING);
                }
            }
        } else if (id == IDC_BTN_HELP_DLL) {
            ShowDllHelpDialog(hWnd);
        } else if (id == IDC_LIST_MATCHES && code == LBN_SELCHANGE) {
            int sel = static_cast<int>(SendMessageW(g_hListMatches, LB_GETCURSEL, 0, 0));
            if (sel != LB_ERR) {
                std::lock_guard<std::mutex> lock(g_matchesMutex);
                if (sel >= 0 && sel < static_cast<int>(g_matches.size())) {
                    g_selectedDllPath = g_matches[sel].path;
                    AppendLog(L"Selected DLSS 5 source: " + g_selectedDllPath);
                    UpdateDllStatus();
                }
            }
        } else if (id == IDC_BTN_INSTALL) {
            PerformInstall();
        } else if (id == IDC_BTN_UNINSTALL) {
            PerformUninstall();
        } else if (id == IDC_BTN_LAUNCH_STEAM) {
            LaunchAlyxSteam();
        } else if (id == IDC_BTN_EXIT) {
            PostQuitMessage(0);
        }
        break;
    }

    case WM_USER_SEARCH_DONE: {
        std::lock_guard<std::mutex> lock(g_matchesMutex);
        SendMessageW(g_hListMatches, LB_RESETCONTENT, 0, 0);

        if (g_matches.empty()) {
            AppendLog(L"Search complete: No nvngx_dlssnr.dll found on local drives.");
            SetWindowTextW(g_hLblDllStatus, L"Status: No DLSS 5 model found on PC. Click 'Where to get it?' for guidance.");
            ShowDllHelpDialog(hWnd);
        } else {
            AppendLog(L"Search complete: Found " + std::to_wstring(g_matches.size()) + L" candidate(s).");
            for (size_t i = 0; i < g_matches.size(); ++i) {
                const auto& m = g_matches[i];
                std::wstring itemText = L"v" + m.version + L" (" + FormatSizeMB(m.sizeBytes) + L") — " + m.path;
                SendMessageW(g_hListMatches, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(itemText.c_str()));
            }

            // Auto-select the first valid match
            SendMessageW(g_hListMatches, LB_SETCURSEL, 0, 0);
            g_selectedDllPath = g_matches[0].path;
            UpdateDllStatus();
            AppendLog(L"Auto-selected best match: " + g_selectedDllPath);
        }
        break;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkColor(hdc, RGB(245, 246, 248));
        return reinterpret_cast<INT_PTR>(g_hBrushBg);
    }

    case WM_DESTROY: {
        g_isSearching.store(false);
        if (g_hFontNormal) DeleteObject(g_hFontNormal);
        if (g_hFontBold) DeleteObject(g_hFontBold);
        if (g_hFontTitle) DeleteObject(g_hFontTitle);
        if (g_hBrushBg) DeleteObject(g_hBrushBg);
        PostQuitMessage(0);
        break;
    }

    default:
        return DefWindowProcW(hWnd, uMsg, wParam, lParam);
    }
    return 0;
}

// WinMain Entry Point
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icex);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"HLX_DLSS5_Installer_Class";
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    wc.hbrBackground = CreateSolidBrush(RGB(245, 246, 248));
    wc.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));   // IDI_APPLICATION

    if (!RegisterClassExW(&wc)) {
        return 1;
    }

    // Center window on primary screen
    int winW = 700;
    int winH = 560;
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int posX = std::max(0, (screenW - winW) / 2);
    int posY = std::max(0, (screenH - winH) / 2);

    HWND hWnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        wc.lpszClassName,
        L"HLX-DLSS5 — NVIDIA DLSS 5 Installer for Half-Life: Alyx",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        posX, posY, winW, winH,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!hWnd) {
        return 1;
    }

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
