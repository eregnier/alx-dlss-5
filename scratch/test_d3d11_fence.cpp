#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <iostream>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

int main() {
    ComPtr<ID3D11Device> dev11;
    ComPtr<ID3D11DeviceContext> ctx11;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        0, nullptr, 0, D3D11_SDK_VERSION,
        &dev11, &fl, &ctx11
    );
    if (FAILED(hr)) {
        std::cout << "D3D11CreateDevice failed: 0x" << std::hex << hr << std::endl;
        return 1;
    }

    ComPtr<ID3D11Device5> dev11_5;
    hr = dev11.As(&dev11_5);
    if (FAILED(hr)) {
        std::cout << "ID3D11Device5 not supported" << std::endl;
        return 2;
    }

    ComPtr<ID3D11DeviceContext4> ctx11_4;
    hr = ctx11.As(&ctx11_4);
    if (FAILED(hr)) {
        std::cout << "ID3D11DeviceContext4 not supported" << std::endl;
        return 3;
    }

    ComPtr<ID3D11Fence> fence11;
    hr = dev11_5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence11));
    if (FAILED(hr)) {
        std::cout << "CreateFence failed: 0x" << std::hex << hr << std::endl;
        return 4;
    }

    HANDLE fenceHandle = nullptr;
    hr = fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fenceHandle);
    if (FAILED(hr)) {
        std::cout << "CreateSharedHandle failed: 0x" << std::hex << hr << std::endl;
        return 5;
    }

    ComPtr<ID3D12Device> dev12;
    hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev12));
    if (FAILED(hr)) {
        std::cout << "D3D12CreateDevice failed" << std::endl;
        return 6;
    }

    ComPtr<ID3D12Fence> fence12;
    hr = dev12->OpenSharedHandle(fenceHandle, IID_PPV_ARGS(&fence12));
    CloseHandle(fenceHandle);
    if (FAILED(hr)) {
        std::cout << "OpenSharedHandle in D3D12 failed: 0x" << std::hex << hr << std::endl;
        return 7;
    }

    std::cout << "SUCCESS! D3D11 <-> D3D12 Shared Hardware Fence verified!" << std::endl;
    return 0;
}
