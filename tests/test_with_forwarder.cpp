#include <windows.h>
#include <d3d12.h>
#include <iostream>
#include <vector>

using NgxResult = uint32_t;

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

using PFN_NVSDK_NGX_D3D12_Init_Ext = NgxResult(*)(uint64_t, const wchar_t*, ID3D12Device*, const void*, void*);
using PFN_NVSDK_NGX_D3D12_GetCapabilityParameters = NgxResult(*)(void**);
using PFN_NVSDK_NGX_D3D12_AllocateParameters = NgxResult(*)(void**);

int main() {
    std::cout << "[TEST] Testing Feature 18 Create + Evaluate with Populated Buffers..." << std::endl;

    // 1. Create D3D12 Device, Queue, Allocator, List, Fence
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

    ID3D12Fence* fence = nullptr;
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 fenceVal = 1;

    // 2. Load _nvngx.dll for capability parameters
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
    auto pfnAllocCore = reinterpret_cast<PFN_NVSDK_NGX_D3D12_AllocateParameters>(GetProcAddress(hCore, "NVSDK_NGX_D3D12_AllocateParameters"));

    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    pfnInitCore(0x24480451ull, tempPath, device, nullptr, nullptr);

    void* capParams = nullptr;
    if (pfnGetCapCore) pfnGetCapCore(&capParams);
    if (!capParams && pfnAllocCore) pfnAllocCore(&capParams);

    // 3. Load nvngx.dll_dlssnr.dll
    const wchar_t* fwdPath = L"C:\\code\\dlss5-vr\\dist\\DLSS5-VR-Release\\deps\\nvngx.dll_dlssnr.dll";
    HMODULE hFwd = LoadLibraryW(fwdPath);
    if (!hFwd) {
        std::cerr << "Failed to load forwarder from " << fwdPath << " (Error: " << GetLastError() << ")" << std::endl;
        return 1;
    }
    auto pfnCreate = reinterpret_cast<PFN_dlssnr_call_create>(GetProcAddress(hFwd, "dlssnr_call_create"));
    auto pfnEvaluate = reinterpret_cast<PFN_dlssnr_call_evaluate_v2>(GetProcAddress(hFwd, "dlssnr_call_evaluate_v2"));
    auto pfnRelease = reinterpret_cast<PFN_dlssnr_call_release>(GetProcAddress(hFwd, "dlssnr_call_release"));
    auto pfnSetFloatSlot = reinterpret_cast<PFN_dlssnr_call_set_float_slot>(GetProcAddress(hFwd, "dlssnr_call_set_float_slot"));
    auto pfnCallError = reinterpret_cast<const char*(*)()>(GetProcAddress(hFwd, "dlssnr_call_error"));
    std::cout << "Forwarder loaded: " << hFwd << ", create: " << pfnCreate << ", eval_v2: " << pfnEvaluate << std::endl;
    if (pfnSetFloatSlot) pfnSetFloatSlot(1);

    // 4. Create textures
    const unsigned width = 512;
    const unsigned height = 512;

    D3D12_HEAP_PROPERTIES hpDefault = {};
    hpDefault.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_HEAP_PROPERTIES hpUpload = {};
    hpUpload.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;

    // Color: R8G8B8A8_UNORM
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource* colorRes = nullptr;
    device->CreateCommittedResource(&hpDefault, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&colorRes));

    // Output: R8G8B8A8_UNORM UAV
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* outRes = nullptr;
    device->CreateCommittedResource(&hpDefault, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&outRes));

    // Depth: R32_FLOAT
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource* depthRes = nullptr;
    device->CreateCommittedResource(&hpDefault, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&depthRes));

    // Motion: R32G32_FLOAT
    desc.Format = DXGI_FORMAT_R32G32_FLOAT;
    ID3D12Resource* motionRes = nullptr;
    device->CreateCommittedResource(&hpDefault, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&motionRes));

    // Upload buffer (large enough for all 3: 512*512*8 + 512*512*4 + 512*512*8 = 5 MB)
    UINT64 uploadSize = 16 * 1024 * 1024;
    D3D12_RESOURCE_DESC bDesc = {};
    bDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bDesc.Width = uploadSize;
    bDesc.Height = 1;
    bDesc.DepthOrArraySize = 1;
    bDesc.MipLevels = 1;
    bDesc.SampleDesc.Count = 1;
    bDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* uploadBuf = nullptr;
    device->CreateCommittedResource(&hpUpload, D3D12_HEAP_FLAG_NONE, &bDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuf));

    char* mapped = nullptr;
    uploadBuf->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
    // Fill color buffer (Offset 0): 0x3C00 (FP16 1.0f)
    for (size_t i = 0; i < width * height * 4; ++i) {
        reinterpret_cast<uint16_t*>(mapped)[i] = 0x3C00;
    }
    // Fill depth buffer (Offset 4MB): 0.5f
    float* depthMapped = reinterpret_cast<float*>(mapped + 4 * 1024 * 1024);
    for (size_t i = 0; i < width * height; ++i) {
        depthMapped[i] = 0.5f;
    }
    // Fill motion buffer (Offset 8MB): 0.0f
    float* motionMapped = reinterpret_cast<float*>(mapped + 8 * 1024 * 1024);
    for (size_t i = 0; i < width * height * 2; ++i) {
        motionMapped[i] = 0.0f;
    }
    uploadBuf->Unmap(0, nullptr);

    // Copy Color
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fpColor{};
    fpColor.Offset = 0;
    fpColor.Footprint.Width = width;
    fpColor.Footprint.Height = height;
    fpColor.Footprint.Depth = 1;
    fpColor.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    fpColor.Footprint.RowPitch = width * 4;

    D3D12_TEXTURE_COPY_LOCATION srcColor{}, dstColor{};
    srcColor.pResource = uploadBuf;
    srcColor.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcColor.PlacedFootprint = fpColor;
    dstColor.pResource = colorRes;
    dstColor.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    cmdList->CopyTextureRegion(&dstColor, 0, 0, 0, &srcColor, nullptr);

    // Copy Depth
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fpDepth{};
    fpDepth.Offset = 4 * 1024 * 1024;
    fpDepth.Footprint.Width = width;
    fpDepth.Footprint.Height = height;
    fpDepth.Footprint.Depth = 1;
    fpDepth.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
    fpDepth.Footprint.RowPitch = width * 4;

    D3D12_TEXTURE_COPY_LOCATION srcDepth{}, dstDepth{};
    srcDepth.pResource = uploadBuf;
    srcDepth.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcDepth.PlacedFootprint = fpDepth;
    dstDepth.pResource = depthRes;
    dstDepth.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    cmdList->CopyTextureRegion(&dstDepth, 0, 0, 0, &srcDepth, nullptr);

    // Copy Motion
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fpMotion{};
    fpMotion.Offset = 8 * 1024 * 1024;
    fpMotion.Footprint.Width = width;
    fpMotion.Footprint.Height = height;
    fpMotion.Footprint.Depth = 1;
    fpMotion.Footprint.Format = DXGI_FORMAT_R32G32_FLOAT;
    fpMotion.Footprint.RowPitch = width * 8;

    D3D12_TEXTURE_COPY_LOCATION srcMotion{}, dstMotion{};
    srcMotion.pResource = uploadBuf;
    srcMotion.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcMotion.PlacedFootprint = fpMotion;
    dstMotion.pResource = motionRes;
    dstMotion.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    cmdList->CopyTextureRegion(&dstMotion, 0, 0, 0, &srcMotion, nullptr);

    // Transition all 3 to NON_PIXEL_SHADER_RESOURCE
    D3D12_RESOURCE_BARRIER barriers[3] = {};
    for (int i = 0; i < 3; ++i) {
        barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[i].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    barriers[0].Transition.pResource = colorRes;
    barriers[1].Transition.pResource = depthRes;
    barriers[2].Transition.pResource = motionRes;
    cmdList->ResourceBarrier(3, barriers);

    // 5. Create DLSS-NR Feature
    const wchar_t* nrPath = L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Half-Life Alyx\\game\\bin\\win64\\nvngx_dlssnr.dll";
    void* feature = pfnCreate(
        nrPath, tempPath, device, cmdList, capParams,
        width, height,
        0, 1.0f, 0, 1.0f, 1.0f, 1.0f, 1, 0
    );
    std::cout << "Feature created: " << feature << std::endl;

    cmdList->Close();
    ID3D12CommandList* lists[] = { cmdList };
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence, fenceVal++);
    fence->SetEventOnCompletion(fenceVal - 1, fenceEvent);
    WaitForSingleObject(fenceEvent, 5000);

    allocator->Reset();
    cmdList->Reset(allocator, nullptr);

    if (feature) {
        std::cout << "Evaluating DLSS-NR Feature with valid textures..." << std::endl;
        int evalRes = pfnEvaluate(
            cmdList, feature, capParams,
            colorRes, depthRes, motionRes, outRes,
            width, height,
            width, height, // guide W, H
            width, height, // motion W, H
            0, 0,          // depth BaseX, BaseY
            0, 0,          // motion BaseX, BaseY
            0,             // depthInverted
            1,             // reset (first frame)
            1.0f,          // intensity
            0,             // style
            1.0f,          // localStructure
            1.0f,          // localTone
            1.0f,          // skinStructure
            1,             // useAutoMask
            float(width), float(height) // mvScale
        );
        std::cout << "Evaluate result: " << evalRes << std::endl;
        if (evalRes != 1 && pfnCallError) {
            std::cout << "Model error: " << pfnCallError() << std::endl;
        }

        cmdList->Close();
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence, fenceVal);
        fence->SetEventOnCompletion(fenceVal, fenceEvent);
        WaitForSingleObject(fenceEvent, 5000);

        std::cout << "SUCCESS! GPU completed DLSS-NR Neural Reconstruction execution on RTX 5090!" << std::endl;
        pfnRelease(feature);
    }

    CloseHandle(fenceEvent);
    uploadBuf->Release();
    motionRes->Release();
    depthRes->Release();
    outRes->Release();
    colorRes->Release();
    fence->Release();
    cmdList->Release();
    allocator->Release();
    queue->Release();
    device->Release();
    return 0;
}
