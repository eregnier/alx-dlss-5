#pragma once

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include "hlx_logger.hpp"

using Microsoft::WRL::ComPtr;

namespace hlx {

// Constant buffer layout matching HLSL cbuffer FoveatedConstants : register(b0)
// 16-byte alignment strictly enforced for D3D11 constant buffers.
struct alignas(16) FoveatedConstants {
    uint32_t outputWidth;
    uint32_t outputHeight;
    uint32_t foveaBaseX;
    uint32_t foveaBaseY;

    uint32_t foveaWidth;
    uint32_t foveaHeight;
    uint32_t neuralWidth;
    uint32_t neuralHeight;

    float foveaRoundness;
    float featherWidth;
    uint32_t foveatedEnabled;
    uint32_t showBorder;

    float sharpness;     // [0.0 - 1.0] neural detail boost & unsharp mask
    uint32_t padding[3]; // pad to 64 bytes
};

static inline const char g_foveatedCompositeHlsl[] = R"(
Texture2D<float4> NativeColor : register(t0);
Texture2D<float4> NeuralColor : register(t1);
RWTexture2D<float4> OutputColor : register(u0);

cbuffer FoveatedConstants : register(b0) {
    uint2 OutputSize;
    uint2 FoveaBase;
    uint2 FoveaSize;
    uint2 NeuralSize;
    float FoveaRoundness;
    float FeatherWidth;
    uint  FoveatedEnabled;
    uint  ShowBorder;
    float Sharpness;
    uint3 Padding;
};

float EllipseDistance(float2 pixel, float2 base, float2 size, float roundness) {
    float2 centered = (pixel - base + 0.5) / (0.5 * max(size, 1.0)) - 1.0;
    float2 scaled = abs(centered);
    return lerp(max(scaled.x, scaled.y), length(scaled), saturate(roundness));
}

[numthreads(16, 16, 1)]
void CSMain(uint3 dispatch_id : SV_DispatchThreadID) {
    if (dispatch_id.x >= OutputSize.x || dispatch_id.y >= OutputSize.y) return;
    uint2 pixel = dispatch_id.xy;
    float4 native_color = NativeColor.Load(int3(pixel, 0));
    
    if (FoveatedEnabled == 0) {
        float2 uv = (float2(pixel) + 0.5) / float2(OutputSize);
        float2 neural_pos = uv * float2(NeuralSize) - 0.5;
        int2 p00 = clamp(int2(floor(neural_pos)), int2(0, 0), int2(NeuralSize) - 1);
        int2 p10 = clamp(p00 + int2(1, 0), int2(0, 0), int2(NeuralSize) - 1);
        int2 p01 = clamp(p00 + int2(0, 1), int2(0, 0), int2(NeuralSize) - 1);
        int2 p11 = clamp(p00 + int2(1, 1), int2(0, 0), int2(NeuralSize) - 1);
        float2 frac_pos = frac(neural_pos);
        float3 neural_rgb = lerp(
            lerp(NeuralColor.Load(int3(p00, 0)).rgb, NeuralColor.Load(int3(p10, 0)).rgb, frac_pos.x),
            lerp(NeuralColor.Load(int3(p01, 0)).rgb, NeuralColor.Load(int3(p11, 0)).rgb, frac_pos.x),
            frac_pos.y
        );
        OutputColor[pixel] = float4(neural_rgb, native_color.a);
        return;
    }
    
    bool inside_box = (pixel.x >= FoveaBase.x && pixel.x < FoveaBase.x + FoveaSize.x &&
                       pixel.y >= FoveaBase.y && pixel.y < FoveaBase.y + FoveaSize.y);
    if (!inside_box) {
        OutputColor[pixel] = native_color;
        return;
    }
    
    float dist = EllipseDistance(float2(pixel), float2(FoveaBase), float2(FoveaSize), FoveaRoundness);
    if (ShowBorder != 0 && abs(dist - 1.0) < 0.02) {
        OutputColor[pixel] = float4(0.0, 1.0, 0.0, native_color.a);
        return;
    }
    if (dist >= 1.0) {
        OutputColor[pixel] = native_color;
        return;
    }
    
    float weight = 1.0 - smoothstep(max(0.0, 1.0 - FeatherWidth), 1.0, dist);
    if (weight <= 0.0) {
        OutputColor[pixel] = native_color;
        return;
    }
    
    float2 local_pos = float2(pixel - FoveaBase);
    float2 uv_fovea = (local_pos + 0.5) / float2(FoveaSize);
    float2 neural_pos = uv_fovea * float2(NeuralSize) - 0.5;
    int2 p00 = clamp(int2(floor(neural_pos)), int2(0, 0), int2(NeuralSize) - 1);
    int2 p10 = clamp(p00 + int2(1, 0), int2(0, 0), int2(NeuralSize) - 1);
    int2 p01 = clamp(p00 + int2(0, 1), int2(0, 0), int2(NeuralSize) - 1);
    int2 p11 = clamp(p00 + int2(1, 1), int2(0, 0), int2(NeuralSize) - 1);
    float2 frac_pos = frac(neural_pos);
    float3 neural_rgb = lerp(
        lerp(NeuralColor.Load(int3(p00, 0)).rgb, NeuralColor.Load(int3(p10, 0)).rgb, frac_pos.x),
        lerp(NeuralColor.Load(int3(p01, 0)).rgb, NeuralColor.Load(int3(p11, 0)).rgb, frac_pos.x),
        frac_pos.y
    );

    // Apply high-frequency contrast-adaptive detail sharpening to the neural sweetspot
    if (Sharpness > 0.01) {
        int2 c_pos = clamp(int2(floor(neural_pos)), int2(1, 1), int2(NeuralSize) - 2);
        float3 c_n = NeuralColor.Load(int3(c_pos + int2(0, -1), 0)).rgb;
        float3 c_s = NeuralColor.Load(int3(c_pos + int2(0, 1), 0)).rgb;
        float3 c_w = NeuralColor.Load(int3(c_pos + int2(-1, 0), 0)).rgb;
        float3 c_e = NeuralColor.Load(int3(c_pos + int2(1, 0), 0)).rgb;
        float3 avg = (c_n + c_s + c_w + c_e) * 0.25;
        float3 highFreq = neural_rgb - avg;
        neural_rgb = saturate(neural_rgb + highFreq * (Sharpness * 2.0));
    }

    OutputColor[pixel] = float4(lerp(native_color.rgb, neural_rgb, weight), native_color.a);
}
)";

class FoveatedCompositePass {
public:
    static FoveatedCompositePass& Instance() {
        static FoveatedCompositePass instance;
        return instance;
    }

    bool Init(ID3D11Device* device) {
        if (isInitialized_ && device == device_) return true;
        device_ = device;

        ComPtr<ID3DBlob> shaderBlob;
        ComPtr<ID3DBlob> errorBlob;

        HMODULE hCompiler = LoadLibraryW(L"d3dcompiler_47.dll");
        if (!hCompiler) {
            HLX_ERROR("Failed to load d3dcompiler_47.dll");
            return false;
        }

        pD3DCompile pfnCompile = reinterpret_cast<pD3DCompile>(GetProcAddress(hCompiler, "D3DCompile"));
        if (!pfnCompile) {
            HLX_ERROR("Failed to get D3DCompile proc address");
            return false;
        }

        HRESULT hr = pfnCompile(
            g_foveatedCompositeHlsl,
            sizeof(g_foveatedCompositeHlsl) - 1,
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

        if (FAILED(hr)) {
            if (errorBlob) {
                HLX_ERROR("Foveated composite shader compilation error: %s",
                    reinterpret_cast<const char*>(errorBlob->GetBufferPointer()));
            }
            return false;
        }

        hr = device_->CreateComputeShader(
            shaderBlob->GetBufferPointer(),
            shaderBlob->GetBufferSize(),
            nullptr,
            &computeShader_
        );
        if (FAILED(hr)) {
            HLX_ERROR("Failed to create D3D11 compute shader: 0x%08X", hr);
            return false;
        }

        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = sizeof(FoveatedConstants);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = device_->CreateBuffer(&bd, nullptr, &constantBuffer_);
        if (FAILED(hr)) {
            HLX_ERROR("Failed to create constant buffer for foveated composite: 0x%08X", hr);
            return false;
        }

        isInitialized_ = true;
        HLX_INFO("FoveatedCompositePass initialized successfully (CS=5.0, CB=%zu bytes)", sizeof(FoveatedConstants));
        return true;
    }

    bool Execute(
        ID3D11DeviceContext* ctx,
        ID3D11ShaderResourceView* nativeSrv,
        ID3D11ShaderResourceView* neuralSrv,
        ID3D11UnorderedAccessView* outputUav,
        const FoveatedConstants& constants
    ) {
        if (!isInitialized_ || !ctx) return false;

        // 1. Update constant buffer
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = ctx->Map(constantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) return false;
        memcpy(mapped.pData, &constants, sizeof(FoveatedConstants));
        ctx->Unmap(constantBuffer_.Get(), 0);

        // 2. Bind pipeline state
        ID3D11ShaderResourceView* srvs[2] = { nativeSrv, neuralSrv };
        ID3D11Buffer* cbs[1] = { constantBuffer_.Get() };
        ID3D11UnorderedAccessView* uavs[1] = { outputUav };

        ctx->CSSetShader(computeShader_.Get(), nullptr, 0);
        ctx->CSSetConstantBuffers(0, 1, cbs);
        ctx->CSSetShaderResources(0, 2, srvs);
        ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

        // 3. Dispatch
        uint32_t dispatchX = (constants.outputWidth + 15) / 16;
        uint32_t dispatchY = (constants.outputHeight + 15) / 16;
        ctx->Dispatch(dispatchX, dispatchY, 1);

        // 4. Unbind resources to prevent hazard locks
        ctx->CSSetShader(nullptr, nullptr, 0);
        ID3D11Buffer* nullCbs[1] = { nullptr };
        ctx->CSSetConstantBuffers(0, 1, nullCbs);
        ID3D11ShaderResourceView* nullSrvs[2] = { nullptr, nullptr };
        ID3D11UnorderedAccessView* nullUavs[1] = { nullptr };
        ctx->CSSetShaderResources(0, 2, nullSrvs);
        ctx->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);

        return true;
    }

    void Release() {
        computeShader_.Reset();
        constantBuffer_.Reset();
        device_ = nullptr;
        isInitialized_ = false;
    }

private:
    FoveatedCompositePass() = default;
    ~FoveatedCompositePass() { Release(); }

    bool isInitialized_ = false;
    ID3D11Device* device_ = nullptr;
    ComPtr<ID3D11ComputeShader> computeShader_;
    ComPtr<ID3D11Buffer> constantBuffer_;
};

} // namespace hlx
