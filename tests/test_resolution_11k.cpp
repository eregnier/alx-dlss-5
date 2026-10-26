#include <windows.h>
#include <d3d12.h>
#include <iostream>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "advapi32.lib")

using PFN_dlssnr_call_create = void*(*)(
    const wchar_t *snippetPath, const wchar_t *dataPath,
    ID3D12Device *device, ID3D12GraphicsCommandList *cmd,
    void *capabilityParams, unsigned int width,
    unsigned int height, int preset, float intensity,
    int style, float localStructure, float localTone,
    float skinStructure, int useAutoMask,
    int uiCorrection
);

using PFN_dlssnr_call_evaluate_v2 = int(*)(
    ID3D12GraphicsCommandList *cmd, void *feature,
    void *capabilityParams, ID3D12Resource *color,
    ID3D12Resource *depth, ID3D12Resource *motion,
    ID3D12Resource *output, unsigned int width,
    unsigned int height, unsigned int guideWidth,
    unsigned int guideHeight, unsigned int motionWidth,
    unsigned int motionHeight, unsigned int depthBaseX,
    unsigned int depthBaseY, unsigned int motionBaseX,
    unsigned int motionBaseY, int depthInverted, int reset,
    float intensity, int style, float localStructure,
    float localTone, float skinStructure, int useAutoMask,
    float mvScaleX, float mvScaleY
);

using PFN_dlssnr_call_release = void(*)(void* feature);
using PFN_dlssnr_call_set_float_slot = void(*)(int slot);
using PFN_dlssnr_call_error = const char*(*)();

using PFN_NVSDK_NGX_D3D12_Init_Ext = uint32_t(*)(uint64_t, const wchar_t*, ID3D12Device*, const void*, void*);
using PFN_NVSDK_NGX_D3D12_GetCapabilityParameters = uint32_t(*)(void**);

int RunEval(PFN_dlssnr_call_evaluate_v2 pfnEvaluate, ID3D12GraphicsCommandList* cmdList, void* feature, void* capParams, ID3D12Resource* colorRes, ID3D12Resource* depthRes, ID3D12Resource* motionRes, ID3D12Resource* outRes, unsigned width, unsigned height) {
    __try {
        return pfnEvaluate(
            cmdList, feature, capParams,
            colorRes, depthRes, motionRes, outRes,
            width, height,
            512, 512,
            512, 512,
            0, 0, 0, 0,
            0, 1, 1.0f, 0, 1.0f, 1.0f, 1.0f, 1,
            float(width), float(height)
        );
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -999;
    }
}

int main() {
    std::cout << "[TEST] Testing DLSS-NR at Alyx resolution 11440x3817 on RTX 5090..." << std::endl;

    const unsigned width = 11440;
    const unsigned height = 3817;

    ID3D12Device* device = nullptr;
    D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));

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
    auto pfnInitCore = reinterpret_cast<PFN_NVSDK_NGX_D3D12_Init_Ext>(GetProcAddress(hCore, "NVSDK_NGX_D3D12_Init_Ext"));
    auto pfnGetCapCore = reinterpret_cast<PFN_NVSDK_NGX_D3D12_GetCapabilityParameters>(GetProcAddress(hCore, "NVSDK_NGX_D3D12_GetCapabilityParameters"));

    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    pfnInitCore(0x24480451ull, tempPath, device, nullptr, nullptr);
    void* capParams = nullptr;
    pfnGetCapCore(&capParams);

    // Load Forwarder
    const wchar_t* fwdPath = L"C:\\code\\dlss5-vr\\dist\\DLSS5-VR-Release\\deps\\nvngx.dll_dlssnr.dll";
    HMODULE hFwd = LoadLibraryW(fwdPath);
    auto pfnCreate = reinterpret_cast<PFN_dlssnr_call_create>(GetProcAddress(hFwd, "dlssnr_call_create"));
    auto pfnEvaluate = reinterpret_cast<PFN_dlssnr_call_evaluate_v2>(GetProcAddress(hFwd, "dlssnr_call_evaluate_v2"));
    auto pfnRelease = reinterpret_cast<PFN_dlssnr_call_release>(GetProcAddress(hFwd, "dlssnr_call_release"));
    auto pfnSetFloatSlot = reinterpret_cast<PFN_dlssnr_call_set_float_slot>(GetProcAddress(hFwd, "dlssnr_call_set_float_slot"));
    auto pfnCallError = reinterpret_cast<PFN_dlssnr_call_error>(GetProcAddress(hFwd, "dlssnr_call_error"));
    if (pfnSetFloatSlot) pfnSetFloatSlot(1);

    // Create textures
    D3D12_HEAP_PROPERTIES hpDefault = {};
    hpDefault.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;

    // Color: R8G8B8A8_UNORM
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ID3D12Resource* colorRes = nullptr;
    HRESULT hr = device->CreateCommittedResource(&hpDefault, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&colorRes));
    std::cout << "Create Color (11440x3817 RGBA8): 0x" << std::hex << hr << std::endl;

    // Output: R8G8B8A8_UNORM UAV
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* outRes = nullptr;
    hr = device->CreateCommittedResource(&hpDefault, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&outRes));
    std::cout << "Create Output (11440x3817 UAV): 0x" << std::hex << hr << std::endl;

    // Depth & Motion (guide size 512x512)
    desc.Width = 512;
    desc.Height = 512;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    ID3D12Resource* depthRes = nullptr;
    device->CreateCommittedResource(&hpDefault, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&depthRes));

    desc.Format = DXGI_FORMAT_R32G32_FLOAT;
    ID3D12Resource* motionRes = nullptr;
    device->CreateCommittedResource(&hpDefault, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&motionRes));

    const wchar_t* nrPath = L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Half-Life Alyx\\game\\bin\\win64\\nvngx_dlssnr.dll";
    void* feature = pfnCreate(nrPath, tempPath, device, cmdList, capParams, width, height, 0, 1.0f, 0, 1.0f, 1.0f, 1.0f, 1, 0);
    std::cout << "Feature 18 at 11440x3817: " << feature << std::endl;

    cmdList->Close();
    ID3D12CommandList* lists[] = { cmdList };
    queue->ExecuteCommandLists(1, lists);

    allocator->Reset();
    cmdList->Reset(allocator, nullptr);

    if (feature) {
        std::cout << "Attempting Evaluate at 11440x3817..." << std::endl;
        int evalRes = RunEval(pfnEvaluate, cmdList, feature, capParams, colorRes, depthRes, motionRes, outRes, width, height);
        std::cout << "Evaluate result: " << evalRes << std::endl;
        if (evalRes != 1 && pfnCallError) {
            std::cout << "Model error: " << pfnCallError() << std::endl;
        }
        pfnRelease(feature);
    }

    motionRes->Release();
    depthRes->Release();
    outRes->Release();
    colorRes->Release();
    cmdList->Release();
    allocator->Release();
    queue->Release();
    device->Release();
    return 0;
}
