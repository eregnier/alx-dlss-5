#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <iostream>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

int main() {
    std::cout << "[TEST] Testing D3D11 <-> D3D12 Shared Texture Interop..." << std::endl;

    // 1. Create D3D11 Device
    ID3D11Device* d3d11Device = nullptr;
    ID3D11DeviceContext* d3d11Context = nullptr;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION,
        &d3d11Device, &fl, &d3d11Context
    );
    if (FAILED(hr)) {
        std::cerr << "Failed to create D3D11 device: 0x" << std::hex << hr << std::endl;
        return 1;
    }
    std::cout << "D3D11 Device created successfully." << std::endl;

    // 2. Create D3D12 Device
    ID3D12Device* d3d12Device = nullptr;
    hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d3d12Device));
    if (FAILED(hr)) {
        std::cerr << "Failed to create D3D12 device: 0x" << std::hex << hr << std::endl;
        return 1;
    }
    std::cout << "D3D12 Device created successfully." << std::endl;

    // 3. Create Shared D3D11 Texture (R8G8B8A8_UNORM) with UAV flag
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = 1024;
    td.Height = 1024;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    ID3D11Texture2D* sharedTex11 = nullptr;
    hr = d3d11Device->CreateTexture2D(&td, nullptr, &sharedTex11);
    std::cout << "Create D3D11 texture with UAV + SHARED: 0x" << std::hex << hr << std::endl;

    if (FAILED(hr)) {
        std::cerr << "Failed to create shared D3D11 texture: 0x" << std::hex << hr << std::endl;
        return 1;
    }
    std::cout << "Shared D3D11 texture created successfully." << std::endl;

    // 4. Get Shared Handle
    HANDLE hShared = nullptr;
    if (td.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) {
        IDXGIResource1* res1 = nullptr;
        sharedTex11->QueryInterface(IID_PPV_ARGS(&res1));
        hr = res1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &hShared);
        res1->Release();
    } else {
        IDXGIResource* res = nullptr;
        sharedTex11->QueryInterface(IID_PPV_ARGS(&res));
        hr = res->GetSharedHandle(&hShared);
        res->Release();
    }

    if (FAILED(hr) || !hShared) {
        std::cerr << "Failed to obtain shared handle: 0x" << std::hex << hr << std::endl;
        return 1;
    }
    std::cout << "Shared handle obtained: " << hShared << std::endl;

    // 5. Open in D3D12
    ID3D12Resource* sharedRes12 = nullptr;
    hr = d3d12Device->OpenSharedHandle(hShared, IID_PPV_ARGS(&sharedRes12));
    if (FAILED(hr)) {
        std::cerr << "Failed to open shared handle in D3D12: 0x" << std::hex << hr << std::endl;
        return 1;
    }
    std::cout << "SUCCESS! D3D12 successfully opened shared handle directly as ID3D12Resource: " << sharedRes12 << std::endl;

    D3D12_RESOURCE_DESC rDesc = sharedRes12->GetDesc();
    std::cout << "D3D12 Resource Desc: " << rDesc.Width << "x" << rDesc.Height << ", Format: " << rDesc.Format << ", Flags: 0x" << std::hex << rDesc.Flags << std::endl;

    sharedRes12->Release();
    if (td.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) {
        CloseHandle(hShared);
    }
    sharedTex11->Release();
    d3d12Device->Release();
    d3d11Context->Release();
    d3d11Device->Release();
    return 0;
}
