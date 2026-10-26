#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <iostream>
#include <vector>
#include "ngx_abi.hpp"

using Microsoft::WRL::ComPtr;
using namespace cheeky::foveated_dlss;

int main() {
    std::cout << "[INTEROP TEST] Testing D3D11 to D3D12 Shared Texture + DLSS-NR Feature 18..." << std::endl;

    // 1. Create D3D11 Device
    ComPtr<ID3D11Device> d3d11Device;
    ComPtr<ID3D11DeviceContext> d3d11Context;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &d3d11Device,
        &fl,
        &d3d11Context
    );
    if (FAILED(hr)) {
        std::cout << "Failed D3D11CreateDevice: 0x" << std::hex << hr << std::endl;
        return 1;
    }
    std::cout << "D3D11 device created." << std::endl;

    // 2. Create D3D12 Device
    ComPtr<ID3D12Device> d3d12Device;
    hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d3d12Device));
    if (FAILED(hr)) {
        std::cout << "Failed D3D12CreateDevice: 0x" << std::hex << hr << std::endl;
        return 1;
    }
    std::cout << "D3D12 device created." << std::endl;

    // Create D3D12 command queue, allocator, list, fence
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC qd = {};
    hr = d3d12Device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));

    ComPtr<ID3D12CommandAllocator> allocator;
    hr = d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));

    ComPtr<ID3D12GraphicsCommandList> cmdList;
    hr = d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&cmdList));

    ComPtr<ID3D12Fence> fence;
    hr = d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 fenceVal = 1;

    // 3. Create a D3D11 Shared Texture (simulating Alyx or an intermediate shared texture)
    const UINT width = 2048;
    const UINT height = 2048;

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;

    ComPtr<ID3D11Texture2D> d3d11Tex;
    hr = d3d11Device->CreateTexture2D(&texDesc, nullptr, &d3d11Tex);
    if (FAILED(hr)) {
        std::cout << "Failed CreateTexture2D on D3D11: 0x" << std::hex << hr << std::endl;
        return 1;
    }
    std::cout << "D3D11 Shared Texture created." << std::endl;

    // Get NT Shared Handle
    ComPtr<IDXGIResource1> dxgiRes;
    hr = d3d11Tex.As(&dxgiRes);
    HANDLE sharedHandle = nullptr;
    hr = dxgiRes->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &sharedHandle);
    if (FAILED(hr) || !sharedHandle) {
        std::cout << "Failed CreateSharedHandle: 0x" << std::hex << hr << std::endl;
        return 1;
    }
    std::cout << "Shared handle acquired: " << sharedHandle << std::endl;

    // 4. Open in D3D12
    ComPtr<ID3D12Resource> d3d12InRes;
    hr = d3d12Device->OpenSharedHandle(sharedHandle, IID_PPV_ARGS(&d3d12InRes));
    if (FAILED(hr)) {
        std::cout << "Failed OpenSharedHandle in D3D12: 0x" << std::hex << hr << std::endl;
        CloseHandle(sharedHandle);
        return 1;
    }
    CloseHandle(sharedHandle);
    std::cout << "Opened shared texture in D3D12: " << d3d12InRes.Get() << std::endl;

    // 5. Create D3D12 private output texture
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC outDesc = {};
    outDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    outDesc.Width = width;
    outDesc.Height = height;
    outDesc.DepthOrArraySize = 1;
    outDesc.MipLevels = 1;
    outDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    outDesc.SampleDesc.Count = 1;
    outDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ComPtr<ID3D12Resource> d3d12OutRes;
    hr = d3d12Device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &outDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&d3d12OutRes));
    if (FAILED(hr)) {
        std::cout << "Failed to create D3D12 output texture: 0x" << std::hex << hr << std::endl;
        return 1;
    }

    // Create dummy depth and motion textures for NR
    outDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
    outDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> motionRes;
    d3d12Device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &outDesc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&motionRes));

    outDesc.Format = DXGI_FORMAT_R32_FLOAT;
    ComPtr<ID3D12Resource> depthRes;
    d3d12Device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &outDesc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&depthRes));

    // 6. Test NGX Feature 18 with these textures!
    HKEY hKey = nullptr;
    wchar_t regPath[MAX_PATH] = {};
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD pathSize = sizeof(regPath);
        RegQueryValueExW(hKey, L"FullPath", nullptr, nullptr, reinterpret_cast<LPBYTE>(regPath), &pathSize);
        RegCloseKey(hKey);
    }
    std::wstring coreDll = std::wstring(regPath) + L"\\_nvngx.dll";
    HMODULE hCore = LoadLibraryW(coreDll.c_str());
    HMODULE hNR = LoadLibraryW(L"C:\\code\\CheekyFoveatedDLSS\\bin\\Release\\runtime-host-test-data\\5480-4341078\\runtime\\nvngx_dlssnr.dll");
    if (!hNR) hNR = LoadLibraryW(L"nvngx_dlssnr.dll");
    std::cout << "hCore=" << hCore << " hNR=" << hNR << std::endl;

    using PFN_NVSDK_NGX_D3D12_Init_Ext = NgxResult(*)(uint64_t, const wchar_t*, ID3D12Device*, const void*, void*);
    using PFN_NVSDK_NGX_D3D12_AllocateParameters = NgxResult(*)(NgxParameters**);
    using PFN_NVSDK_NGX_D3D12_DestroyParameters = NgxResult(*)(NgxParameters*);
    using PFN_NVSDK_NGX_D3D12_CreateFeature = NgxResult(*)(ID3D12GraphicsCommandList*, uint32_t, NgxParameters*, NgxHandle**);
    using PFN_NVSDK_NGX_D3D12_ReleaseFeature = NgxResult(*)(NgxHandle*);
    using PFN_NVSDK_NGX_D3D12_EvaluateFeature = NgxResult(*)(ID3D12GraphicsCommandList*, const NgxHandle*, const NgxParameters*, void*);

    auto pfnInit12 = reinterpret_cast<PFN_NVSDK_NGX_D3D12_Init_Ext>(GetProcAddress(hNR ? hNR : hCore, "NVSDK_NGX_D3D12_Init_Ext"));
    auto pfnAlloc12 = reinterpret_cast<PFN_NVSDK_NGX_D3D12_AllocateParameters>(GetProcAddress(hCore, "NVSDK_NGX_D3D12_AllocateParameters"));
    auto pfnDestroy12 = reinterpret_cast<PFN_NVSDK_NGX_D3D12_DestroyParameters>(GetProcAddress(hCore, "NVSDK_NGX_D3D12_DestroyParameters"));
    auto pfnCreate12 = reinterpret_cast<PFN_NVSDK_NGX_D3D12_CreateFeature>(GetProcAddress(hNR ? hNR : hCore, "NVSDK_NGX_D3D12_CreateFeature"));
    auto pfnRelease12 = reinterpret_cast<PFN_NVSDK_NGX_D3D12_ReleaseFeature>(GetProcAddress(hNR ? hNR : hCore, "NVSDK_NGX_D3D12_ReleaseFeature"));
    auto pfnEvaluate12 = reinterpret_cast<PFN_NVSDK_NGX_D3D12_EvaluateFeature>(GetProcAddress(hNR ? hNR : hCore, "NVSDK_NGX_D3D12_EvaluateFeature"));

    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    NgxResult res = pfnInit12(0x1337BEEF, tempPath, d3d12Device.Get(), nullptr, nullptr);
    std::cout << "NVSDK_NGX_D3D12_Init_Ext: 0x" << std::hex << res << std::endl;

    NgxParameters* params = nullptr;
    NgxResult aRes = pfnAlloc12(&params);
    std::cout << "Allocated params: 0x" << std::hex << aRes << " ptr=" << (void*)params << std::endl;

    try {
        std::cout << "Setting Width..." << std::endl;
        params->Set("Width", width);
        std::cout << "Setting Height..." << std::endl;
        params->Set("Height", height);
    } catch (...) {
        std::cout << "Caught exception when setting params!" << std::endl;
    }
    std::cout << "Setting other create params..." << std::endl;
    params->Set("OutWidth", width);
    params->Set("OutHeight", height);
    params->Set("DLSSNR.Width", width);
    params->Set("DLSSNR.Height", height);
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
    std::cout << "Calling pfnCreate12..." << std::endl;

    NgxHandle* featHandle = nullptr;
    res = pfnCreate12(cmdList.Get(), 18, params, &featHandle);
    std::cout << "pfnCreate12(18): 0x" << std::hex << res << " handle=" << featHandle << std::endl;

    if (featHandle) {
        // Set evaluation parameters
        params->Set("DLSSNR.Color", d3d12InRes.Get());
        params->Set("DLSSNR.Output", d3d12OutRes.Get());
        params->Set("DLSSNR.Depth", depthRes.Get());
        params->Set("DLSSNR.MVec", motionRes.Get());

        for (const char* r : {"Color", "Output", "Depth", "MVec"}) {
            std::string prefix = std::string("DLSSNR.") + r + "Subrect";
            params->Set((prefix + "BaseX").c_str(), 0U);
            params->Set((prefix + "BaseY").c_str(), 0U);
            params->Set((prefix + "Width").c_str(), width);
            params->Set((prefix + "Height").c_str(), height);
        }
        params->Set("DLSSNR.MVecScaleX", float(width));
        params->Set("DLSSNR.MVecScaleY", float(height));
        params->Set("DLSSNR.DepthInverted", 0U);
        params->Set("DLSSNR.Reset", 1U);

        // Transition d3d12InRes to NON_PIXEL_SHADER_RESOURCE
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = d3d12InRes.Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &b);

        res = pfnEvaluate12(cmdList.Get(), featHandle, params, nullptr);
        std::cout << "pfnEvaluate12(18): 0x" << std::hex << res << std::endl;

        cmdList->Close();
        ID3D12CommandList* lists[] = { cmdList.Get() };
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence.Get(), fenceVal);
        fence->SetEventOnCompletion(fenceVal, fenceEvent);
        WaitForSingleObject(fenceEvent, 5000);
        std::cout << "[SUCCESS] DLSS-NR Feature 18 successfully executed on D3D11 shared texture!" << std::endl;

        pfnRelease12(featHandle);
    }

    if (params) pfnDestroy12(params);
    CloseHandle(fenceEvent);
    return 0;
}
