#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <iostream>
#include <vector>

using NgxResult = uint32_t;
struct NgxHandle { uint32_t Id; };

struct NgxParameters {
    virtual void Set(const char*, unsigned long long) = 0;
    virtual void Set(const char*, float) = 0;
    virtual void Set(const char*, double) = 0;
    virtual void Set(const char*, unsigned int) = 0;
    virtual void Set(const char*, int) = 0;
    virtual void Set(const char*, ID3D11Resource*) = 0;
    virtual void Set(const char*, ID3D12Resource*) = 0;
    virtual void Set(const char*, void*) = 0;
    virtual NgxResult Get(const char*, unsigned long long*) const = 0;
    virtual NgxResult Get(const char*, float*) const = 0;
    virtual NgxResult Get(const char*, double*) const = 0;
    virtual NgxResult Get(const char*, unsigned int*) const = 0;
    virtual NgxResult Get(const char*, int*) const = 0;
    virtual NgxResult Get(const char*, ID3D11Resource**) const = 0;
    virtual NgxResult Get(const char*, ID3D12Resource**) const = 0;
    virtual NgxResult Get(const char*, void**) const = 0;
    virtual void Reset() = 0;
};

using NgxInitExtFn = NgxResult (*)(unsigned long long, const wchar_t*, ID3D12Device*, uint32_t, const void*);
using NgxAllocateParametersFn = NgxResult (*)(NgxParameters**);
using NgxDestroyParametersFn = NgxResult (*)(NgxParameters*);
using NgxCreateFeatureFn = NgxResult (*)(ID3D12GraphicsCommandList*, uint32_t, NgxParameters*, NgxHandle**);
using NgxEvaluateFeatureFn = NgxResult (*)(ID3D12GraphicsCommandList*, const NgxHandle*, const NgxParameters*, void*);
using NgxReleaseFeatureFn = NgxResult (*)(NgxHandle*);

int main() {
    std::cout << "[TEST] Running exact dlss_nr.cpp pattern against nvngx_dlssnr.dll..." << std::endl;

    // Load nvngx_dlssnr.dll
    const wchar_t* dllPath = L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Half-Life Alyx\\game\\bin\\win64\\nvngx_dlssnr.dll";
    const wchar_t* dllDir = L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Half-Life Alyx\\game\\bin\\win64";
    HMODULE hNR = LoadLibraryW(dllPath);
    if (!hNR) {
        std::wcout << L"Failed to load nvngx_dlssnr.dll from " << dllPath << L" error=" << GetLastError() << std::endl;
        return 1;
    }
    std::cout << "Loaded nvngx_dlssnr.dll at " << (void*)hNR << std::endl;

    // Load _nvngx.dll for parameters
    HKEY hKey = nullptr;
    wchar_t regPath[MAX_PATH] = {};
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD pathSize = sizeof(regPath);
        RegQueryValueExW(hKey, L"FullPath", nullptr, nullptr, reinterpret_cast<LPBYTE>(regPath), &pathSize);
        RegCloseKey(hKey);
    }
    std::wstring coreDll = std::wstring(regPath) + L"\\_nvngx.dll";
    HMODULE hCore = LoadLibraryW(coreDll.c_str());
    std::cout << "Loaded _nvngx.dll at " << (void*)hCore << std::endl;

    auto pfnInit = reinterpret_cast<NgxInitExtFn>(GetProcAddress(hNR, "NVSDK_NGX_D3D12_Init_Ext"));
    auto pfnAlloc = reinterpret_cast<NgxAllocateParametersFn>(GetProcAddress(hNR, "NVSDK_NGX_D3D12_AllocateParameters"));
    auto pfnDestroy = reinterpret_cast<NgxDestroyParametersFn>(GetProcAddress(hNR, "NVSDK_NGX_D3D12_DestroyParameters"));
    if (!pfnAlloc || !pfnDestroy) {
        pfnAlloc = reinterpret_cast<NgxAllocateParametersFn>(GetProcAddress(hCore, "NVSDK_NGX_D3D12_AllocateParameters"));
        pfnDestroy = reinterpret_cast<NgxDestroyParametersFn>(GetProcAddress(hCore, "NVSDK_NGX_D3D12_DestroyParameters"));
    }
    auto pfnCreate = reinterpret_cast<NgxCreateFeatureFn>(GetProcAddress(hNR, "NVSDK_NGX_D3D12_CreateFeature"));
    auto pfnRelease = reinterpret_cast<NgxReleaseFeatureFn>(GetProcAddress(hNR, "NVSDK_NGX_D3D12_ReleaseFeature"));
    auto pfnEvaluate = reinterpret_cast<NgxEvaluateFeatureFn>(GetProcAddress(hNR, "NVSDK_NGX_D3D12_EvaluateFeature"));

    std::cout << "pfnInit=" << (void*)pfnInit << " pfnAlloc=" << (void*)pfnAlloc << " pfnCreate=" << (void*)pfnCreate << std::endl;

    // Create D3D12 Device
    ID3D12Device* device = nullptr;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
    if (FAILED(hr)) return 1;

    ID3D12CommandQueue* queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd = {};
    device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));

    ID3D12CommandAllocator* allocator = nullptr;
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));

    ID3D12GraphicsCommandList* cmdList = nullptr;
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&cmdList));

    // Call NVSDK_NGX_D3D12_Init_Ext on nvngx_dlssnr.dll
    constexpr unsigned long long application_id = 0x0876232cULL;
    constexpr uint32_t ngx_sdk_version = 0x15U;
    NgxResult initRes = pfnInit(application_id, dllDir, device, ngx_sdk_version, nullptr);
    std::cout << "pfnInit on nvngx_dlssnr.dll result: 0x" << std::hex << initRes << std::endl;

    if ((initRes & 0xFFF00000U) != 0xBAD00000U) {
        NgxParameters* params = nullptr;
        pfnAlloc(&params);
        std::cout << "Allocated params: " << (void*)params << std::endl;

        const unsigned width = 2048;
        const unsigned height = 2048;
        params->Set("Width", width);
        params->Set("Height", height);
        params->Set("OutWidth", width);
        params->Set("OutHeight", height);
        params->Set("DLSSNR.Width", width);
        params->Set("DLSSNR.Height", height);
        params->Set("DLSSNR.InputWidth", width);
        params->Set("DLSSNR.InputHeight", height);
        params->Set("DLSSNR.OutputWidth", width);
        params->Set("DLSSNR.OutputHeight", height);
        params->Set("DLSSNR.Output.Width", width);
        params->Set("DLSSNR.Output.Height", height);
        params->Set("DLSSNR.ScalingRatio", 1.0F);
        params->Set("DLSSNR.Scale", 1.0F);
        params->Set("DLSSNR.Upscaling", 0U);
        params->Set("DLSSNR.Enabled", 1U);
        params->Set("DLSSNR.Hint.Render.Preset", 0U);
        params->Set("DLSSNR.Intensity", 1.0f);
        params->Set("DLSSNR.LocalToneStrength", 1.0f);
        params->Set("DLSSNR.LocalStructureStrength", 1.0f);
        params->Set("DLSSNR.SkinStructureStrength", 1.0f);
        params->Set("DLSSNR.ControlMask", static_cast<void*>(nullptr));
        params->Set("DLSSNR.UseAutoMask", 1U);
        params->Set("DLSSNR.Style", 0U);
        params->Set("DLSSNR.UICorrection", 0U);
        params->Set("CreationNodeMask", 1U);
        params->Set("VisibilityNodeMask", 1U);

        std::cout << "Calling pfnCreate..." << std::endl;
        NgxHandle* handle = nullptr;
        NgxResult cRes = pfnCreate(cmdList, 18, params, &handle);
        std::cout << "pfnCreate(18) result: 0x" << std::hex << cRes << " handle=" << (void*)handle << std::endl;

        if (handle) {
            std::cout << "SUCCESS! Production DLSS-NR 310.8 Feature 18 is alive and created!" << std::endl;
            pfnRelease(handle);
        }
        pfnDestroy(params);
    }
    return 0;
}
