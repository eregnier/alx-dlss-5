#include <windows.h>
#include <d3d11.h>
#include <iostream>

using NgxResult = uint32_t;
struct NgxHandle { uint32_t Id; };
struct NgxParameters;

using PFN_NVSDK_NGX_D3D11_Init = NgxResult(*)(unsigned long long, const wchar_t*, ID3D11Device*, const void*, unsigned);
using PFN_NVSDK_NGX_D3D11_AllocateParameters = NgxResult(*)(NgxParameters**);
using PFN_NVSDK_NGX_D3D11_DestroyParameters = NgxResult(*)(NgxParameters*);
using PFN_NVSDK_NGX_D3D11_CreateFeature = NgxResult(*)(ID3D11DeviceContext*, unsigned, NgxParameters*, NgxHandle**);
using PFN_NVSDK_NGX_D3D11_EvaluateFeature = NgxResult(*)(ID3D11DeviceContext*, const NgxHandle*, const NgxParameters*, void*);
using PFN_NVSDK_NGX_D3D11_ReleaseFeature = NgxResult(*)(NgxHandle*);
using PFN_NVSDK_NGX_D3D11_Shutdown1 = NgxResult(*)(ID3D11Device*);

int main() {
    std::cout << "[D3D11 NGX TEST] Testing D3D11 NGX Feature 18..." << std::endl;

    // Create D3D11 device
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &device,
        &featureLevel,
        &context
    );

    if (FAILED(hr) || !device) {
        std::cout << "Failed to create D3D11 device: " << std::hex << hr << std::endl;
        return 1;
    }
    std::cout << "D3D11 device created successfully!" << std::endl;

    // Load _nvngx.dll
    HKEY hKey = nullptr;
    wchar_t regPath[MAX_PATH] = {};
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD pathSize = sizeof(regPath);
        RegQueryValueExW(hKey, L"FullPath", nullptr, nullptr, reinterpret_cast<LPBYTE>(regPath), &pathSize);
        RegCloseKey(hKey);
    }

    std::wstring coreDll = std::wstring(regPath) + L"\\_nvngx.dll";
    HMODULE hCore = LoadLibraryW(coreDll.c_str());
    if (!hCore) {
        std::cout << "Failed to load _nvngx.dll" << std::endl;
        return 1;
    }
    std::cout << "Loaded _nvngx.dll" << std::endl;

    auto pfnInit = reinterpret_cast<PFN_NVSDK_NGX_D3D11_Init>(GetProcAddress(hCore, "NVSDK_NGX_D3D11_Init"));
    auto pfnAlloc = reinterpret_cast<PFN_NVSDK_NGX_D3D11_AllocateParameters>(GetProcAddress(hCore, "NVSDK_NGX_D3D11_AllocateParameters"));
    auto pfnDestroy = reinterpret_cast<PFN_NVSDK_NGX_D3D11_DestroyParameters>(GetProcAddress(hCore, "NVSDK_NGX_D3D11_DestroyParameters"));
    auto pfnCreate = reinterpret_cast<PFN_NVSDK_NGX_D3D11_CreateFeature>(GetProcAddress(hCore, "NVSDK_NGX_D3D11_CreateFeature"));
    auto pfnRelease = reinterpret_cast<PFN_NVSDK_NGX_D3D11_ReleaseFeature>(GetProcAddress(hCore, "NVSDK_NGX_D3D11_ReleaseFeature"));

    std::cout << "pfnInit=" << (void*)pfnInit << " pfnAlloc=" << (void*)pfnAlloc << " pfnCreate=" << (void*)pfnCreate << std::endl;

    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    NgxResult res = pfnInit(0x1337BEEF, tempPath, device, nullptr, 0x15);
    std::cout << "NVSDK_NGX_D3D11_Init result: 0x" << std::hex << res << std::endl;

    if ((res & 0xFFF00000U) != 0xBAD00000U) {
        NgxParameters* params = nullptr;
        pfnAlloc(&params);
        std::cout << "Allocated params: " << (void*)params << std::endl;

        // Try Feature 18
        NgxHandle* handle = nullptr;
        NgxResult cRes = pfnCreate(context, 1, params, &handle);
        std::cout << "NVSDK_NGX_D3D11_CreateFeature(18) result: 0x" << std::hex << cRes << " handle=" << (void*)handle << std::endl;

        if (handle) {
            pfnRelease(handle);
        }
        if (params) {
            pfnDestroy(params);
        }
    }

    context->Release();
    device->Release();
    return 0;
}
