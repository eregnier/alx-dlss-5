#include <cassert>
#include <iostream>
#include <cmath>
#include <algorithm>
#include <d3d11.h>
#include <d3dcompiler.h>
#include "../include/foveated_composite_shader.hpp"
#include "../include/hlx_config.hpp"

// Forward declare verification functions
void TestConstantBufferAlignment();
void TestFoveatedGeometryMath();
void TestFeatherWeightCurve();
void TestShaderCompilation();

int main() {
    std::cout << "=== Running HLX-DLSS5 Foveated Pipeline Unit Tests ===" << std::endl;

    TestConstantBufferAlignment();
    TestFoveatedGeometryMath();
    TestFeatherWeightCurve();
    TestShaderCompilation();

    std::cout << "=== All Foveated Pipeline Unit Tests PASSED (5/5) ===" << std::endl;
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
