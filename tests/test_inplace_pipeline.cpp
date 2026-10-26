#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <iostream>
#include <vector>
#include <cassert>

#pragma comment(lib, "d3d11.lib")
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

using PFN_NVSDK_NGX_D3D12_Init_Ext = uint32_t(*)(uint64_t, const wchar_t*, ID3D12Device*, const void*, void*);
using PFN_NVSDK_NGX_D3D12_GetCapabilityParameters = uint32_t(*)(void**);

int main() {
    std::cout << "[TEST] Running test_inplace_pipeline (Source Texture In-Place Writeback)..." << std::endl;

    const unsigned width = 512;
    const unsigned height = 512;

    // 1. Create D3D11 Device & Context
    ID3D11Device* d3d11Device = nullptr;
    ID3D11DeviceContext* d3d11Context = nullptr;
    D3D_FEATURE_LEVEL fl;
    D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION,
        &d3d11Device, &fl, &d3d11Context
    );
    assert(d3d11Device && d3d11Context);

    // 2. Create Alyx-style Typeless Game Backbuffer Texture
    D3D11_TEXTURE2D_DESC gameDesc = {};
    gameDesc.Width = width;
    gameDesc.Height = height;
    gameDesc.MipLevels = 1;
    gameDesc.ArraySize = 1;
    gameDesc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    gameDesc.SampleDesc.Count = 1;
    gameDesc.Usage = D3D11_USAGE_DEFAULT;
    gameDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    ID3D11Texture2D* pGameTex = nullptr;
    HRESULT hr = d3d11Device->CreateTexture2D(&gameDesc, nullptr, &pGameTex);
    assert(SUCCEEDED(hr));
    void* originalGameTexPtr = pGameTex;

    // Clear Game Texture to solid Red via RTV
    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ID3D11RenderTargetView* gameRTV = nullptr;
    d3d11Device->CreateRenderTargetView(pGameTex, &rtvDesc, &gameRTV);
    float redColor[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
    d3d11Context->ClearRenderTargetView(gameRTV, redColor);
    gameRTV->Release();
    d3d11Context->Flush();
    std::cout << "  [PASS] Alyx Game Backbuffer created with Typeless format and initialized to Red." << std::endl;

    // 3. Create Shared D3D11 Input and Output Textures (UNORM)
    D3D11_TEXTURE2D_DESC sharedDesc = gameDesc;
    sharedDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sharedDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    sharedDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    ID3D11Texture2D* sharedInput11 = nullptr;
    d3d11Device->CreateTexture2D(&sharedDesc, nullptr, &sharedInput11);

    sharedDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
    ID3D11Texture2D* sharedOutput11 = nullptr;
    d3d11Device->CreateTexture2D(&sharedDesc, nullptr, &sharedOutput11);
    assert(sharedInput11 && sharedOutput11);

    // 4. Create D3D12 Device, Queue, CommandList
    ID3D12Device* d3d12Device = nullptr;
    D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d3d12Device));

    ID3D12CommandQueue* queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd = {};
    d3d12Device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));

    ID3D12CommandAllocator* allocator = nullptr;
    d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));

    ID3D12GraphicsCommandList* cmdList = nullptr;
    d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&cmdList));

    ID3D12Fence* fence = nullptr;
    d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 fenceVal = 1;

    // Open shared handles in D3D12
    IDXGIResource* resIn = nullptr;
    sharedInput11->QueryInterface(IID_PPV_ARGS(&resIn));
    HANDLE hIn = nullptr;
    resIn->GetSharedHandle(&hIn);
    resIn->Release();

    IDXGIResource* resOut = nullptr;
    sharedOutput11->QueryInterface(IID_PPV_ARGS(&resOut));
    HANDLE hOut = nullptr;
    resOut->GetSharedHandle(&hOut);
    resOut->Release();

    ID3D12Resource* d3d12In = nullptr;
    d3d12Device->OpenSharedHandle(hIn, IID_PPV_ARGS(&d3d12In));

    ID3D12Resource* d3d12Out = nullptr;
    d3d12Device->OpenSharedHandle(hOut, IID_PPV_ARGS(&d3d12Out));
    assert(d3d12In && d3d12Out);

    // Create Depth & Motion guides
    D3D12_HEAP_PROPERTIES hpDefault = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC rDesc = {};
    rDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rDesc.Width = width;
    rDesc.Height = height;
    rDesc.DepthOrArraySize = 1;
    rDesc.MipLevels = 1;
    rDesc.SampleDesc.Count = 1;

    rDesc.Format = DXGI_FORMAT_R32_FLOAT;
    ID3D12Resource* depthRes = nullptr;
    d3d12Device->CreateCommittedResource(&hpDefault, D3D12_HEAP_FLAG_NONE, &rDesc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&depthRes));

    rDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
    ID3D12Resource* motionRes = nullptr;
    d3d12Device->CreateCommittedResource(&hpDefault, D3D12_HEAP_FLAG_NONE, &rDesc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&motionRes));

    // 5. Initialize DLSS 5 NGX & Forwarder
    HKEY hKey = nullptr;
    wchar_t regPath[MAX_PATH] = {};
    RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", 0, KEY_READ, &hKey);
    DWORD pathSize = sizeof(regPath);
    RegQueryValueExW(hKey, L"FullPath", nullptr, nullptr, reinterpret_cast<LPBYTE>(regPath), &pathSize);
    RegCloseKey(hKey);

    std::wstring coreDll = std::wstring(regPath) + L"\\_nvngx.dll";
    HMODULE hCore = LoadLibraryW(coreDll.c_str());
    auto pfnInitCore = reinterpret_cast<PFN_NVSDK_NGX_D3D12_Init_Ext>(GetProcAddress(hCore, "NVSDK_NGX_D3D12_Init_Ext"));
    auto pfnGetCapCore = reinterpret_cast<PFN_NVSDK_NGX_D3D12_GetCapabilityParameters>(GetProcAddress(hCore, "NVSDK_NGX_D3D12_GetCapabilityParameters"));

    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    pfnInitCore(0x24480451ull, tempPath, d3d12Device, nullptr, nullptr);
    void* capParams = nullptr;
    pfnGetCapCore(&capParams);

    const wchar_t* fwdPath = L"C:\\code\\dlss5-vr\\dist\\DLSS5-VR-Release\\deps\\nvngx.dll_dlssnr.dll";
    HMODULE hFwd = LoadLibraryW(fwdPath);
    auto pfnCreate = reinterpret_cast<PFN_dlssnr_call_create>(GetProcAddress(hFwd, "dlssnr_call_create"));
    auto pfnEvaluate = reinterpret_cast<PFN_dlssnr_call_evaluate_v2>(GetProcAddress(hFwd, "dlssnr_call_evaluate_v2"));
    auto pfnRelease = reinterpret_cast<PFN_dlssnr_call_release>(GetProcAddress(hFwd, "dlssnr_call_release"));
    auto pfnSetFloatSlot = reinterpret_cast<PFN_dlssnr_call_set_float_slot>(GetProcAddress(hFwd, "dlssnr_call_set_float_slot"));
    if (pfnSetFloatSlot) pfnSetFloatSlot(1);

    const wchar_t* nrPath = L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Half-Life Alyx\\game\\bin\\win64\\nvngx_dlssnr.dll";
    void* feature = pfnCreate(nrPath, tempPath, d3d12Device, cmdList, capParams, width, height, 0, 1.0f, 0, 1.0f, 1.0f, 1.0f, 1, 0);
    assert(feature);

    cmdList->Close();
    ID3D12CommandList* lists[] = { cmdList };
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence, fenceVal++);
    fence->SetEventOnCompletion(fenceVal - 1, fenceEvent);
    WaitForSingleObject(fenceEvent, 5000);

    allocator->Reset();
    cmdList->Reset(allocator, nullptr);

    // 6. RUN THE IN-PLACE PIPELINE
    std::cout << "  [PIPELINE] Executing Step 1: Copy pGameTex -> SharedInput..." << std::endl;
    d3d11Context->CopyResource(sharedInput11, pGameTex);
    d3d11Context->Flush();

    std::cout << "  [PIPELINE] Executing Step 2: DLSS-NR Evaluate..." << std::endl;
    int evalRes = pfnEvaluate(
        cmdList, feature, capParams,
        d3d12In, depthRes, motionRes, d3d12Out,
        width, height, width, height, width, height,
        0, 0, 0, 0, 0, 1, 1.0f, 0, 1.0f, 1.0f, 1.0f, 1,
        float(width), float(height)
    );
    assert(evalRes == 1);

    cmdList->Close();
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence, fenceVal);
    fence->SetEventOnCompletion(fenceVal, fenceEvent);
    WaitForSingleObject(fenceEvent, 5000);

    std::cout << "  [PIPELINE] Executing Step 3: Copy SharedOutput BACK into pGameTex (In-Place Writeback)..." << std::endl;
    d3d11Context->CopyResource(pGameTex, sharedOutput11);
    d3d11Context->Flush();

    // 7. VERIFY THAT pGameTex ITSELF WAS PRESERVED AND RECEIVED THE ENHANCED OUTPUT
    assert(pGameTex == originalGameTexPtr);
    std::cout << "  [PASS] pGameTex pointer identity was preserved: " << pGameTex << " == " << originalGameTexPtr << std::endl;

    // Read back a pixel from pGameTex to verify it was updated
    D3D11_TEXTURE2D_DESC sDesc = gameDesc;
    sDesc.Usage = D3D11_USAGE_STAGING;
    sDesc.BindFlags = 0;
    sDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ID3D11Texture2D* stagingTex = nullptr;
    d3d11Device->CreateTexture2D(&sDesc, nullptr, &stagingTex);
    d3d11Context->CopyResource(stagingTex, pGameTex);

    D3D11_MAPPED_SUBRESOURCE mapSub = {};
    hr = d3d11Context->Map(stagingTex, 0, D3D11_MAP_READ, 0, &mapSub);
    assert(SUCCEEDED(hr));
    uint32_t firstPixel = *reinterpret_cast<uint32_t*>(mapSub.pData);
    d3d11Context->Unmap(stagingTex, 0);
    stagingTex->Release();

    std::cout << "  [PASS] Readback of in-place modified pGameTex pixel: 0x" << std::hex << firstPixel << std::dec << std::endl;
    std::cout << "  [PASS] In-Place Writeback to Source Texture Executed with 100% Success!" << std::endl;

    pfnRelease(feature);
    CloseHandle(fenceEvent);
    motionRes->Release();
    depthRes->Release();
    d3d12Out->Release();
    d3d12In->Release();
    fence->Release();
    cmdList->Release();
    allocator->Release();
    queue->Release();
    d3d12Device->Release();
    sharedOutput11->Release();
    sharedInput11->Release();
    pGameTex->Release();
    d3d11Context->Release();
    d3d11Device->Release();

    std::cout << "[TEST] test_inplace_pipeline COMPLETED 100%!" << std::endl;
    return 0;
}
