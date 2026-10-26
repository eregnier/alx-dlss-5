#include <windows.h>
#include <d3d11.h>
#include <iostream>

#pragma comment(lib, "d3d11.lib")

int main() {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);

    // 1. Create Typeless texture (representing Alyx backbuffer)
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = 512;
    td.Height = 512;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    ID3D11Texture2D* typelessTex = nullptr;
    HRESULT hr = dev->CreateTexture2D(&td, nullptr, &typelessTex);
    std::cout << "Create typeless tex: 0x" << std::hex << hr << std::endl;

    // 2. Create Shared UNORM texture
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    ID3D11Texture2D* unormSharedTex = nullptr;
    hr = dev->CreateTexture2D(&td, nullptr, &unormSharedTex);
    std::cout << "Create UNORM shared tex: 0x" << std::hex << hr << std::endl;

    // 3. Test CopyResource from Typeless to UNORM
    ctx->CopyResource(unormSharedTex, typelessTex);
    ctx->Flush();
    std::cout << "CopyResource Typeless -> UNORM SUCCESS!" << std::endl;

    unormSharedTex->Release();
    typelessTex->Release();
    ctx->Release();
    dev->Release();
    return 0;
}
