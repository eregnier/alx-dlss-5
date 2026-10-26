#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <iostream>
#include <vector>

using NgxResult = uint32_t;
struct NgxHandle { uint32_t Id; };

struct NgxFeatureCommonInfo {
    struct { const wchar_t* const* paths{}; unsigned count{}; } path_list;
    void* internal_data{};
    struct {
        void (*callback)(const char*, int, int){};
        int minimum_level{};
        bool disable_other_sinks{};
    } logging;
};

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

using PFN_NVSDK_NGX_D3D12_Init_Ext = NgxResult(*)(uint64_t, const wchar_t*, ID3D12Device*, uint32_t, const void*);
using PFN_NVSDK_NGX_D3D12_AllocateParameters = NgxResult(*)(NgxParameters**);
using PFN_NVSDK_NGX_D3D12_DestroyParameters = NgxResult(*)(NgxParameters*);
using PFN_NVSDK_NGX_D3D12_CreateFeature = NgxResult(*)(ID3D12GraphicsCommandList*, uint32_t, NgxParameters*, NgxHandle**);
using PFN_NVSDK_NGX_D3D12_ReleaseFeature = NgxResult(*)(NgxHandle*);

int main() {
    std::cout << "[TEST] Testing Feature 18 Creation with CommonInfo Paths..." << std::endl;

    // Create D3D12 Device
    ID3D12Device* device = nullptr;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
    std::cout << "D3D12CreateDevice hr=0x" << std::hex << hr << " device=" << (void*)device << std::endl;
    if (FAILED(hr)) return 1;

    ID3D12CommandQueue* queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd = {};
    device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));

    ID3D12CommandAllocator* allocator = nullptr;
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));

    ID3D12GraphicsCommandList* cmdList = nullptr;
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&cmdList));

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

    auto pfnInit = reinterpret_cast<PFN_NVSDK_NGX_D3D12_Init_Ext>(GetProcAddress(hCore, "NVSDK_NGX_D3D12_Init_Ext"));
    auto pfnAlloc = reinterpret_cast<PFN_NVSDK_NGX_D3D12_AllocateParameters>(GetProcAddress(hCore, "NVSDK_NGX_D3D12_AllocateParameters"));
    auto pfnDestroy = reinterpret_cast<PFN_NVSDK_NGX_D3D12_DestroyParameters>(GetProcAddress(hCore, "NVSDK_NGX_D3D12_DestroyParameters"));
    auto pfnCreate = reinterpret_cast<PFN_NVSDK_NGX_D3D12_CreateFeature>(GetProcAddress(hCore, "NVSDK_NGX_D3D12_CreateFeature"));
    auto pfnRelease = reinterpret_cast<PFN_NVSDK_NGX_D3D12_ReleaseFeature>(GetProcAddress(hCore, "NVSDK_NGX_D3D12_ReleaseFeature"));

    const wchar_t* searchPaths[] = {
        L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Half-Life Alyx\\game\\bin\\win64",
        L"C:\\code\\CheekyFoveatedDLSS\\bin\\Release\\runtime-host-test-data\\5480-4341078\\runtime"
    };

    NgxFeatureCommonInfo common{};
    common.path_list.paths = searchPaths;
    common.path_list.count = 2;

    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);

    NgxResult res = pfnInit(0x0876232cULL, tempPath, device, 0x15, &common);
    std::cout << "Init_Ext result: 0x" << std::hex << res << std::endl;

    NgxParameters* params = nullptr;
    pfnAlloc(&params);

    const unsigned width = 2048;
    const unsigned height = 2048;
    for (const char* k : {"Width", "Height", "OutWidth", "OutHeight", "DLSSNR.Width", "DLSSNR.Height"}) {
        params->Set(k, width);
    }
    params->Set("DLSSNR.Enabled", 1U);
    params->Set("CreationNodeMask", 1U);
    params->Set("VisibilityNodeMask", 1U);
    params->Set("DLSSNR.Hint.Render.Preset", 0U);
    params->Set("DLSSNR.Intensity", 1.0f);
    params->Set("DLSSNR.LocalToneStrength", 1.0f);
    params->Set("DLSSNR.LocalStructureStrength", 1.0f);
    params->Set("DLSSNR.SkinStructureStrength", 1.0f);
    params->Set("DLSSNR.UseAutoMask", 1U);
    params->Set("DLSSNR.ControlMask", static_cast<void*>(nullptr));
    params->Set("DLSSNR.Style", 0U);
    params->Set("DLSSNR.UICorrection", 0U);

    NgxHandle* handle = nullptr;
    res = pfnCreate(cmdList, 18, params, &handle);
    std::cout << "pfnCreate(18) result: 0x" << std::hex << res << " handle=" << handle << std::endl;

    if (handle) {
        std::cout << "SUCCESS! Feature 18 Created successfully!" << std::endl;
        pfnRelease(handle);
    }
    pfnDestroy(params);
    return 0;
}
