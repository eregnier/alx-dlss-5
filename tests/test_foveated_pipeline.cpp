#undef NDEBUG // Build is Release: keep asserts live
#include <cassert>
#include <iostream>
#include <vector>
#include <cmath>
#include <algorithm>
#include <d3d11.h>
#include <d3dcompiler.h>
#pragma comment(lib, "d3d11.lib")
#include "../include/foveated_composite_shader.hpp"
#include "../include/hlx_config.hpp"

// Forward declare verification functions
void TestConstantBufferAlignment();
void TestFoveatedGeometryMath();
void TestFeatherWeightCurve();
void TestShaderCompilation();
void TestDownsampleShaderCompilation();
void TestNeuralWorkingSizeMath();
void TestMatchedResidualCompositeGpu();

int main() {
    std::cout << "=== Running HLX-DLSS5 Foveated Pipeline Unit Tests ===" << std::endl;

    TestConstantBufferAlignment();
    TestFoveatedGeometryMath();
    TestFeatherWeightCurve();
    TestShaderCompilation();
    TestDownsampleShaderCompilation();
    TestNeuralWorkingSizeMath();
    TestMatchedResidualCompositeGpu();

    std::cout << "=== All Foveated Pipeline Unit Tests PASSED (7/7) ===" << std::endl;
    return 0;
}

void TestConstantBufferAlignment() {
    std::cout << "[TEST] Verifying FoveatedConstants alignment and size..." << std::endl;
    
    // D3D11 constant buffers must be a multiple of 16 bytes
    assert(sizeof(hlx::FoveatedConstants) % 16 == 0);
    assert(sizeof(hlx::FoveatedConstants) == 64);
    assert(alignof(hlx::FoveatedConstants) >= 16);

    hlx::FoveatedConstants c = {};
    c.outputWidth = 2872;
    c.outputHeight = 1914;
    c.foveaWidth = 1580;
    c.foveaHeight = 1052;
    c.foveaBaseX = (2872 - 1580) / 2;
    c.foveaBaseY = (1914 - 1052) / 2;
    c.foveaRoundness = 0.50f;
    c.featherWidth = 0.12f;
    c.foveatedEnabled = 1;

    assert(c.foveaBaseX == 646);
    assert(c.foveaBaseY == 431);
    assert(c.foveaBaseX + c.foveaWidth <= c.outputWidth);
    assert(c.foveaBaseY + c.foveaHeight <= c.outputHeight);

    std::cout << "  -> Alignment & packing verified (size=" << sizeof(c) << " bytes)." << std::endl;
}

void TestFoveatedGeometryMath() {
    std::cout << "[TEST] Verifying Foveated Geometry Sweetspot Calculation..." << std::endl;

    uint32_t fullW = 2872;
    uint32_t fullH = 1914;
    float fovW = 0.55f;
    float fovH = 0.55f;

    uint32_t sweetspotW = (static_cast<uint32_t>(fullW * fovW) + 7) & ~7u;
    uint32_t sweetspotH = (static_cast<uint32_t>(fullH * fovH) + 7) & ~7u;

    assert(sweetspotW % 8 == 0);
    assert(sweetspotH % 8 == 0);
    assert(sweetspotW < fullW);
    assert(sweetspotH < fullH);

    // Neural pixel workload reduction ratio
    double fullPixels = static_cast<double>(fullW) * fullH;
    double foveaPixels = static_cast<double>(sweetspotW) * sweetspotH;
    double reductionRatio = 1.0 - (foveaPixels / fullPixels);

    std::cout << "  -> Full: " << fullW << "x" << fullH << " (" << (fullPixels / 1e6) << " MPix)" << std::endl;
    std::cout << "  -> Fovea: " << sweetspotW << "x" << sweetspotH << " (" << (foveaPixels / 1e6) << " MPix)" << std::endl;
    std::cout << "  -> Neural Workload Reduction: " << (reductionRatio * 100.0) << "%" << std::endl;

    // Must reduce workload by at least 60%
    assert(reductionRatio >= 0.60);
}

void TestFeatherWeightCurve() {
    std::cout << "[TEST] Verifying Cosine Smoothstep Falloff Curve..." << std::endl;

    float feather = 0.15f;
    auto smoothstep_val = [](float edge0, float edge1, float x) -> float {
        float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };

    auto calc_weight = [&](float dist) -> float {
        if (dist >= 1.0f) return 0.0f;
        float edge0 = std::max(0.0f, 1.0f - feather);
        float t = smoothstep_val(edge0, 1.0f, dist);
        return 1.0f - t;
    };

    // Center sweetspot (dist = 0.0): 100% neural
    assert(std::abs(calc_weight(0.0f) - 1.0f) < 1e-4);
    // Inner fovea (dist = 0.5): 100% neural
    assert(std::abs(calc_weight(0.5f) - 1.0f) < 1e-4);
    // Boundary onset (dist = 0.85): 100% neural before transition
    assert(std::abs(calc_weight(0.85f) - 1.0f) < 1e-3);
    // Mid-transition (dist = 0.925): 50% blend
    float midWeight = calc_weight(0.925f);
    assert(midWeight > 0.40f && midWeight < 0.60f);
    // Outer edge (dist = 1.0): 0% neural (100% native game pixels)
    assert(std::abs(calc_weight(1.0f) - 0.0f) < 1e-4);
    // Beyond edge (dist = 1.2): 0% neural
    assert(std::abs(calc_weight(1.2f) - 0.0f) < 1e-4);

    std::cout << "  -> Smoothstep transition curve verified across all intervals." << std::endl;
}

void TestShaderCompilation() {
    std::cout << "[TEST] Verifying HLSL Compute Shader Bytecode Compilation..." << std::endl;

    HMODULE hCompiler = LoadLibraryW(L"d3dcompiler_47.dll");
    assert(hCompiler != nullptr);

    auto pfnCompile = reinterpret_cast<pD3DCompile>(GetProcAddress(hCompiler, "D3DCompile"));
    assert(pfnCompile != nullptr);

    ID3DBlob* shaderBlob = nullptr;
    ID3DBlob* errorBlob = nullptr;

    HRESULT hr = pfnCompile(
        hlx::g_foveatedCompositeHlsl,
        sizeof(hlx::g_foveatedCompositeHlsl) - 1,
        "FoveatedCompositeHLSL",
        nullptr,
        nullptr,
        "CSMain",
        "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &shaderBlob,
        &errorBlob
    );

    if (FAILED(hr) && errorBlob) {
        std::cerr << "Shader compile error: " << reinterpret_cast<const char*>(errorBlob->GetBufferPointer()) << std::endl;
    }

    assert(SUCCEEDED(hr));
    assert(shaderBlob != nullptr);
    assert(shaderBlob->GetBufferSize() > 0);

    std::cout << "  -> Shader compiled successfully into " << shaderBlob->GetBufferSize() << " bytes of CS 5.0 bytecode." << std::endl;

    if (shaderBlob) shaderBlob->Release();
    if (errorBlob) errorBlob->Release();
}

void TestDownsampleShaderCompilation() {
    std::cout << "[TEST] Verifying fovea downsample CS compilation..." << std::endl;
    assert(sizeof(hlx::FoveaDownsampleConstants) % 16 == 0);
    assert(sizeof(hlx::FoveaDownsampleConstants) == 32);

    HMODULE hCompiler = LoadLibraryW(L"d3dcompiler_47.dll");
    assert(hCompiler != nullptr);
    auto pfnCompile = reinterpret_cast<pD3DCompile>(GetProcAddress(hCompiler, "D3DCompile"));
    assert(pfnCompile != nullptr);

    ID3DBlob* shaderBlob = nullptr;
    ID3DBlob* errorBlob = nullptr;
    HRESULT hr = pfnCompile(
        hlx::g_foveaDownsampleHlsl, sizeof(hlx::g_foveaDownsampleHlsl) - 1,
        "FoveaDownsampleHLSL", nullptr, nullptr, "CSMain", "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shaderBlob, &errorBlob);

    if (FAILED(hr) && errorBlob) {
        std::cerr << "Downsample compile error: " << reinterpret_cast<const char*>(errorBlob->GetBufferPointer()) << std::endl;
    }
    assert(SUCCEEDED(hr));
    assert(shaderBlob != nullptr && shaderBlob->GetBufferSize() > 0);
    std::cout << "  -> Downsample shader compiled into " << shaderBlob->GetBufferSize() << " bytes." << std::endl;
    if (shaderBlob) shaderBlob->Release();
    if (errorBlob) errorBlob->Release();
}

// Mirrors the working-size computation in InitEyePipeline.
void TestNeuralWorkingSizeMath() {
    std::cout << "[TEST] Verifying neural working size (WorkingScale) math..." << std::endl;
    auto work = [](uint32_t fovea, float scale) {
        uint32_t w = (static_cast<uint32_t>(static_cast<float>(fovea) * scale) + 7) & ~7u;
        return (std::clamp)(w, 64u, fovea);
    };
    assert(work(1232, 1.00f) == 1232);   // default: model size == fovea size
    assert(work(1232, 0.50f) == 616);    // half: 4x less model work
    assert(work(1232, 0.75f) % 8 == 0);  // aligned for the tensor
    assert(work(1232, 0.01f) == 64);     // clamped to the minimum
    assert(work(96, 2.00f) == 96);       // never above the fovea
    std::cout << "  -> Working size math verified." << std::endl;
}

void TestMatchedResidualCompositeGpu() {
    std::cout << "[TEST] Verifying D3D11 GPU Area-Weighted Downsample + Matched Residual Composite..." << std::endl;

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
    assert(SUCCEEDED(hr) && dev && ctx);

    assert(hlx::FoveaDownsamplePass::Instance().Init(dev));
    assert(hlx::FoveatedCompositePass::Instance().Init(dev));

    const uint32_t foveaW = 256;
    const uint32_t foveaH = 256;
    const uint32_t workW = 128; // WorkingScale = 0.50
    const uint32_t workH = 128;

    // 1. Native full-res texture with 1px high-frequency checkerboard (102 vs 154 -> mean = 128)
    std::vector<uint32_t> nativePixels(foveaW * foveaH);
    for (uint32_t y = 0; y < foveaH; ++y) {
        for (uint32_t x = 0; x < foveaW; ++x) {
            uint8_t v = ((x + y) & 1) ? 154 : 102;
            nativePixels[y * foveaW + x] = (255u << 24) | (v << 16) | (v << 8) | v;
        }
    }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = foveaW;
    td.Height = foveaH;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initNative = { nativePixels.data(), foveaW * 4, 0 };
    ID3D11Texture2D* nativeTex = nullptr;
    assert(SUCCEEDED(dev->CreateTexture2D(&td, &initNative, &nativeTex)));
    ID3D11ShaderResourceView* nativeSrv = nullptr;
    assert(SUCCEEDED(dev->CreateShaderResourceView(nativeTex, nullptr, &nativeSrv)));

    // 2. InputProxy texture (128x128) populated by FoveaDownsamplePass
    td.Width = workW;
    td.Height = workH;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    ID3D11Texture2D* inputProxyTex = nullptr;
    assert(SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &inputProxyTex)));
    ID3D11UnorderedAccessView* inputProxyUav = nullptr;
    assert(SUCCEEDED(dev->CreateUnorderedAccessView(inputProxyTex, nullptr, &inputProxyUav)));
    ID3D11ShaderResourceView* inputProxySrv = nullptr;
    assert(SUCCEEDED(dev->CreateShaderResourceView(inputProxyTex, nullptr, &inputProxySrv)));

    hlx::FoveaDownsampleConstants dsConsts = { 0, 0, foveaW, foveaH, workW, workH, 0, 0 };
    assert(hlx::FoveaDownsamplePass::Instance().Execute(ctx, nativeSrv, inputProxyUav, dsConsts));

    // 3. Neural output texture (128x128) with a +26 lighting residual above the 128 box mean (= 154)
    std::vector<uint32_t> neuralPixels(workW * workH, (255u << 24) | (154u << 16) | (154u << 8) | 154u);
    D3D11_SUBRESOURCE_DATA initNeural = { neuralPixels.data(), workW * 4, 0 };
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* neuralTex = nullptr;
    assert(SUCCEEDED(dev->CreateTexture2D(&td, &initNeural, &neuralTex)));
    ID3D11ShaderResourceView* neuralSrv = nullptr;
    assert(SUCCEEDED(dev->CreateShaderResourceView(neuralTex, nullptr, &neuralSrv)));

    // 4. Composited output texture (256x256)
    td.Width = foveaW;
    td.Height = foveaH;
    td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    ID3D11Texture2D* compTex = nullptr;
    assert(SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &compTex)));
    ID3D11UnorderedAccessView* compUav = nullptr;
    assert(SUCCEEDED(dev->CreateUnorderedAccessView(compTex, nullptr, &compUav)));

    hlx::FoveatedConstants fovConsts = {};
    fovConsts.outputWidth = foveaW;
    fovConsts.outputHeight = foveaH;
    fovConsts.foveaBaseX = 0;
    fovConsts.foveaBaseY = 0;
    fovConsts.foveaWidth = foveaW;
    fovConsts.foveaHeight = foveaH;
    fovConsts.neuralWidth = workW;
    fovConsts.neuralHeight = workH;
    fovConsts.foveaRoundness = 0.50f;
    fovConsts.featherWidth = 0.15f;
    fovConsts.foveatedEnabled = 1;
    fovConsts.showBorder = 0;
    fovConsts.sharpness = 0.0f;

    assert(hlx::FoveatedCompositePass::Instance().Execute(ctx, nativeSrv, neuralSrv, compUav, fovConsts, inputProxySrv));

    // Read back composited texture
    D3D11_TEXTURE2D_DESC stDesc = td;
    stDesc.Usage = D3D11_USAGE_STAGING;
    stDesc.BindFlags = 0;
    stDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    assert(SUCCEEDED(dev->CreateTexture2D(&stDesc, nullptr, &staging)));
    ctx->CopyResource(staging, compTex);

    D3D11_MAPPED_SUBRESOURCE map = {};
    assert(SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map)));
    const uint8_t* rowCenter = reinterpret_cast<const uint8_t*>(map.pData) + (foveaH / 2) * map.RowPitch;
    int c0 = rowCenter[(foveaW / 2) * 4];
    int c1 = rowCenter[(foveaW / 2 + 1) * 4];
    int highFreqContrast = std::abs(c1 - c0);
    float centerMean = 0.5f * (c0 + c1);

    const uint8_t* rowCorner = reinterpret_cast<const uint8_t*>(map.pData) + 2 * map.RowPitch;
    int corner0 = rowCorner[2 * 4];
    int cornerNative0 = nativePixels[2 * foveaW + 2] & 0xFF;
    ctx->Unmap(staging, 0);

    std::cout << "  -> Center pixels after 0.50x Matched Residual Composite: p0=" << c0 << ", p1=" << c1
              << " (1px contrast=" << highFreqContrast << ", mean=" << centerMean << " vs native mean 128)" << std::endl;

    // 1. High-frequency 1px native checkerboard contrast MUST be preserved (naive upscaling gives 0!)
    assert(highFreqContrast >= 40);
    // 2. Neural lighting residual (+26 boost) MUST be transferred at center sweetspot
    assert(centerMean > 140.0f);
    // 3. Corner outside ellipse must remain 100% untouched native pixel
    assert(corner0 == cornerNative0);

    staging->Release();
    compUav->Release();
    compTex->Release();
    neuralSrv->Release();
    neuralTex->Release();
    inputProxySrv->Release();
    inputProxyUav->Release();
    inputProxyTex->Release();
    nativeSrv->Release();
    nativeTex->Release();
    ctx->Release();
    dev->Release();
    std::cout << "  -> GPU Matched Residual Composite verified." << std::endl;
}
